// clang-format off
#pragma once

#include <cstdint>

#include "vulkan/vulkan.h"

#include "rasm/types.h"
#include "rasm/camera.h"

namespace rasm
{

    struct Window
    {
        WindowHandle    handle;
        uint32_t        width;          // logical size, matches mouse coordinates
        uint32_t        height;
        uint32_t        pixelWidth;     // drawable size in pixels, differs from width/height on HiDPI displays
        uint32_t        pixelHeight;
        bool            mouseDragActive;
        bool            resized;
        bool            minimized;
        glm::vec2       lastMousePos = glm::vec2(0.0f);
    };

}
// clang-format on