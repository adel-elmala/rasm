// clang-format off
#pragma once

#include <cstdint>

namespace rasm
{

    constexpr uint32_t          BLEND_SORT_BLOCK                = 1024;
    constexpr uint32_t          BLEND_SORT_GROUP_SIZE           = 256;
    constexpr uint32_t          MAX_BLEND_SORT_DISPATCHES       = 256;

    enum class BlendSortStep : uint32_t
    {
        KEYS,
        MERGE,
        LOCAL_MERGE,
        WRITE,
    };

    constexpr uint32_t          BLEND_SORT_STEPS                = 4;

    struct BlendSortDispatch
    {
        BlendSortStep           step                            = BlendSortStep::KEYS;
        uint32_t                argument                        = 0;
        uint32_t                groups                          = 0;
    };

    uint32_t                    getBlendSortCapacity(uint32_t drawCount);
    uint32_t                    getBlendSortDispatches(uint32_t capacity, BlendSortDispatch* dispatches, uint32_t maxDispatches);

}
// clang-format on
