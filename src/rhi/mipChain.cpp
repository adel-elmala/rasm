#include "rhi/mipChain.h"

#include "stb_image_resize2.h"

#include <algorithm>

namespace rasm::rhi
{
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
        size_t total = 0;
        for (uint32_t level = 1, w = width, h = height; level < levelCount; ++level)
        {
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
            total += static_cast<size_t>(w) * h * 4;
        }
        storage.resize(total);

        levels[0] = {.data = rgba, .size = static_cast<uint64_t>(width) * height * 4};
        const unsigned char *source = rgba;
        unsigned char *next = storage.data();
        for (uint32_t level = 1; level < levelCount; ++level)
        {
            const uint32_t nextWidth = std::max(1u, width / 2);
            const uint32_t nextHeight = std::max(1u, height / 2);
            const unsigned char *resized = srgb ? stbir_resize_uint8_srgb(source, static_cast<int>(width), static_cast<int>(height), 0, next, static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0, STBIR_RGBA)
                                                : stbir_resize_uint8_linear(source, static_cast<int>(width), static_cast<int>(height), 0, next, static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0, STBIR_4CHANNEL);
            if (!resized)
                return false;
            levels[level] = {.data = next, .size = static_cast<uint64_t>(nextWidth) * nextHeight * 4};
            source = next;
            next += levels[level].size;
            width = nextWidth;
            height = nextHeight;
        }
        return true;
    }
}
