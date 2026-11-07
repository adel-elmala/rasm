// clang-format off
#pragma once

#include "rasm/engine.h"
#include "rasm/handle.h"
#include "rasm/types.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace rasm
{

    struct StatsFrameSlot
    {
        QueryPoolHandle                         timestamps;
        BufferHandle                            text;
        std::string                             timerNames[MAX_GPU_TIMERS];
        uint32_t                                timerCount          = 0;
        uint64_t                                readFenceValue      = 0;
        uint32_t                                columns             = 0;
        uint32_t                                rows                = 0;
    };

    struct StatsOverlay
    {
        StatsFrameSlot                          slots[MAX_FRAMES_IN_FLIGHT];
        FrameStats                              stats;
        PipelineHandle                          pipeline;
        uint32_t                                frameTimer          = UINT32_MAX;
        uint32_t                                passTimer           = UINT32_MAX;
        std::string                             grid;
        uint32_t                                columns             = 0;
        uint32_t                                rows                = 0;
        std::chrono::steady_clock::time_point   gridTime            = {};
    };

}
// clang-format on
