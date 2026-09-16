// clang-format off
#pragma once

#include <cstdint>

#include "vulkan/vulkan.h"

#include "rasm/core/types.h"
#include "rasm/core/camera.h"

namespace rasm
{

    // TODO: delete this entire file, not needed anymore
    struct Window
    {
        struct Extent
        {
            uint32_t width;
            uint32_t height;
        };

        Extent extent{1080, 720}; 
    };

}
// clang-format on