// clang-format off
#pragma once

#include <optional>

#include "VkBootstrap.h"

#include "rasm/core/types.h"
#include "rasm/gfx/types.h"

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

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
            std::optional<PipelineVKHandle>         createGraphicsPipeline(ResourceDesc desc, const ShaderVKHandle& vertexShader, const ShaderVKHandle& fragmentShader, const DescriptorSetLayoutVKHandle& bindlessDescriptorSetLayout);
            // std::optional<PipelineVKHandle>         createComputePipeline(ResourceDesc desc, const ShaderVKHandle& computeShader);
            std::optional<DescriptorSetLayoutVKHandle> createBindlessDescriptorSetLayout(ResourceDesc desc);
            std::optional<DescriptorPoolVKHandle>   createDescriptorPool(ResourceDesc desc);
            std::optional<DescriptorSetVKHandle>    allocateDescriptorSet(ResourceDesc desc, const DescriptorPoolVKHandle &pool, const DescriptorSetLayoutVKHandle &layout);
            std::optional<CommandPoolVKHandle>      createCommandPool(vkb::QueueType type);
            std::optional<CommandBufferVKHandle>    createCommandBuffer(const CommandPoolVKHandle& commandPool);
            std::optional<SemaphoreVKHandle>        createSemaphore(bool timeline = false, uint64_t initialValue = 0);
            std::optional<FenceVKHandle>            createFence(bool signaled = false);
            std::vector<TextureVKHandle>            getSwapchainImages();
            Format                                  getSwapchainImageFormat();
            std::optional<SwapchainVKHandle>        recreateSwapchain();
            void                                    waitIdle();
            void                                    fillBuffer(const BufferVKHandle& buffer, const void* data, size_t size, size_t offset = 0);
            void                                    fillTexture(const TextureVKHandle& texture, const void* data);
            void                                    transitionImageLayout(const CommandBufferVKHandle& commandBuffer, VkImage image, VkAccessFlags2 srcAccessMask, VkAccessFlags2 dstAccessMask, VkPipelineStageFlags2 srcStageMask, VkPipelineStageFlags2 dstStageMask, VkImageLayout oldLayout, VkImageLayout newLayout);
            void                                    removeBuffer(const BufferVKHandle& buffer);
            void                                    removeTexture(const TextureVKHandle& texture);
            void                                    removeShader(const ShaderVKHandle& shader);
            void                                    removePipeline(const PipelineVKHandle& pipeline);
            void                                    removeCommandPool(const CommandPoolVKHandle& commandPool);
            void                                    removeSemaphore(const SemaphoreVKHandle& semaphore);
            void                                    removeFence(const FenceVKHandle& fence);
            void                                    bindPipeline(const CommandBufferVKHandle& commandBuffer, const PipelineVKHandle& pipeline);
            void                                    bindVertexBuffer(const CommandBufferVKHandle& commandBuffer, const BufferVKHandle& buffer, uint64_t offset, uint32_t binding = 0);
            void                                    bindIndexBuffer(const CommandBufferVKHandle& commandBuffer, const BufferVKHandle& buffer, uint64_t offset, Format indexType);
            void                                    bindDescriptorSet(const CommandBufferVKHandle& commandBuffer, const PipelineVKHandle& pipeline, const DescriptorSetVKHandle& descriptorSet, uint32_t setIndex = 0);
            void                                    setUniform(const CommandBufferVKHandle& commandBuffer, const std::string& name, const void* data, size_t size);
            void                                    pushConstants(const CommandBufferVKHandle& commandBuffer, const PipelineVKHandle& pipeline, ShaderType stage, const void* data, uint32_t size, uint32_t offset = 0);
            void                                    draw(const CommandBufferVKHandle& commandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1);
            void                                    drawIndexed(const CommandBufferVKHandle& commandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t vertexOffset = 0, uint32_t firstIndex = 0, uint32_t firstInstance = 0);
            bool                                    updateBindlessDescriptorSet(const DescriptorSetVKHandle& bindlessSet, const TextureVKHandle& texture, uint32_t slot);
            bool                                    waitForFence(const FenceVKHandle& fence, uint64_t timeout = UINT64_MAX);
            bool                                    resetFence(const FenceVKHandle& fence);
            bool                                    acquireNextImage(const SemaphoreVKHandle& signalSemaphore, uint64_t timeout , uint32_t& imageIndex);
            bool                                    beginCommandBuffer(const CommandBufferVKHandle& commandBuffer);
            bool                                    endCommandBuffer(const CommandBufferVKHandle& commandBuffer);
            bool                                    transitionImageLayout(const CommandBufferVKHandle& commandBuffer, const TextureVKHandle& texture, const TextureUsage& oldUsage, const TextureUsage& newUsage);
            bool                                    beginRendering(const CommandBufferVKHandle& commandBuffer, const TextureVKHandle& colorAttachment, const TextureVKHandle& depthAttachment);
            bool                                    endRendering(const CommandBufferVKHandle& commandBuffer);
            bool                                    setViewport(const CommandBufferVKHandle& commandBuffer, float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f);
            bool                                    setScissor(const CommandBufferVKHandle& commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height);
            bool                                    submit(const CommandBufferVKHandle& commandBuffer, const std::vector<SemaphoreVKHandle>& waitSemaphores, const std::vector<SemaphoreVKHandle>& signalSemaphores, const FenceVKHandle& fence);
            bool                                    present(uint32_t imageIndex, const SemaphoreVKHandle& waitSemaphore);

        private:
            Engine*             engine                      = nullptr;
            VmaAllocator        allocator                   = {};
            vkb::Instance       instance                    = {};
            vkb::PhysicalDevice physical_device             = {};
            vkb::Device         device                      = {};
            vkb::DispatchTable  dispatch_table              = {};
            VkQueue             graphics_queue              = {};
            SwapchainVKHandle   swapchain                   = {};
            VkSurfaceKHR        surface                     = VK_NULL_HANDLE;
            uint32_t            graphics_queue_family_index = 0;
            uint32_t            current_swapchain_image     = 0;
        };
    }
}
// clang-format on