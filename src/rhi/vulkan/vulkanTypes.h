// clang-format off
#pragma once

#include "rhi/device.h"

// volk first: vk_mem_alloc.h declares vmaImportVulkanFunctionsFromVolk only after it.
#include "volk.h"
#include "vk_mem_alloc.h"

#include <cstdint>

namespace rasm::rhi
{
    // A full chain of the largest texture any GPU allows (32768) has 16 levels.
    constexpr uint32_t              MAX_TEXTURE_MIPS        = 16;
    constexpr VkShaderStageFlags    PUSH_CONSTANT_STAGES    = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    constexpr VkShaderStageFlags    MESH_SHADER_STAGES      = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;

    struct VulkanBuffer : Buffer
    {
        VkBuffer                buffer          = VK_NULL_HANDLE;
        VmaAllocation           allocation      = nullptr;
        void*                   mappedData      = nullptr;
    };

    struct VulkanTexture : Texture
    {
        VkImage                 image                       = VK_NULL_HANDLE;
        VkImageView             view                        = VK_NULL_HANDLE;   // a cube's view samples all its faces
        VkImageView             faceViews[CUBE_FACES]       = {};               // a cube's attachments, one face each
        VkImageView             mipViews[MAX_TEXTURE_MIPS]  = {};               // a storage texture's: one mip, every layer, as a 2D array
        VkImageView             encodedView                 = VK_NULL_HANDLE;   // an sRGB swapchain image's or TextureDesc::encodedView's UNORM view (AttachmentOps::encodedColor)
        VmaAllocation           allocation                  = nullptr;
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
        VkPipelineLayout        layout          = VK_NULL_HANDLE;   // its set layout's, not its own
        VkPipelineBindPoint     bindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    };

    struct VulkanDescriptorSetLayout : DescriptorSetLayout
    {
        VkDescriptorSetLayout   layout          = VK_NULL_HANDLE;
        VkPipelineLayout        pipelineLayout  = VK_NULL_HANDLE;   // this set and the push-constant range, shared by every pipeline
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
