// clang-format off
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rasm/types.h"
#include "rasm/profiler.h"
#include "rasm/render/resource.h"

namespace rasm
{
    class Engine;

    class RenderContext
    {
    public:
                                            RenderContext(Engine* owner);
                                            ~RenderContext();
        bool                                initialize(Backend backend);
        void                                cleanup();
        BufferHandle                        createBuffer(ResourceDesc desc);
        TextureHandle                       createTexture(ResourceDesc desc);
        ShaderHandle                        createShader(ResourceDesc desc);
        PipelineHandle                      createPipeline(ResourceDesc desc);
        DescriptorSetLayoutHandle           createBindlessDescriptorSetLayout(ResourceDesc desc);
        DescriptorPoolHandle                createDescriptorPool(ResourceDesc desc);
        DescriptorSetHandle                 allocateDescriptorSet(ResourceDesc desc, const DescriptorPoolHandle& pool, const DescriptorSetLayoutHandle& layout);
        CommandPoolHandle                   createCommandPool();
        CommandBufferHandle                 createCommandBuffer(const CommandPoolHandle& commandPool);
        SemaphoreHandle                     createSemaphore(bool timeline = false, uint64_t initialValue = 0);
        FenceHandle                         createFence(bool signaled = false);
        RenderTargetHandle                  createRenderTarget(const ResourceDesc& desc);
        const RenderTarget&                 getRenderTarget(const RenderTargetHandle& handle);
        std::vector<TextureHandle>          getSwapchainImages();
        Format                              getSwapchainImageFormat();
        glm::uvec2                          getSwapchainExtent();
        uint64_t                            getBufferDeviceAddress(const BufferHandle& buffer);
        ResourceDesc                        getResourceDesc(const BufferHandle& buffer);
        ResourceDesc                        getResourceDesc(const TextureHandle& texture);
        VRAMStats                           queryVRAMStats();
        uint32_t                            updateBindlessDescriptorSet(const DescriptorSetHandle& bindlessSet, const TextureHandle& texture, uint32_t slot = MAX_BINDLESS_TEXTURES);
        // bool                                updateBindlessDescriptorSet(const DescriptorSetHandle& bindlessSet, const RenderTargetHandle& renderTarget, uint32_t slot);
        bool                                waitForFence(FenceHandle fence, uint64_t timeout = UINT64_MAX);
        bool                                resetFence(FenceHandle fence);
        [[nodiscard]] SwapchainStatus       acquireNextImage(SemaphoreHandle signalSemaphore, uint64_t timeout, uint32_t& imageIndex);
        bool                                beginCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                endCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                transitionImageLayout(const CommandBufferHandle& commandBuffer, TextureHandle texture, TextureUsage oldUsage, TextureUsage newUsage);
        bool                                fillBuffer(const BufferHandle& buffer, const void* data, size_t size, size_t offset = 0);
        bool                                fillTexture(const TextureHandle& texture, const void* data);
        // Invalidates the handles returned by getSwapchainImages unless the result is OUT_OF_DATE.
        [[nodiscard]] SwapchainStatus       recreateSwapchain();
        bool                                beginRendering(const CommandBufferHandle& commandBuffer,TextureHandle colorAttachment, TextureHandle depthAttachment, const glm::vec4& clearColor);
        bool                                endRendering(const CommandBufferHandle& commandBuffer);
        bool                                setViewport(const CommandBufferHandle& commandBuffer, float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f);
        bool                                setScissor(const CommandBufferHandle& commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height);
        bool                                submit(const CommandBufferHandle& commandBuffer, const std::vector<SemaphoreHandle>& waitSemaphores, const std::vector<SemaphoreHandle>& signalSemaphores, FenceHandle fence);
        [[nodiscard]] SwapchainStatus       present(uint32_t imageIndex, SemaphoreHandle waitSemaphore);
        void                                destroySemaphore(const SemaphoreHandle& semaphore);
        void                                destroyTexture(const TextureHandle& texture);
        void                                destroyBuffer(const BufferHandle& buffer);
        void                                bindPipeline(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline);
        void                                bindIndexBuffer(const CommandBufferHandle& commandBuffer, const BufferHandle& buffer, uint64_t offset, Format indexType);
        void                                bindDescriptorSet(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline, const DescriptorSetHandle& descriptorSet, uint32_t setIndex = 0);
        void                                pushConstants(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline, ShaderType stage, const void* data, uint32_t size, uint32_t offset = 0);
        void                                setUniform(const CommandBufferHandle& commandBuffer, const std::string& name, const void* data, size_t size);
        void                                draw(const CommandBufferHandle& commandBuffer, const uint32_t vertexCount, uint32_t instanceCount = 1);
        void                                drawIndexed(const CommandBufferHandle& commandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t vertexOffset = 0, uint32_t firstIndex = 0, uint32_t firstInstance = 0);
        void                                drawIndexedIndirect(const CommandBufferHandle& commandBuffer, const BufferHandle& drawInfoBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride);

    private:
        struct Impl;

        Engine*                 engine  = nullptr;
        Backend                 backend = Backend::VULKAN;
        std::unique_ptr<Impl>   impl;
    };
}
// clang-format on