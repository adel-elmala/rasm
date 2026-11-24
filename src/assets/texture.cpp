#include "rasm/renderer.h"

#include "assets/ktxImage.h"
#include "assets/loadJobs.h"
#include "core/workerPool.h"
#include "platform/paths.h"
#include "renderer/gpuScene.h"
#include "rhi/device.h"
#include "rhi/mipChain.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "stb_image.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace rasm
{
    namespace
    {
        const std::unordered_set<std::string> supportedTextureExtensions = {"png", "jpg", "jpeg", "ktx2"};

        // Runs on a worker thread: it must not touch the engine. The mips are made here too, so the main thread only
        // copies the levels (P2).
        TextureRaw decodeTexture(const std::string &path, Format format, uint32_t levelCount)
        {
            ZoneScoped;

            int width, height, nChannels;
            unsigned char *data = stbi_load(path.c_str(), &width, &height, &nChannels, 4);
            if (!data)
                return {};
            TextureRaw raw;
            if (!rhi::buildTextureLevels(data, static_cast<uint32_t>(width), static_cast<uint32_t>(height), format, false, levelCount, raw))
                spdlog::error("Failed to make the mips of {}.", path);
            stbi_image_free(data);
            return raw;
        }

        // Runs on a worker thread: reads the file and transcodes it, or takes its levels, in the format the main thread
        // picked when it read the header.
        TextureRaw decodeKtxFile(const std::string &path, Format format)
        {
            ZoneScoped;

            std::ifstream file(pathFromUtf8(path), std::ios::binary | std::ios::ate);
            std::vector<unsigned char> bytes(file ? static_cast<size_t>(file.tellg()) : 0);
            file.seekg(0);
            if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                bytes.clear();
            TextureRaw raw;
            std::string error;
            if (bytes.empty() || !decodeKtx(bytes.data(), bytes.size(), format, raw, error))
            {
                spdlog::error("Failed to decode KTX2 texture {}: {}", path, bytes.empty() ? "the file can't be read" : error);
                return {};
            }
            return raw;
        }
    }

    TextureHandle Renderer::loadTexture(const std::string &assetPath, Format format)
    {
        if (format != Format::R8G8B8A8_SRGB && format != Format::R8G8B8A8_UNORM)
        {
            spdlog::error("loadTexture takes R8G8B8A8_SRGB for a color slot or R8G8B8A8_UNORM for a data slot.");
            return {};
        }
        const std::string path = resolveAssetPath(assetPath);
        const auto cacheKey = path + "#" + std::to_string(static_cast<int>(format));
        // A texture destroyed through RenderContext directly leaves its entry; it is loaded again then.
        if (auto it = registry.loadedTextures.find(cacheKey); it != registry.loadedTextures.end())
        {
            if (ctx.isAlive(it->second))
                return it->second;
            registry.loadedTextures.erase(it);
        }

        // find if the texture extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of('.') + 1);
        if (!supportedTextureExtensions.contains(extension))
        {
            spdlog::error("Unsupported texture format: {}", extension);
            return TextureHandle{};
        }

        // Only the header is read here, so the texture exists (and can go into a material) before its pixels do.
        TextureDesc desc = {.name = path, .format = format, .usage = TextureUsage::SAMPLED};
        if (extension == "ktx2")
        {
            KtxInfo info;
            std::string error;
            if (!readKtxInfo(path, info, error))
            {
                spdlog::error("Failed to load KTX2 texture {}: {}", path, error);
                return TextureHandle{};
            }
            desc.format = chooseKtxFormat(info, format == Format::R8G8B8A8_SRGB ? KtxSlot::COLOR : KtxSlot::CHANNELS, getSupportedTextureFormats(ctx));
            if (desc.format == Format::UNKNOWN)
            {
                spdlog::error("Failed to load KTX2 texture {}: the device can't sample its format.", path);
                return TextureHandle{};
            }
            warnAboutKtxFormat(path, info, desc.format);
            desc.width = info.width;
            desc.height = info.height;
            desc.mipLevels = getKtxMipCount(info, desc.format);
        }
        else
        {
            int width, height, nChannels;
            if (!stbi_info(path.c_str(), &width, &height, &nChannels))
            {
                spdlog::error("Failed to load texture: {}", path);
                return TextureHandle{};
            }
            desc.width = static_cast<uint32_t>(width);
            desc.height = static_cast<uint32_t>(height);
            desc.mipLevels = rhi::getFullMipCount(desc.width, desc.height);
        }

        auto textureHandle = ctx.createTexture(desc);
        if (!textureHandle.isValid())
        {
            spdlog::error("Failed to create texture: {}", path);
            return TextureHandle{};
        }

        registry.loadedTextures[cacheKey] = textureHandle;
        registry.textureCacheKeys.resize(std::max<size_t>(registry.textureCacheKeys.size(), textureHandle.index + 1));
        registry.textureCacheKeys[textureHandle.index] = cacheKey;
        // The header gives the size of every level, so the budget knows what the decode will hold before it starts: the
        // levels, and while the job runs an image's decoded pixels or a KTX2 file's bytes.
        std::error_code error;
        const uint64_t fileBytes = std::filesystem::file_size(pathFromUtf8(path), error);
        uint64_t levelBytes = extension == "ktx2" ? (error ? 0 : fileBytes) : static_cast<uint64_t>(desc.width) * desc.height * 4;
        for (uint32_t level = 0; level < desc.mipLevels; ++level)
            levelBytes += rhi::getTextureLevelSize(desc.format, std::max(1u, desc.width >> level), std::max(1u, desc.height >> level));
        auto job = std::make_shared<TextureLoadJob>();
        WorkerPool *pool = workers.get();
        auto decode = [job, pool, path, ktx = extension == "ktx2", format = desc.format, levelCount = desc.mipLevels]()
        {
            if (!job->ticket.cancelled)
                job->raw = ktx ? decodeKtxFile(path, format) : decodeTexture(path, format, levelCount);
            pool->setLoadBytes(job->ticket, job->raw.levels.size());
            pool->finishLoad(job->ticket);
        };
        pool->push({.run = std::move(decode), .bytes = levelBytes, .ticket = std::shared_ptr<LoadTicket>(job, &job->ticket)});
        pendingTextures.emplace(textureHandle, PendingTextureLoad{.handle = textureHandle, .path = path, .job = std::move(job)});
        return textureHandle;
    }

    bool Renderer::isLoaded(TextureHandle texture) const
    {
        return ctx.isAlive(texture) && !pendingTextures.contains(texture);
    }

    // Each material that uses the texture loses it, and its row is written again.
    void Renderer::detachTexture(const TextureHandle &texture)
    {
        if (!texture.isValid())
            return;
        bool changed = false;
        for (uint64_t i = 1; i < registry.materials.size(); ++i)
        {
            Material &material = registry.materials[i];
            if (!isLive(material.handle))
                continue;
            bool used = false;
            for (TextureHandle &slot : material.textures)
            {
                if (slot == texture)
                {
                    slot = {};
                    used = true;
                }
            }
            if (used)
            {
                writeMaterialRow(material.handle);
                changed = true;
            }
        }
        // An entity that waited for this texture may be drawable now.
        if (changed)
            gpuScene->texturesFinished = true;
    }

    // Materials sample by slot number, so each one that uses the texture loses it before the slot can take another
    // texture. A load still in the pool is dropped: a queued job never runs, a running one gives its result up.
    void Renderer::destroyTexture(const TextureHandle &texture)
    {
        if (!ctx.isAlive(texture))
        {
            spdlog::error("destroyTexture: texture handle {{{}, {}}} is invalid or was destroyed.", texture.index, texture.generation);
            return;
        }

        detachTexture(texture);
        dropTextureLoad(texture);

        if (texture.index < registry.textureCacheKeys.size() && !registry.textureCacheKeys[texture.index].empty())
        {
            registry.loadedTextures.erase(registry.textureCacheKeys[texture.index]);
            registry.textureCacheKeys[texture.index].clear();
        }
        ctx.destroyTexture(texture);
    }

    // The load's bytes go back to the budget once a worker freed its data, from here or once it is filled (finishLoads).
    void Renderer::dropTextureLoad(TextureHandle texture)
    {
        auto it = pendingTextures.find(texture);
        if (it == pendingTextures.end())
            return;
        if (it->second.job)
        {
            LoadTicket &ticket = it->second.job->ticket;
            dropPendingLoad(ticket, std::move(it->second.job));
        }
        else
            freeOnWorker(std::move(it->second.levels), it->second.bytes);
        pendingTextures.erase(it);
    }

    bool Renderer::fillTextureLevels(TextureHandle texture, const TextureRaw &raw)
    {
        TextureFillProgress progress;
        return fillTextureLevels(texture, raw, progress, UINT64_MAX);
    }

    bool Renderer::fillTextureLevels(TextureHandle texture, const TextureRaw &raw, TextureFillProgress &progress, uint64_t maxBytes)
    {
        std::vector<TextureLevel> levels(raw.levelSizes.size());
        uint64_t offset = 0;
        for (size_t level = 0; level < levels.size(); ++level)
        {
            levels[level] = {.data = raw.levels.data() + offset, .size = raw.levelSizes[level]};
            offset += raw.levelSizes[level];
        }
        return ctx.fillTexturePart(texture, levels.data(), static_cast<uint32_t>(levels.size()), progress, maxBytes);
    }

    // The levels arrive whole, mips included, so this only copies them: on the host, or into the staging ring, about
    // maxBytes per call, so a big texture's copies spread over frames (P24). They are freed when the load is erased (P6).
    // Returns whether the fill is over: its last part queued, failed, or its texture gone.
    bool Renderer::finishTextureLoad(PendingTextureLoad &load, uint64_t maxBytes)
    {
        ZoneScoped;

        const TextureRaw &raw = load.levels ? *load.levels : load.job->raw;

        // The texture was destroyed through RenderContext while it decoded, so there is nothing to fill.
        if (!ctx.isAlive(load.handle))
            return true;
        const TextureDesc &desc = ctx.getTextureDesc(load.handle);

        // A material may already sample the texture, so a failed decode or upload leaves it plain (white, or a flat
        // normal for a two-channel format) instead of undefined: an image makes its mips from white pixels, a block
        // format gets every level of constant blocks.
        const bool decoded = !raw.levelSizes.empty() && raw.format == desc.format && raw.width == desc.width && raw.height == desc.height;
        if (!decoded)
            spdlog::error("Failed to decode texture {}; it stays plain white.", load.path);
        const bool filled = decoded && fillTextureLevels(load.handle, raw, load.progress, maxBytes);
        if (filled)
            return load.progress.done;
        // A glTF texture's role is its material slot, where white would be a tilted normal or full metal: the materials
        // drop it instead, and read their defaults.
        if (load.levels)
        {
            spdlog::error("Failed to fill texture {}; its materials go without it.", load.path);
            destroyTexture(load.handle);
            return true;
        }
        if (decoded)
            spdlog::error("Failed to fill texture {}; it stays plain white.", load.path);
        if (desc.format == Format::R8G8B8A8_SRGB || desc.format == Format::R8G8B8A8_UNORM)
        {
            const std::vector<unsigned char> white(static_cast<size_t>(desc.width) * desc.height * 4, 255);
            ctx.fillTexture(load.handle, white.data());
            return true;
        }
        TextureRaw blank;
        makeBlankKtxLevels(desc.format, desc.width, desc.height, desc.mipLevels, blank);
        fillTextureLevels(load.handle, blank);
        return true;
    }
}
