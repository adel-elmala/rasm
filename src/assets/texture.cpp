#include "rasm/engine.h"

#include "assets/ktxImage.h"
#include "platform/paths.h"
#include "renderer/gpuScene.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "stb_image.h"

#include <algorithm>
#include <fstream>
#include <future>
#include <string>
#include <unordered_set>
#include <vector>

namespace rasm
{
    namespace
    {
        const std::unordered_set<std::string> supportedTextureExtensions = {"png", "jpg", "jpeg", "ktx2"};

        // Runs on a worker thread: it must not touch the engine.
        TextureRaw decodeTexture(const std::string &path)
        {
            ZoneScoped;

            int width, height, nChannels;
            unsigned char *data = stbi_load(path.c_str(), &width, &height, &nChannels, 4);
            if (!data)
                return {};
            return {.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height), .data = data};
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

    TextureHandle Engine::loadTexture(const std::string &assetPath, Format format)
    {
        if (format != Format::R8G8B8A8_SRGB && format != Format::R8G8B8A8_UNORM)
        {
            spdlog::error("loadTexture takes R8G8B8A8_SRGB for a color slot or R8G8B8A8_UNORM for a data slot.");
            return {};
        }
        const std::string path = resolveAssetPath(assetPath);
        const auto cacheKey = path + "#" + std::to_string(static_cast<int>(format));
        if (registry.loadedTextures.find(cacheKey) != registry.loadedTextures.end())
        {
            return registry.loadedTextures[cacheKey];
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
        auto result = extension == "ktx2" ? std::async(std::launch::async, decodeKtxFile, path, desc.format) : std::async(std::launch::async, decodeTexture, path);
        pendingTextures.emplace(textureHandle, PendingTextureLoad{textureHandle, path, std::move(result)});
        return textureHandle;
    }

    bool Engine::isLoaded(TextureHandle texture) const
    {
        return ctx.isAlive(texture) && !pendingTextures.contains(texture);
    }

    // Materials sample by slot number, so each one that uses the texture loses it before the slot can take another
    // texture. A load still decoding is dropped when it finishes (finishTextureLoad).
    void Engine::destroyTexture(const TextureHandle &texture)
    {
        if (!ctx.isAlive(texture))
        {
            spdlog::error("destroyTexture: texture handle {{{}, {}}} is invalid or was destroyed.", texture.index, texture.generation);
            return;
        }

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

        if (texture.index < registry.textureCacheKeys.size() && !registry.textureCacheKeys[texture.index].empty())
        {
            registry.loadedTextures.erase(registry.textureCacheKeys[texture.index]);
            registry.textureCacheKeys[texture.index].clear();
        }
        ctx.destroyTexture(texture);
    }

    bool Engine::fillTextureLevels(TextureHandle texture, const TextureRaw &raw)
    {
        std::vector<TextureLevel> levels(raw.levelSizes.size());
        uint64_t offset = 0;
        for (size_t level = 0; level < levels.size(); ++level)
        {
            levels[level] = {.data = raw.levels.data() + offset, .size = raw.levelSizes[level]};
            offset += raw.levelSizes[level];
        }
        return ctx.fillTexture(texture, levels.data(), static_cast<uint32_t>(levels.size()));
    }

    void Engine::finishTextureLoad(PendingTextureLoad &load)
    {
        ZoneScoped;

        TextureRaw raw = load.result.get();

        // The app destroyed the texture while it decoded, so there is nothing to fill.
        if (!ctx.isAlive(load.handle))
        {
            stbi_image_free(raw.data);
            return;
        }
        const TextureDesc desc = ctx.getTextureDesc(load.handle);

        // A material may already sample the texture, so a failed decode or upload leaves it plain (white, or a flat
        // normal for a two-channel format) instead of undefined: an image makes its mips from white pixels, a KTX2
        // texture gets every level of constant blocks.
        const bool ktx = desc.mipLevels > 0;
        const bool decoded = ktx ? !raw.levelSizes.empty() && raw.format == desc.format && raw.width == desc.width && raw.height == desc.height
                                 : raw.data && raw.width == desc.width && raw.height == desc.height;
        if (!decoded)
            spdlog::error("Failed to decode texture {}; it stays plain white.", load.path);
        const bool filled = decoded && (ktx ? fillTextureLevels(load.handle, raw) : ctx.fillTexture(load.handle, raw.data));
        if (decoded && !filled)
            spdlog::error("Failed to fill texture {}; it stays plain white.", load.path);
        if (!filled && ktx)
        {
            TextureRaw blank;
            makeBlankKtxLevels(desc.format, desc.width, desc.height, desc.mipLevels, blank);
            fillTextureLevels(load.handle, blank);
        }
        else if (!filled)
        {
            std::vector<unsigned char> white(static_cast<size_t>(desc.width) * desc.height * 4, 255);
            ctx.fillTexture(load.handle, white.data());
        }

        // fillTexture copies the pixels (on the host, or into the staging ring), so they are not needed once it returns (P6).
        stbi_image_free(raw.data);
    }
}
