// clang-format off
#pragma once

#include "rasm/render/resource.h"

#include <cstdint>
#include <vector>

namespace rasm::rhi
{
    uint32_t        getFullMipCount(uint32_t width, uint32_t height);

    bool            buildMipChain(const unsigned char* rgba, uint32_t width, uint32_t height, bool srgb, uint32_t levelCount,
                                  std::vector<unsigned char>& storage, TextureLevel* levels);
}
// clang-format on
