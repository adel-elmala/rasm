// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/types.h"

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
        float                                   gridAge             = 0.0f;     // seconds of frame time since the grid was built
    };

}
// clang-format on
