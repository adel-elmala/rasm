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
    VkImageAspectFlags  toVkAspectMask(TextureUsage usage);
    const char*         toVkResultString(VkResult result);
    bool                hasStencil(Format format);
}
// clang-format on
