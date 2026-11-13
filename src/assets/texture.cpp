#include "rasm/engine.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "stb_image.h"

#include <future>
#include <string>
#include <unordered_set>
#include <vector>

namespace rasm
{
    namespace
    {
        const std::unordered_set<std::string> supportedTextureExtensions = {"png", "jpg", "jpeg"};

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
    }

    TextureHandle Engine::loadTexture(const std::string &assetPath, Format format)
    {
        if (format != Format::R8G8B8A8_SRGB && format != Format::R8G8B8A8_UNORM)
        {
            spdlog::error("loadTexture requires RGBA8 sRGB or UNORM format.");
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
        int width, height, nChannels;
        if (!stbi_info(path.c_str(), &width, &height, &nChannels))
        {
            spdlog::error("Failed to load texture: {}", path);
            return TextureHandle{};
        }

        auto textureHandle = ctx.createTexture({.name = path, .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height), .format = format, .usage = TextureUsage::SAMPLED});
        if (!textureHandle.isValid())
        {
            spdlog::error("Failed to create texture: {}", path);
            return TextureHandle{};
        }

        registry.loadedTextures[cacheKey] = textureHandle;
        pendingTextures.emplace(textureHandle, PendingTextureLoad{textureHandle, path, std::async(std::launch::async, decodeTexture, path)});
        return textureHandle;
    }

    bool Engine::isLoaded(TextureHandle texture) const
    {
        return texture.isValid() && !pendingTextures.contains(texture);
    }

    void Engine::finishTextureLoad(PendingTextureLoad &load)
    {
        ZoneScoped;

        TextureRaw raw = load.result.get();
        const TextureDesc desc = ctx.getTextureDesc(load.handle);

        // The app destroyed the texture while it decoded, so there is nothing to fill.
        if (desc.width == 0 || desc.height == 0)
        {
            stbi_image_free(raw.data);
            return;
        }

        // A material may already sample the texture, so a failed decode leaves it white instead of undefined.
        if (!raw.data || raw.width != desc.width || raw.height != desc.height)
        {
            spdlog::error("Failed to decode texture {}; it stays white.", load.path);
            std::vector<unsigned char> white(static_cast<size_t>(desc.width) * desc.height * 4, 255);
            ctx.fillTexture(load.handle, white.data());
        }
        else if (!ctx.fillTexture(load.handle, raw.data))
        {
            spdlog::error("Failed to fill texture: {}", load.path);
        }

        // fillTexture copies on the host (host image copy), so the pixels are not needed once it returns (P6).
        stbi_image_free(raw.data);
    }
}
