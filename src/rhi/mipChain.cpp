#include "rhi/mipChain.h"

#include "stb_image_resize2.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rasm::rhi
{
    namespace
    {
        uint64_t getChainBytes(uint32_t width, uint32_t height, uint32_t firstLevel, uint32_t levelCount)
        {
            uint64_t total = 0;
            for (uint32_t level = firstLevel; level < levelCount; ++level)
                total += static_cast<uint64_t>(std::max(1u, width >> level)) * std::max(1u, height >> level) * 4;
            return total;
        }

        // A mip of a normal map averages unit vectors into shorter ones; each texel is made unit length again, so the
        // map stores directions at every level. Alpha is left as it is.
        void renormalize(unsigned char *rgba, uint64_t texels)
        {
            float decode[256];
            for (uint32_t value = 0; value < 256; ++value)
                decode[value] = static_cast<float>(value) / 127.5f - 1.0f;
            for (uint64_t i = 0; i < texels; ++i)
            {
                unsigned char *texel = rgba + i * 4;
                const float x = decode[texel[0]];
                const float y = decode[texel[1]];
                const float z = decode[texel[2]];
                const float lengthSquared = x * x + y * y + z * z;
                if (lengthSquared < 1e-8f)
                    continue;
                // (n + 1) * 127.5 rounded; a unit n keeps it in [0, 255].
                const float scale = 127.5f / std::sqrt(lengthSquared);
                texel[0] = static_cast<unsigned char>(x * scale + 128.0f);
                texel[1] = static_cast<unsigned char>(y * scale + 128.0f);
                texel[2] = static_cast<unsigned char>(z * scale + 128.0f);
            }
        }

        // Each level is made from the one before, into next. sRGB color is filtered in linear light and weighted by
        // alpha, so a transparent texel adds no color; data is averaged as it is.
        bool resizeChain(const unsigned char *rgba, uint32_t width, uint32_t height, bool srgb, bool normals, uint32_t levelCount, unsigned char *next, TextureLevel *levels)
        {
            levels[0] = {.data = rgba, .size = static_cast<uint64_t>(width) * height * 4};
            const unsigned char *source = rgba;
            for (uint32_t level = 1; level < levelCount; ++level)
            {
                const uint32_t nextWidth = std::max(1u, width / 2);
                const uint32_t nextHeight = std::max(1u, height / 2);
                const unsigned char *resized = srgb ? stbir_resize_uint8_srgb(source, static_cast<int>(width), static_cast<int>(height), 0, next, static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0, STBIR_RGBA)
                                                    : stbir_resize_uint8_linear(source, static_cast<int>(width), static_cast<int>(height), 0, next, static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0, STBIR_4CHANNEL);
                if (!resized)
                    return false;
                levels[level] = {.data = next, .size = static_cast<uint64_t>(nextWidth) * nextHeight * 4};
                if (normals)
                    renormalize(next, static_cast<uint64_t>(nextWidth) * nextHeight);
                source = next;
                next += levels[level].size;
                width = nextWidth;
                height = nextHeight;
            }
            return true;
        }
    }

    uint32_t getFullMipCount(uint32_t width, uint32_t height)
    {
        uint32_t count = 1;
        for (uint32_t size = std::max(width, height); size > 1; size /= 2)
            ++count;
        return count;
    }

    bool buildMipChain(const unsigned char *rgba, uint32_t width, uint32_t height, bool srgb, uint32_t levelCount, std::vector<unsigned char> &storage, TextureLevel *levels)
    {
        // Sized once, so the level pointers stay valid.
        storage.resize(getChainBytes(width, height, 1, levelCount));
        return resizeChain(rgba, width, height, srgb, false, levelCount, storage.data(), levels);
    }

    // The whole chain in one block, level 0 first, as fillTexture takes it: a worker makes it, so the main thread only
    // copies (P2).
    bool buildTextureLevels(const unsigned char *rgba, uint32_t width, uint32_t height, Format format, bool normals, uint32_t levelCount, TextureRaw &raw)
    {
        ZoneScoped;

        raw = {.width = width, .height = height, .format = format};
        raw.levels.resize(getChainBytes(width, height, 0, levelCount));
        std::memcpy(raw.levels.data(), rgba, static_cast<size_t>(width) * height * 4);
        std::vector<TextureLevel> levels(levelCount);
        if (!resizeChain(raw.levels.data(), width, height, format == Format::R8G8B8A8_SRGB, normals, levelCount, raw.levels.data() + static_cast<size_t>(width) * height * 4, levels.data()))
        {
            raw = {};
            return false;
        }
        raw.levelSizes.resize(levelCount);
        for (uint32_t level = 0; level < levelCount; ++level)
            raw.levelSizes[level] = levels[level].size;
        return true;
    }
}
