// clang-format off
#pragma once

#include "rasm/types.h"

#include "volk.h"

namespace rasm::rhi
{

    bool    createVulkanSurface(const NativeWindow& window, VkInstance instance, VkSurfaceKHR* surface);

}
// clang-format on
