// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/render/resource.h"

#include "vulkan/vulkan.h"

namespace rasm::rhi
{
    VkFormat            toVkFormat(Format format);
    Format              toFormat(VkFormat format);
    VkShaderStageFlags  toVkShaderStageFlags(ShaderType stage);
    VkImageAspectFlags  toVkAspectMask(Format format);
    const char*         toVkResultString(VkResult result);
    bool                hasStencil(Format format);
    uint32_t            getHostCopyMemoryTypes(const VkPhysicalDeviceMemoryProperties& memory, uint32_t hostTransferTypes, uint32_t copyTypes);
}
// clang-format on
