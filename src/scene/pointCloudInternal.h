// clang-format off
#pragma once

#include "glm/glm.hpp"

#include <cstdint>

namespace rasm
{

    constexpr uint32_t  MORTON_LEVELS       = 21;               // bits per axis of a 63-bit Morton code

    uint64_t            spreadMortonBits(uint32_t v);           // the 21 bits of v to every third bit of 63
    uint32_t            compactMortonBits(uint64_t x);          // every third bit of x, from bit 0, back to 21 bits
    float               measurePointSpacing(const glm::vec3* positions, uint32_t count);

}
// clang-format on
