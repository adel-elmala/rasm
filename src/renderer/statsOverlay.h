// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/types.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace rasm
{

    struct StatsOverlaySlot
    {
        BufferHandle                            text;
        uint32_t                                columns             = 0;
        uint32_t                                rows                = 0;
    };

    struct StatsOverlay
    {
        StatsOverlaySlot                        slots[MAX_FRAMES_IN_FLIGHT];
        PipelineHandle                          pipeline;
        std::string                             grid;
        uint32_t                                columns             = 0;
        uint32_t                                rows                = 0;
        std::chrono::steady_clock::time_point   gridTime            = {};
    };

}
// clang-format on
