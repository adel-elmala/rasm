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

    struct FrameStatsSlot
    {
        QueryPoolHandle                         timestamps;
        std::string                             timerNames[MAX_GPU_TIMERS];
        bool                                    timerOnCompute[MAX_GPU_TIMERS]  = {};
        uint32_t                                timerCount          = 0;
        uint64_t                                readFenceValue      = 0;
        bool                                    measuring           = false;    // the slot's last frame was measured
    };

    struct FrameStatsCollector
    {
        FrameStatsSlot                          slots[MAX_FRAMES_IN_FLIGHT];
        FrameStats                              stats;
        uint32_t                                frameTimer          = UINT32_MAX;
        uint32_t                                passTimer           = UINT32_MAX;
        bool                                    requested           = false;    // getFrameStats was called
        bool                                    timersFullLogged    = false;
        bool                                    lightsDroppedLogged = false;
        std::chrono::steady_clock::time_point   lightsDroppedTime   = {};
    };

    // 1234, 12.3K, 1.23M: a count short enough for the stats overlay and the RASM window.
    std::string                                 formatCount(uint64_t count);

}
// clang-format on
