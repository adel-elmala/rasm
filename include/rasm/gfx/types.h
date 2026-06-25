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
        VmaAllocationInfo allocationInfo;
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

    struct CommandPoolVKHandle
    {
        CommandPoolHandle handle;
        VkCommandPool commandPool;
    };

    struct CommandBufferVKHandle
    {
        CommandBufferHandle handle;
        VkCommandBuffer commandBuffer;
    };

    struct SemaphoreVKHandle
    {
        enum class Type
        {
            BINARY,
            TIMELINE
        } type;
        SemaphoreHandle handle;
        VkSemaphore semaphore;
    };

    struct FenceVKHandle
    {
        FenceHandle handle;
        VkFence fence;
    };

    struct SwapchainVKHandle
    {
        SwapchainHandle handle;
        vkb::Swapchain swapchain;
        std::vector<TextureVKHandle> images;
        Format imageFormat;
        uint32_t imageCount;
    };
}
// clang-format on
