// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/profiler.h"
#include "rasm/render/resource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rasm
{
    struct Window;
}

namespace rasm::rhi
{
    enum class QueueType
    {
        GRAPHICS,
    };

    enum class Filter
    {
        NEAREST,
        LINEAR,
    };

    enum class AddressMode
    {
        REPEAT,
        MIRRORED_REPEAT,
        CLAMP_TO_EDGE,
    };

    struct SamplerDesc
    {
        std::string     name;
        Filter          magFilter           = Filter::LINEAR;
        Filter          minFilter           = Filter::LINEAR;
        Filter          mipFilter           = Filter::LINEAR;
        AddressMode     addressMode         = AddressMode::REPEAT;
        float           maxAnisotropy       = 1.0f;
    };

    struct SwapchainDesc
    {
        uint32_t        width               = 0;
        uint32_t        height              = 0;
    };

    struct DeviceDesc
    {
        std::string     appName;
        const Window*   window              = nullptr;
        bool            enableValidation    = false;
    };

    struct Buffer
    {
        BufferDesc                          desc;
        uint64_t                            deviceAddress       = 0;
        bool                                mapped              = false;
    };

    struct BufferCopy
    {
        Buffer*                             dst                 = nullptr;
        uint64_t                            srcOffset           = 0;
        uint64_t                            dstOffset           = 0;
        uint64_t                            size                = 0;
    };

    struct Texture
    {
        TextureDesc                         desc;
    };

    struct Sampler
    {
        SamplerDesc                         desc;
    };

    struct Shader
    {
        ShaderDesc                          desc;
    };

    struct Pipeline
    {
        PipelineDesc                        desc;
    };

    struct DescriptorSetLayout
    {
        BindlessDescriptorSetLayoutDesc     desc;
    };

    struct DescriptorPool
    {
        DescriptorPoolDesc                  desc;
    };

    struct DescriptorSet
    {
        std::string                         name;
    };

    struct Fence
    {
    };

    class CommandBuffer
    {
    public:
        virtual                     ~CommandBuffer() = default;
        virtual bool                begin() = 0;
        virtual bool                end() = 0;
        virtual bool                transitionTexture(Texture* texture, TextureUsage oldUsage, TextureUsage newUsage) = 0;
        virtual void                beginRendering(Texture* colorAttachment, Texture* depthAttachment, const glm::vec4& clearColor) = 0;
        virtual void                endRendering() = 0;
        virtual void                setViewport(float x, float y, float width, float height, float minDepth, float maxDepth) = 0;
        virtual void                setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;
        virtual void                setCullMode(CullMode cullMode) = 0;
        virtual void                bindPipeline(Pipeline* pipeline) = 0;
        virtual void                bindIndexBuffer(Buffer* buffer, uint64_t offset, Format indexType) = 0;
        virtual void                bindDescriptorSet(Pipeline* pipeline, DescriptorSet* descriptorSet, uint32_t setIndex) = 0;
        virtual void                pushConstants(Pipeline* pipeline, ShaderType stages, const void* data, uint32_t size, uint32_t offset) = 0;
        virtual void                draw(uint32_t vertexCount, uint32_t instanceCount) = 0;
        virtual void                drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t vertexOffset, uint32_t firstIndex, uint32_t firstInstance) = 0;
        virtual void                drawIndexedIndirect(Buffer* buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;
        virtual void                copyBuffers(Buffer* src, const BufferCopy* copies, uint32_t copyCount) = 0;
    };

    class Swapchain
    {
    public:
        virtual                     ~Swapchain() = default;
        virtual SwapchainStatus     acquireNextImage(uint32_t& imageIndex) = 0;
        virtual SwapchainStatus     present() = 0;
        virtual SwapchainStatus     resize(uint32_t width, uint32_t height) = 0;
        virtual uint32_t            getImageCount() const = 0;
        virtual Texture*            getImage(uint32_t index) = 0;
        virtual Format              getFormat() const = 0;
        virtual glm::uvec2          getExtent() const = 0;
    };

    struct SubmitDesc
    {
        CommandBuffer* const*           commandBuffers      = nullptr;
        uint32_t                        commandBufferCount  = 0;
        Swapchain*                      swapchain           = nullptr;
        Fence*                          signalFence         = nullptr;
        uint64_t                        signalValue         = 0;
    };

    class Queue
    {
    public:
        virtual                     ~Queue() = default;
        virtual bool                submit(const SubmitDesc& desc) = 0;
    };

    class Device
    {
    public:
        virtual                         ~Device() = default;
        virtual Buffer*                 createBuffer(const BufferDesc& desc) = 0;
        virtual Texture*                createTexture(const TextureDesc& desc) = 0;
        virtual Sampler*                createSampler(const SamplerDesc& desc) = 0;
        virtual Shader*                 createShader(const ShaderDesc& desc) = 0;
        virtual Pipeline*               createGraphicsPipeline(const PipelineDesc& desc, Shader* vertexShader, Shader* fragmentShader, DescriptorSetLayout* layout) = 0;
        virtual DescriptorSetLayout*    createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc& desc) = 0;
        virtual DescriptorPool*         createDescriptorPool(const DescriptorPoolDesc& desc) = 0;
        virtual DescriptorSet*          allocateDescriptorSet(const std::string& name, DescriptorPool* pool, DescriptorSetLayout* layout) = 0;
        virtual CommandBuffer*          createCommandBuffer(QueueType queueType, const std::string& name) = 0;
        virtual Fence*                  createFence(uint64_t initialValue, const std::string& name) = 0;
        virtual Swapchain*              createSwapchain(const SwapchainDesc& desc) = 0;
        virtual void                    destroyBuffer(Buffer* buffer) = 0;
        virtual void                    destroyTexture(Texture* texture) = 0;
        virtual void                    destroySampler(Sampler* sampler) = 0;
        virtual void                    destroyShader(Shader* shader) = 0;
        virtual void                    destroyPipeline(Pipeline* pipeline) = 0;
        virtual void                    destroyDescriptorSetLayout(DescriptorSetLayout* layout) = 0;
        virtual void                    destroyDescriptorPool(DescriptorPool* pool) = 0;
        virtual void                    destroyDescriptorSet(DescriptorSet* descriptorSet) = 0;
        virtual void                    destroyCommandBuffer(CommandBuffer* commandBuffer) = 0;
        virtual void                    destroyFence(Fence* fence) = 0;
        virtual void                    destroySwapchain(Swapchain* swapchain) = 0;
        virtual Queue*                  getQueue(QueueType queueType) = 0;
        virtual bool                    fillBuffer(Buffer* buffer, const void* data, size_t size, size_t offset) = 0;
        virtual bool                    fillTexture(Texture* texture, const void* data) = 0;
        virtual void                    updateBindlessTexture(DescriptorSet* descriptorSet, uint32_t slot, Texture* texture, Sampler* sampler) = 0;
        virtual bool                    waitForFence(Fence* fence, uint64_t value, uint64_t timeout) = 0;
        virtual uint64_t                getFenceValue(Fence* fence) = 0;
        virtual VRAMStats               queryVRAMStats() = 0;
        virtual void                    waitIdle() = 0;
    };

    std::unique_ptr<Device>             createDevice(Backend backend, const DeviceDesc& desc);
}
// clang-format on
