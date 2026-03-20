// clang-format off
#pragma once

#include "VkBootstrap.h"
#include "vk_mem_alloc.h"

#include "rasm/core/types.h"
namespace rasm::gfx
{
    struct BufferVKHandle
    {
        ResourceDesc desc;
        BufferHandle handle;
        VkBuffer buffer;
        VmaAllocation allocation;
    };

    struct TextureVKHandle
    {
        ResourceDesc desc;
        TextureHandle handle;
        VkImage image;
        VkImageView view;
        VmaAllocation allocation;
    };

    struct ShaderVKHandle
    {
        ResourceDesc desc;
        ShaderHandle handle;
        VkShaderModule module;
    };

    struct PipelineVKHandle
    {
        ResourceDesc desc;
        PipelineHandle handle;
        VkPipeline pipeline;
        VkPipelineLayout layout;
    };
}
// clang-format on