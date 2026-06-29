// clang-format off
#pragma once

#include <unordered_map>

#include "rasm/core/types.h"
#include "rasm/gfx/types.h"
#include "rasm/gfx/vulkan.h"

namespace rasm
{
    class Engine;

    class RenderContext
    {
    public:
                                            RenderContext() = default;
                                            RenderContext(Engine *owner);
                                            ~RenderContext() = default;
        bool                                initialize(Backend backend);
        void                                cleanup();
        BufferHandle                        createBuffer(ResourceDesc desc);
        TextureHandle                       createTexture(ResourceDesc desc);
        ShaderHandle                        createShader(ResourceDesc desc);
        PipelineHandle                      createPipeline(ResourceDesc desc);
        CommandPoolHandle                   createCommandPool(vkb::QueueType type);
        CommandBufferHandle                 createCommandBuffer(const CommandPoolHandle& commandPool);
        SemaphoreHandle                     createSemaphore(bool timeline = false, uint64_t initialValue = 0);
        FenceHandle                         createFence(bool signaled = false);
        std::vector<TextureHandle>          getSwapchainImages();
        Format                              getSwapchainImageFormat();
        uint64_t                            getBufferDeviceAddress(const BufferHandle& buffer);
        ResourceDesc                        getResourceDesc(const BufferHandle& buffer);
        ResourceDesc                        getResourceDesc(const TextureHandle& texture);
        bool                                waitForFence(FenceHandle fence, uint64_t timeout = UINT64_MAX);
        bool                                resetFence(FenceHandle fence);
        bool                                acquireNextImage(SemaphoreHandle signalSemaphore, uint64_t timeout, uint32_t& imageIndex);
        bool                                beginCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                endCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                transitionImageLayout(const CommandBufferHandle& commandBuffer, TextureHandle texture, TextureUsage oldUsage, TextureUsage newUsage);
        bool                                fillBuffer(const BufferHandle& buffer, const void* data, size_t size, size_t offset = 0);
        bool                                recreateSwapchain();
        bool                                beginRendering(const CommandBufferHandle& commandBuffer,TextureHandle colorAttachment, TextureHandle depthAttachment);
        bool                                endRendering(const CommandBufferHandle& commandBuffer);
        bool                                setViewport(const CommandBufferHandle& commandBuffer, float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f);
        bool                                setScissor(const CommandBufferHandle& commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height);
        bool                                submit(const CommandBufferHandle& commandBuffer, const std::vector<SemaphoreHandle>& waitSemaphores, const std::vector<SemaphoreHandle>& signalSemaphores, FenceHandle fence);
        bool                                present(uint32_t imageIndex, SemaphoreHandle waitSemaphore);
        void                                destroySemaphore(const SemaphoreHandle& semaphore);
        void                                bindPipeline(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline);
        void                                bindVertexBuffer(const CommandBufferHandle& commandBuffer, const BufferHandle& buffer, uint64_t offset, uint32_t binding = 0);
        void                                bindIndexBuffer(const CommandBufferHandle& commandBuffer, const BufferHandle& buffer, uint64_t offset, Format indexType);
        void                                pushConstants(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline, ShaderType stage, const void* data, uint32_t size, uint32_t offset = 0);
        void                                setUniform(const CommandBufferHandle& commandBuffer, const std::string& name, const void* data, size_t size);
        void                                draw(const CommandBufferHandle& commandBuffer, const uint32_t vertexCount, uint32_t instanceCount = 1);
        void                                drawIndexed(const CommandBufferHandle& commandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t vertexOffset = 0, uint32_t firstIndex = 0, uint32_t firstInstance = 0);

    protected:
        Engine*                                                                         engine        = nullptr;
        gfx::VulkanContext                                                              vulkanContext = {};
        Backend                                                                         backend       = Backend::VULKAN;
        // TODO: make the cache non-vulkan specific.
        std::unordered_map<BufferHandle, gfx::BufferVKHandle, HandleHash>               bufferCache;
        std::unordered_map<TextureHandle, gfx::TextureVKHandle, HandleHash>             textureCache;
        std::unordered_map<TextureHandle, gfx::TextureVKHandle, HandleHash>             swapchainImageCache;
        std::unordered_map<ShaderHandle, gfx::ShaderVKHandle, HandleHash>               shaderCache;
        std::unordered_map<PipelineHandle, gfx::PipelineVKHandle, HandleHash>           pipelineCache;
        std::unordered_map<CommandPoolHandle, gfx::CommandPoolVKHandle, HandleHash>     commandPoolCache;
        std::unordered_map<CommandBufferHandle, gfx::CommandBufferVKHandle, HandleHash> commandBufferCache;
        std::unordered_map<SemaphoreHandle, gfx::SemaphoreVKHandle, HandleHash>         semaphoreCache;
        std::unordered_map<FenceHandle, gfx::FenceVKHandle, HandleHash>                 fenceCache;

        // map resource handles to their desc.
        std::unordered_map<ResourceDesc, BufferHandle, ResourceDescHash> bufferDescCache;
        std::unordered_map<ResourceDesc, TextureHandle, ResourceDescHash> textureDescCache;
        std::unordered_map<ResourceDesc, ShaderHandle, ResourceDescHash> shaderDescCache;
        std::unordered_map<ResourceDesc, PipelineHandle, ResourceDescHash> pipelineDescCache;
    };
}
// clang-format on