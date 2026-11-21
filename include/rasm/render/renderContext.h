// clang-format off
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rasm/types.h"
#include "rasm/render/resource.h"

namespace rasm::rhi
{
    struct Heap;
}

namespace rasm
{
    class Engine;

    struct FenceValue
    {
        FenceHandle                         fence;
        uint64_t                            value                   = 0;
    };

    struct SubmitInfo
    {
        QueueType                           queue                   = QueueType::GRAPHICS;
        const CommandBufferHandle*          commandBuffers          = nullptr;
        uint32_t                            commandBufferCount      = 0;
        const FenceValue*                   waits                   = nullptr;
        uint32_t                            waitCount               = 0;
        FenceValue                          signal                  = {};
        bool                                waitsForSwapchainImage  = false;
        bool                                presentsSwapchainImage  = false;
    };

    class RenderContext
    {
    public:
                                            RenderContext(Engine* owner);
                                            ~RenderContext();
        bool                                initialize(Backend backend);
        void                                cleanup();
        BufferHandle                        createBuffer(const BufferDesc& desc);
        TextureHandle                       createTexture(const TextureDesc& desc);
        ShaderHandle                        createShader(const ShaderDesc& desc);
        PipelineHandle                      createPipeline(const PipelineDesc& desc);
        DescriptorSetLayoutHandle           createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc& desc);
        DescriptorPoolHandle                createDescriptorPool(const DescriptorPoolDesc& desc);
        DescriptorSetHandle                 allocateDescriptorSet(const std::string& name, const DescriptorPoolHandle& pool, const DescriptorSetLayoutHandle& layout);
        CommandBufferHandle                 createCommandBuffer(const std::string& name, QueueType queue = QueueType::GRAPHICS);
        FenceHandle                         createFence(const std::string& name, uint64_t initialValue = 0);
        QueryPoolHandle                     createTimestampQueryPool(const std::string& name, uint32_t count);
        RenderTargetHandle                  createRenderTarget(const RenderTargetDesc& desc);
        RenderTargetHandle                  createTransientRenderTarget(const RenderTargetDesc& desc);
        bool                                placeTransientRenderTargets(const RenderTargetHandle* renderTargets, const uint32_t* firstPasses, const uint32_t* lastPasses, uint32_t count);
        void                                destroyTransientRenderTargets(const RenderTargetHandle* renderTargets, uint32_t count);
        void                                clearNewHistory(const CommandBufferHandle& commandBuffer);
        void                                resizeRenderTargets();
        const RenderTarget&                 getRenderTarget(const RenderTargetHandle& handle);
        RenderTargetDesc                    getRenderTargetDesc(const RenderTargetHandle& handle);
        std::vector<TextureHandle>          getSwapchainImages();
        Format                              getSwapchainImageFormat();
        glm::uvec2                          getSwapchainExtent();
        uint64_t                            getBufferDeviceAddress(const BufferHandle& buffer);
        BufferDesc                          getBufferDesc(const BufferHandle& buffer);
        TextureDesc                         getTextureDesc(const TextureHandle& texture);
        VRAMStats                           queryVRAMStats();
        uint32_t                            updateBindlessDescriptorSet(const DescriptorSetHandle& bindlessSet, const TextureHandle& texture, SamplerType sampler = SamplerType::LINEAR_REPEAT, uint32_t slot = MAX_BINDLESS_TEXTURES);
        bool                                updateBindlessStorageImage(const DescriptorSetHandle& bindlessSet, uint32_t slot, const TextureHandle& texture, uint32_t mip);
        TextureUsage                        getTextureState(const TextureHandle& texture);
        uint64_t                            getFenceValue(FenceHandle fence);
        bool                                waitForFence(FenceHandle fence, uint64_t value, uint64_t timeout = UINT64_MAX);
        [[nodiscard]] SwapchainStatus       acquireNextImage(uint32_t& imageIndex);
        bool                                beginCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                endCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                transitionImageLayout(const CommandBufferHandle& commandBuffer, TextureHandle texture, TextureUsage newUsage, bool discard = false);
        bool                                fillBuffer(const BufferHandle& buffer, const void* data, size_t size, size_t offset = 0);
        bool                                readBuffer(const BufferHandle& buffer, void* data, size_t size, size_t offset = 0);
        void                                resetQueries(const QueryPoolHandle& pool);
        bool                                getTimestamps(const QueryPoolHandle& pool, uint32_t firstQuery, uint32_t count, uint64_t* nanoseconds);
        void                                writeTimestamp(const CommandBufferHandle& commandBuffer, const QueryPoolHandle& pool, uint32_t index);
        bool                                fillTexture(const TextureHandle& texture, const void* data);
        bool                                fillTexture(const TextureHandle& texture, const TextureLevel* levels, uint32_t levelCount);
        bool                                supportsTextureFormat(Format format) const;
        [[nodiscard]] SwapchainStatus       recreateSwapchain();
        bool                                beginRendering(const CommandBufferHandle& commandBuffer, const TextureHandle* colorAttachments, uint32_t colorCount, TextureHandle depthAttachment, const glm::vec4& clearColor, const AttachmentOps& ops = {}, uint32_t face = 0);
        bool                                endRendering(const CommandBufferHandle& commandBuffer);
        bool                                setViewport(const CommandBufferHandle& commandBuffer, float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f);
        const Viewport&                     getViewport() const;
        bool                                setScissor(const CommandBufferHandle& commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height);
        bool                                setCullMode(const CommandBufferHandle& commandBuffer, CullMode cullMode);
        bool                                submit(const SubmitInfo& info);
        bool                                hasAsyncCompute() const;
        void                                discardRecording();
        [[nodiscard]] SwapchainStatus       present();
        void                                destroyTexture(const TextureHandle& texture);
        void                                destroyBuffer(const BufferHandle& buffer);
        bool                                resizeBuffer(const BufferHandle& buffer, uint64_t size);
        void                                bindPipeline(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline);
        void                                bindIndexBuffer(const CommandBufferHandle& commandBuffer, const BufferHandle& buffer, uint64_t offset, Format indexType);
        void                                bindDescriptorSet(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline, const DescriptorSetHandle& descriptorSet, uint32_t setIndex = 0);
        void                                pushConstants(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline, ShaderType stage, const void* data, uint32_t size, uint32_t offset = 0);
        void                                draw(const CommandBufferHandle& commandBuffer, const uint32_t vertexCount, uint32_t instanceCount = 1);
        void                                drawIndexed(const CommandBufferHandle& commandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t vertexOffset = 0, uint32_t firstIndex = 0, uint32_t firstInstance = 0);
        void                                drawIndexedIndirect(const CommandBufferHandle& commandBuffer, const BufferHandle& drawInfoBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride);
        void                                drawIndexedIndirectCount(const CommandBufferHandle& commandBuffer, const BufferHandle& drawInfoBuffer, uint64_t offset, const BufferHandle& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride);
        void                                dispatch(const CommandBufferHandle& commandBuffer, uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);
        void                                memoryBarrier(const CommandBufferHandle& commandBuffer, PipelineStage before, PipelineStage after);
        void                                aliasingBarrier(const CommandBufferHandle& commandBuffer);

    private:
        struct Impl;

        bool                                createAttachments(RenderTarget& renderTarget, const RenderTargetDesc& desc, rhi::Heap* const* heaps, const uint64_t* offsets);
        void                                destroyAttachments(RenderTarget& renderTarget);
        bool                                placeGroup(uint32_t group);

        Engine*                 engine  = nullptr;
        std::unique_ptr<Impl>   impl;
    };
}
// clang-format on
