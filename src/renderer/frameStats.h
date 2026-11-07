// clang-format off
#pragma once

#include "rasm/engine.h"
#include "rasm/handle.h"
#include "rasm/types.h"

#include <cstdint>
#include <string>

namespace rasm
{

    struct FrameStatsSlot
    {
        QueryPoolHandle                         timestamps;
        std::string                             timerNames[MAX_GPU_TIMERS];
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
    };

}
// clang-format on
