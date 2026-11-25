// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/render/resource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rasm::rhi
{
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
        uint32_t                width               = 0;
        uint32_t                height              = 0;
        const NativeWindow*     window              = nullptr;  // a window of its own, whose surface the swapchain makes and owns; nullptr: the device's window (DeviceDesc::window)
    };

    struct DeviceDesc
    {
        std::string             appName;
        const NativeWindow*     window                  = nullptr;  // the surface's window; nullptr: headless, no surface
        bool                    enableValidation        = false;
        bool                    enableSyncValidation    = false;
        bool                    enableGpuValidation     = false;
        bool                    asyncCompute            = false;
        bool                    transferQueue           = false;    // a queue of a family that only copies, for the load uploads
        bool                    hostImageCopy           = true;     // false: every texture goes through the staging ring
        std::string             cacheDir;                           // the pipeline cache's folder; empty: none
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
        TextureUsage                        state               = TextureUsage::UNKNOWN;
        uint32_t                            mipLevels           = 1;
        bool                                hostCopy            = false;    // filled on the host; otherwise copied from the staging ring
        bool                                ownershipTransfer   = false;    // exclusive to the graphics family: filled on the transfer queue, it is released there and acquired on the graphics queue
        uint64_t                            memoryBytes         = 0;        // its own allocation's size; 0 when placed in a shared heap
    };

    struct TextureCopy
    {
        Texture*                            dst                 = nullptr;
        uint64_t                            srcOffset           = 0;
        uint32_t                            mip                 = 0;
        uint32_t                            y                   = 0;        // rows [y, y + height) of the mip, in texels
        uint32_t                            height              = 0;
        bool                                discard             = false;    // the texture's first copy: its old contents may go
        bool                                last                = false;    // the fill's last copy: the texture leaves TRANSFER_DST after it
    };

    struct FormatBlock
    {
        uint32_t                            width               = 1;
        uint32_t                            height              = 1;
        uint32_t                            bytes               = 0;
    };

    FormatBlock                             getFormatBlock(Format format);
    uint64_t                                getTextureLevelSize(Format format, uint32_t width, uint32_t height);

    struct MemoryRequirements
    {
        uint64_t                            size                = 0;
        uint64_t                            alignment           = 1;
        uint32_t                            memoryTypeBits      = 0;
    };

    struct Heap
    {
        uint64_t                            size                = 0;
        uint32_t                            memoryTypeBits      = 0;
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
        uint32_t                            storageImageCount   = 0;    // binding 1's size, from its layout
    };

    struct Fence
    {
    };

    struct QueryPool
    {
        uint32_t                            count               = 0;
    };

    class CommandBuffer
    {
    public:
        virtual                     ~CommandBuffer() = default;
        virtual bool                begin() = 0;
        virtual bool                end() = 0;
        virtual bool                transitionTexture(Texture* texture, TextureUsage oldUsage, TextureUsage newUsage) = 0;
        virtual void                beginRendering(Texture* const* colorAttachments, uint32_t colorCount, Texture* depthAttachment, const glm::vec4& clearColor, const AttachmentOps& ops, uint32_t face) = 0;
        virtual void                endRendering() = 0;
        virtual void                setViewport(float x, float y, float width, float height, float minDepth, float maxDepth) = 0;
        virtual void                setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;
        virtual void                setCullMode(CullMode cullMode) = 0;
        virtual void                bindPipeline(Pipeline* pipeline) = 0;
        virtual void                bindIndexBuffer(Buffer* buffer, uint64_t offset, Format indexType) = 0;
        virtual void                bindDescriptorSet(Pipeline* pipeline, DescriptorSet* descriptorSet, uint32_t setIndex) = 0;
        virtual void                pushConstants(Pipeline* pipeline, const void* data, uint32_t size, uint32_t offset) = 0;
        virtual void                draw(uint32_t vertexCount, uint32_t instanceCount) = 0;
        virtual void                drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t vertexOffset, uint32_t firstIndex, uint32_t firstInstance) = 0;
        virtual void                drawIndexedIndirect(Buffer* buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;
        virtual void                drawIndexedIndirectCount(Buffer* buffer, uint64_t offset, Buffer* countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride) = 0;
        virtual void                drawMeshTasksIndirect(Buffer* buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;
        virtual void                dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
        virtual void                dispatchIndirect(Buffer* buffer, uint64_t offset) = 0;
        virtual void                memoryBarrier(PipelineStage before, PipelineStage after) = 0;
        virtual void                aliasingBarrier() = 0;
        virtual void                copyBuffers(Buffer* src, const BufferCopy* copies, uint32_t copyCount) = 0;
        virtual void                copyToTextures(Buffer* src, const TextureCopy* copies, uint32_t copyCount) = 0;
        // On the graphics queue, after a wait for the transfer queue's submit that released them (Texture::ownershipTransfer).
        virtual void                acquireTextures(Texture* const* textures, uint32_t count) = 0;
        // Copies one layer and mip of a single-sample texture to dst at dstOffset, rows tightly packed, for the host to
        // read after the submit's fence. The texture is in state before and after; a depth texture gives its depth.
        virtual void                copyFromTexture(Texture* src, TextureUsage state, uint32_t layer, uint32_t mip, Buffer* dst, uint64_t dstOffset, uint32_t width, uint32_t height) = 0; // the texels from (0, 0), rows packed
        virtual void                writeTimestamp(QueryPool* pool, uint32_t index) = 0;
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
        // The format of a view that sees the images' stored sRGB numbers (blending on them), or UNKNOWN without one.
        virtual Format              getEncodedFormat() const = 0;
        virtual glm::uvec2          getExtent() const = 0;
    };

    constexpr uint32_t                  MAX_SUBMIT_COMMAND_BUFFERS  = 8;
    constexpr uint32_t                  MAX_SUBMIT_WAITS            = 4;
    constexpr uint32_t                  MAX_SUBMIT_SWAPCHAINS       = 8;    // windows one submit waits for or presents

    struct FenceWait
    {
        Fence*                          fence               = nullptr;
        uint64_t                        value               = 0;
    };

    struct SubmitDesc
    {
        CommandBuffer* const*           commandBuffers      = nullptr;
        uint32_t                        commandBufferCount  = 0;
        const FenceWait*                waits               = nullptr;
        uint32_t                        waitCount           = 0;
        Swapchain* const*               acquireSwapchains   = nullptr;  // the submit waits for each one's acquired image
        uint32_t                        acquireCount        = 0;
        Swapchain* const*               presentSwapchains   = nullptr;  // and signals each one's present
        uint32_t                        presentCount        = 0;
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
        virtual MemoryRequirements      getTextureMemoryRequirements(const TextureDesc& desc) = 0;
        virtual Heap*                   createHeap(uint64_t size, uint64_t alignment, uint32_t memoryTypeBits, const std::string& name) = 0;
        virtual Texture*                createPlacedTexture(const TextureDesc& desc, Heap* heap, uint64_t offset) = 0;
        virtual Sampler*                createSampler(const SamplerDesc& desc) = 0;
        virtual Shader*                 createShader(const ShaderDesc& desc) = 0;
        virtual Pipeline*               createGraphicsPipeline(const PipelineDesc& desc, Shader* vertexShader, Shader* fragmentShader, DescriptorSetLayout* layout) = 0;
        virtual Pipeline*               createComputePipeline(const PipelineDesc& desc, Shader* computeShader, DescriptorSetLayout* layout) = 0;
        virtual Pipeline*               createMeshPipeline(const PipelineDesc& desc, Shader* taskShader, Shader* meshShader, Shader* fragmentShader, DescriptorSetLayout* layout) = 0;
        virtual DescriptorSetLayout*    createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc& desc) = 0;
        virtual DescriptorPool*         createDescriptorPool(const DescriptorPoolDesc& desc) = 0;
        virtual DescriptorSet*          allocateDescriptorSet(const std::string& name, DescriptorPool* pool, DescriptorSetLayout* layout) = 0;
        virtual CommandBuffer*          createCommandBuffer(QueueType queueType, const std::string& name) = 0;
        virtual Fence*                  createFence(uint64_t initialValue, const std::string& name) = 0;
        virtual Swapchain*              createSwapchain(const SwapchainDesc& desc) = 0;
        virtual QueryPool*              createTimestampQueryPool(uint32_t count, const std::string& name) = 0;
        virtual void                    destroyBuffer(Buffer* buffer) = 0;
        virtual void                    destroyTexture(Texture* texture) = 0;
        virtual void                    destroyHeap(Heap* heap) = 0;
        virtual void                    destroySampler(Sampler* sampler) = 0;
        virtual void                    destroyShader(Shader* shader) = 0;
        virtual void                    destroyPipeline(Pipeline* pipeline) = 0;
        virtual void                    destroyDescriptorSetLayout(DescriptorSetLayout* layout) = 0;
        virtual void                    destroyDescriptorPool(DescriptorPool* pool) = 0;
        virtual void                    destroyDescriptorSet(DescriptorSet* descriptorSet) = 0;
        virtual void                    destroyCommandBuffer(CommandBuffer* commandBuffer) = 0;
        virtual void                    destroyFence(Fence* fence) = 0;
        virtual void                    destroySwapchain(Swapchain* swapchain) = 0;
        virtual void                    destroyQueryPool(QueryPool* pool) = 0;
        virtual Queue*                  getQueue(QueueType queueType) = 0;
        virtual bool                    hasAsyncCompute() const = 0;
        virtual bool                    hasTransferQueue() const = 0;
        virtual bool                    hasMeshShaders() const = 0;
        // Timestamps on the queue count like the graphics queue's: same valid bits and period.
        virtual bool                    hasTimestamps(QueueType queueType) const = 0;
        virtual bool                    fillBuffer(Buffer* buffer, const void* data, size_t size, size_t offset) = 0;
        virtual bool                    readBuffer(Buffer* buffer, void* data, size_t size, size_t offset) = 0;
        virtual void                    resetQueries(QueryPool* pool) = 0;
        virtual bool                    getTimestamps(QueryPool* pool, uint32_t firstQuery, uint32_t count, uint64_t* nanoseconds) = 0;
        virtual bool                    supportsTextureFormat(Format format) const = 0;
        // The sample counts a render target can have, as bits: bit n set means 2^n samples (1, 2, 4, 8, ...).
        virtual uint32_t                supportedSampleCounts() const = 0;
        // Slots binding 0 of the bindless set (combined image samplers, updated after bind) may have.
        virtual uint32_t                maxBindlessTextures() const = 0;
        virtual bool                    fillTexture(Texture* texture, const TextureLevel* levels, uint32_t levelCount) = 0;
        virtual void                    updateBindlessTexture(DescriptorSet* descriptorSet, uint32_t slot, Texture* texture, Sampler* sampler) = 0;
        virtual void                    updateBindlessStorageImage(DescriptorSet* descriptorSet, uint32_t slot, Texture* texture, uint32_t mip) = 0;
        virtual bool                    waitForFence(Fence* fence, uint64_t value, uint64_t timeout) = 0;
        virtual uint64_t                getFenceValue(Fence* fence) = 0;
        virtual VRAMStats               queryVRAMStats() = 0;
        virtual void                    waitIdle() = 0;
    };

    std::unique_ptr<Device>             createDevice(Backend backend, const DeviceDesc& desc);
}
// clang-format on
