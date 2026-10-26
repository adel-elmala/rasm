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
        uint32_t        width;
        uint32_t        height;
        bool            mouseDragActive;
        bool            resized;
        glm::vec2       lastMousePos = glm::vec2(0.0f);
    };

}
// clang-format on