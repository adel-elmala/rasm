// clang-format off
#pragma once

#include "glm/glm.hpp"

#include <cstdint>

namespace rasm
{
    // The down chain's levels, each half the one before: 1/2 to 1/64 of the source.
    constexpr uint32_t          BLOOM_LEVELS            = 6;
    // A level smaller than this on a side ends the chain: halving it again would only repeat it.
    constexpr uint32_t          BLOOM_MIN_LEVEL_SIZE    = 2;

    // Level 1 to BLOOM_LEVELS of a source of a fixed size: the size halved level times, at least 1 x 1.
    glm::uvec2                  getBloomLevelSize(glm::uvec2 sourceSize, uint32_t level);
    // The levels a frame runs: those from level 1 on that are at least BLOOM_MIN_LEVEL_SIZE on both sides, and at least
    // level 1. levelSizes[i] is level i + 1.
    uint32_t                    getBloomLevelCount(const glm::uvec2* levelSizes, uint32_t count);
}
// clang-format on
