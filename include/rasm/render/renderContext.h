// clang-format off
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rasm/types.h"
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
        BufferHandle                        createBuffer(const BufferDesc& desc);
        TextureHandle                       createTexture(const TextureDesc& desc);
        ShaderHandle                        createShader(const ShaderDesc& desc);
        PipelineHandle                      createPipeline(const PipelineDesc& desc);
        DescriptorSetLayoutHandle           createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc& desc);
        DescriptorPoolHandle                createDescriptorPool(const DescriptorPoolDesc& desc);
        DescriptorSetHandle                 allocateDescriptorSet(const std::string& name, const DescriptorPoolHandle& pool, const DescriptorSetLayoutHandle& layout);
        CommandBufferHandle                 createCommandBuffer(const std::string& name);
        FenceHandle                         createFence(const std::string& name, uint64_t initialValue = 0);
        QueryPoolHandle                     createTimestampQueryPool(const std::string& name, uint32_t count);
        RenderTargetHandle                  createRenderTarget(const RenderTargetDesc& desc);
        const RenderTarget&                 getRenderTarget(const RenderTargetHandle& handle);
        std::vector<TextureHandle>          getSwapchainImages();
        Format                              getSwapchainImageFormat();
        glm::uvec2                          getSwapchainExtent();
        uint64_t                            getBufferDeviceAddress(const BufferHandle& buffer);
        BufferDesc                          getBufferDesc(const BufferHandle& buffer);
        TextureDesc                         getTextureDesc(const TextureHandle& texture);
        VRAMStats                           queryVRAMStats();
        uint32_t                            updateBindlessDescriptorSet(const DescriptorSetHandle& bindlessSet, const TextureHandle& texture, SamplerType sampler = SamplerType::LINEAR_REPEAT, uint32_t slot = MAX_BINDLESS_TEXTURES);
        bool                                waitForFence(FenceHandle fence, uint64_t value, uint64_t timeout = UINT64_MAX);
        [[nodiscard]] SwapchainStatus       acquireNextImage(uint32_t& imageIndex);
        bool                                beginCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                endCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                transitionImageLayout(const CommandBufferHandle& commandBuffer, TextureHandle texture, TextureUsage oldUsage, TextureUsage newUsage);
        bool                                fillBuffer(const BufferHandle& buffer, const void* data, size_t size, size_t offset = 0);
        bool                                readBuffer(const BufferHandle& buffer, void* data, size_t size, size_t offset = 0);
        void                                resetQueries(const QueryPoolHandle& pool);
        bool                                getTimestamps(const QueryPoolHandle& pool, uint32_t count, uint64_t* nanoseconds);
        void                                writeTimestamp(const CommandBufferHandle& commandBuffer, const QueryPoolHandle& pool, uint32_t index);
        bool                                fillTexture(const TextureHandle& texture, const void* data);
        [[nodiscard]] SwapchainStatus       recreateSwapchain();
        bool                                beginRendering(const CommandBufferHandle& commandBuffer, TextureHandle colorAttachment, TextureHandle depthAttachment, const glm::vec4& clearColor, bool load = false);
        bool                                endRendering(const CommandBufferHandle& commandBuffer);
        bool                                setViewport(const CommandBufferHandle& commandBuffer, float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f);
        bool                                setScissor(const CommandBufferHandle& commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height);
        bool                                setCullMode(const CommandBufferHandle& commandBuffer, CullMode cullMode);
        bool                                submit(const CommandBufferHandle* commandBuffers, uint32_t commandBufferCount, FenceHandle signalFence, uint64_t signalValue, bool presentsSwapchainImage);
        [[nodiscard]] SwapchainStatus       present();
        void                                destroyTexture(const TextureHandle& texture);
        void                                destroyBuffer(const BufferHandle& buffer);
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

    private:
        struct Impl;

        Engine*                 engine  = nullptr;
        std::unique_ptr<Impl>   impl;
    };
}
// clang-format on
