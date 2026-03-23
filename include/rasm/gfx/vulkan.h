// clang-format off
#pragma once

#include "VkBootstrap.h"
#include "vk_mem_alloc.h"

#include "rasm/core/types.h"

#include <optional>

namespace rasm
{
    class RenderContext;
    class Engine;

    namespace gfx
    {
        class VulkanContext
        {
        public:
                                                    VulkanContext(Engine *owner);
                                                    VulkanContext() = default;
                                                    ~VulkanContext();
            bool                                    initialize();
            void                                    cleanup();
            std::optional<BufferVKHandle>           createBuffer(ResourceDesc desc);
            std::optional<TextureVKHandle>          createTexture(ResourceDesc desc);
            std::optional<ShaderVKHandle>           createShader(ResourceDesc desc);
            std::optional<PipelineVKHandle>         createGraphicsPipeline(ResourceDesc desc, const ShaderVKHandle& vertexShader, const ShaderVKHandle& fragmentShader);
            // std::optional<PipelineVKHandle>         createComputePipeline(ResourceDesc desc, const ShaderVKHandle& computeShader);
            std::optional<CommandPoolVKHandle>      createCommandPool(vkb::QueueType type);
            std::optional<CommandBufferVKHandle>    createCommandBuffer(const CommandPoolVKHandle& commandPool);
            std::optional<SemaphoreVKHandle>        createSemaphore(bool timeline = false, uint64_t initialValue = 0);
            std::optional<FenceVKHandle>            createFence(bool signaled = false);
            std::optional<SwapchainVKHandle>        recreateSwapchain();
            void                                    transitionImageLayout(const CommandBufferVKHandle& commandBuffer, VkImage image, VkAccessFlags2 srcAccessMask, VkAccessFlags2 dstAccessMask, VkPipelineStageFlags2 srcStageMask, VkPipelineStageFlags2 dstStageMask, VkImageLayout oldLayout, VkImageLayout newLayout);
            void                                    beginCommandBuffer(const CommandBufferVKHandle& commandBuffer);
            void                                    endCommandBuffer(const CommandBufferVKHandle& commandBuffer);
            void                                    beginRendering(const CommandBufferVKHandle& commandBuffer);
            void                                    endRendering(const CommandBufferVKHandle& commandBuffer);
            void                                    removeBuffer(const BufferVKHandle& buffer);
            void                                    removeTexture(const TextureVKHandle& texture);
            void                                    removeShader(const ShaderVKHandle& shader);
            void                                    removePipeline(const PipelineVKHandle& pipeline);
            void                                    removeCommandPool(const CommandPoolVKHandle& commandPool);
            void                                    removeSemaphore(const SemaphoreVKHandle& semaphore);
            void                                    removeFence(const FenceVKHandle& fence);
            void                                    bindPipeline(const CommandBufferVKHandle& commandBuffer, const PipelineVKHandle& pipeline);
            void                                    setUniform(const CommandBufferVKHandle& commandBuffer, const std::string& name, const void* data, size_t size);
            void                                    draw(const CommandBufferVKHandle& commandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1);

        private:
            Engine*             engine          = nullptr;
            VmaAllocator        allocator       = {};
            vkb::Instance       instance        = {};
            vkb::PhysicalDevice physical_device = {};
            vkb::Device         device          = {};
            VkQueue             graphics_queue  = {};
            vkb::Swapchain      swapchain       = {};
            VkSurfaceKHR        surface         = VK_NULL_HANDLE;
            uint32_t            current_swapchain_image   = 0;

        };
    }
}
// clang-format on