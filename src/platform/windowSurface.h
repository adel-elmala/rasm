// clang-format off
#pragma once

#include "rasm/window.h"

#include "vulkan/vulkan.h"

namespace rasm
{

    bool createWindowSurface(const Window& window, VkInstance instance, VkSurfaceKHR* surface);

}
// clang-format on
