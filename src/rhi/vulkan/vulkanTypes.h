// clang-format off
#pragma once

#include "rhi/device.h"

#include "vk_mem_alloc.h"

#include <cstdint>

namespace rasm::rhi
{
    struct VulkanBuffer : Buffer
    {
        VkBuffer                buffer          = VK_NULL_HANDLE;
        VmaAllocation           allocation      = nullptr;
        void*                   mappedData      = nullptr;
    };

    struct VulkanTexture : Texture
    {
        VkImage                 image           = VK_NULL_HANDLE;
        VkImageView             view            = VK_NULL_HANDLE;
        VmaAllocation           allocation      = nullptr;
        uint32_t                mipLevels       = 1;
    };

    struct VulkanHeap : Heap
    {
        VmaAllocation           allocation      = nullptr;
    };

    struct VulkanSampler : Sampler
    {
        VkSampler               sampler         = VK_NULL_HANDLE;
    };

    struct VulkanShader : Shader
    {
        VkShaderModule          module          = VK_NULL_HANDLE;
    };

    struct VulkanPipeline : Pipeline
    {
        VkPipeline              pipeline        = VK_NULL_HANDLE;
        VkPipelineLayout        layout          = VK_NULL_HANDLE;
        VkPipelineBindPoint     bindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    };

    struct VulkanDescriptorSetLayout : DescriptorSetLayout
    {
        VkDescriptorSetLayout   layout          = VK_NULL_HANDLE;
    };

    struct VulkanDescriptorPool : DescriptorPool
    {
        VkDescriptorPool        pool            = VK_NULL_HANDLE;
    };

    struct VulkanDescriptorSet : DescriptorSet
    {
        VkDescriptorSet         set             = VK_NULL_HANDLE;
    };

    struct VulkanFence : Fence
    {
        VkSemaphore             semaphore       = VK_NULL_HANDLE;
    };

    struct VulkanQueryPool : QueryPool
    {
        VkQueryPool             pool            = VK_NULL_HANDLE;
    };
}
// clang-format on
