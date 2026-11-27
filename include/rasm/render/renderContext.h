// clang-format off
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rasm/types.h"
#include "rasm/window.h"
#include "rasm/render/resource.h"

namespace rasm::rhi
{
    struct Heap;
}

namespace rasm
{
    // What the device is made for. window is the main window (its native handle makes the device's surface, its pixel
    // size the first swapchain); headless makes no surface, and every window gets an offscreen swapchain instead.
    struct RenderContextDesc
    {
        Backend                             backend                 = Backend::VULKAN;
        std::string                         appName                 = {};
        const Window*                       window                  = nullptr;
        bool                                headless                = false;
        bool                                enableValidation        = false;
        bool                                enableSyncValidation    = false;
        bool                                enableGpuValidation     = false;
        bool                                asyncCompute            = false;
        bool                                transferQueue           = true;
        bool                                hostImageCopy           = true;
        bool                                sampledDepth            = true;     // render target depths are SAMPLED and bindless, for Hi-Z culling
        uint32_t                            bindlessTextures        = 1u << 14; // slots of binding 0, at most MAX_BINDLESS_TEXTURES and the device's limit
        std::string                         cacheDir                = {};       // the pipeline cache's folder; "": none
    };

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
        bool                                waitsForSwapchainImage  = false;    // waits for the image of every window that has one acquired
        bool                                presentsSwapchainImage  = false;    // presents them all
    };

    class RenderContext
    {
    public:
                                            RenderContext();
                                            ~RenderContext();
        bool                                initialize(const RenderContextDesc& desc);
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
        void                                destroyRenderTargets(const RenderTargetHandle* renderTargets, uint32_t count);
        void                                destroyRenderTarget(const RenderTargetHandle& renderTarget);
        void                                clearNewHistory(const CommandBufferHandle& commandBuffer);
        void                                resizeRenderTargets();
        // A fixed-size target at a new size, its texture handles and bindless slots kept. Call it only once the GPU is
        // done with every frame that used the target (Renderer::resizeRenderTarget waits for them).
        bool                                resizeRenderTarget(const RenderTargetHandle& renderTarget, uint32_t width, uint32_t height);
        const RenderTarget&                 getRenderTarget(const RenderTargetHandle& handle);
        RenderTargetDesc                    getRenderTargetDesc(const RenderTargetHandle& handle);
        // The set every texture slot and storage image is in, made by initialize; pipelines take its layout.
        DescriptorSetLayoutHandle           getBindlessDescriptorSetLayout() const;
        DescriptorSetHandle                 getBindlessDescriptorSet() const;
        // A window's swapchain, from its native handle and pixel size (an invalid handle: the main window's).
        // Renderer::addWindow makes one, removeWindow destroys it, waiting for the device.
        bool                                createSwapchain(const Window& window);
        void                                destroySwapchain(const WindowHandle& window);
        std::vector<TextureHandle>          getSwapchainImages(const WindowHandle& window = {});
        Format                              getSwapchainImageFormat(const WindowHandle& window = {});
        // The swapchain's UNORM view format for AttachmentOps::encodedColor, or UNKNOWN when the device has none.
        Format                              getSwapchainEncodedFormat(const WindowHandle& window = {});
        glm::uvec2                          getSwapchainExtent(const WindowHandle& window = {});
        uint64_t                            getBufferDeviceAddress(const BufferHandle& buffer);
        BufferDesc                          getBufferDesc(const BufferHandle& buffer);
        const TextureDesc&                  getTextureDesc(const TextureHandle& texture);
        VRAMStats                           queryVRAMStats();
        uint32_t                            updateBindlessDescriptorSet(const DescriptorSetHandle& bindlessSet, const TextureHandle& texture, SamplerType sampler = SamplerType::LINEAR_REPEAT, uint32_t slot = MAX_BINDLESS_TEXTURES);
        // The texture's slot in binding 0 of the bindless set, or UINT32_MAX for none.
        uint32_t                            getBindlessTextureIndex(const TextureHandle& texture) const;
        bool                                isAlive(const TextureHandle& texture) const;
        bool                                isAlive(const BufferHandle& buffer) const;
        bool                                isAlive(const RenderTargetHandle& renderTarget) const;
        bool                                updateBindlessStorageImage(const DescriptorSetHandle& bindlessSet, uint32_t slot, const TextureHandle& texture, uint32_t mip);
        TextureUsage                        getTextureState(const TextureHandle& texture);
        uint64_t                            getFenceValue(FenceHandle fence);
        bool                                waitForFence(FenceHandle fence, uint64_t value, uint64_t timeout = UINT64_MAX);
        // Before the frame's acquires: only the windows acquired after it take part in the frame's submits and presents.
        void                                beginSwapchainFrame();
        [[nodiscard]] SwapchainStatus       acquireNextImage(uint32_t& imageIndex, const WindowHandle& window = {});
        bool                                beginCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                endCommandBuffer(const CommandBufferHandle& commandBuffer);
        bool                                transitionImageLayout(const CommandBufferHandle& commandBuffer, TextureHandle texture, TextureUsage newUsage, bool discard = false);
        // Copies one layer and mip of a readable texture into the start of buffer, rows tightly packed, for readBuffer after the fence.
        bool                                copyTextureToBuffer(const CommandBufferHandle& commandBuffer, const TextureHandle& texture, uint32_t layer, uint32_t mip, const BufferHandle& buffer);
        // Only the width x height texels from the corner (0, 0) of mip 0 and layer 0, rows packed, at bufferOffset.
        bool                                copyTextureToBuffer(const CommandBufferHandle& commandBuffer, const TextureHandle& texture, uint32_t width, uint32_t height, const BufferHandle& buffer, uint64_t bufferOffset);
        bool                                fillBuffer(const BufferHandle& buffer, const void* data, size_t size, size_t offset = 0);
        bool                                readBuffer(const BufferHandle& buffer, void* data, size_t size, size_t offset = 0);
        void                                resetQueries(const QueryPoolHandle& pool);
        bool                                getTimestamps(const QueryPoolHandle& pool, uint32_t firstQuery, uint32_t count, uint64_t* nanoseconds);
        void                                writeTimestamp(const CommandBufferHandle& commandBuffer, const QueryPoolHandle& pool, uint32_t index);
        bool                                fillTexture(const TextureHandle& texture, const void* data);
        bool                                fillTexture(const TextureHandle& texture, const TextureLevel* levels, uint32_t levelCount);
        bool                                fillTexturePart(const TextureHandle& texture, const TextureLevel* levels, uint32_t levelCount, TextureFillProgress& progress, uint64_t maxBytes);
        // A load's fills go between these two; their data may be used once isLoadUploadDone(the value endLoadUploads returns).
        void                                beginLoadUploads();
        uint64_t                            endLoadUploads();
        void                                updateLoadUploadsDone();
        bool                                isLoadUploadDone(uint64_t value) const;
        bool                                waitForLoadUploads(uint64_t value);
        float                               takeUploadGpuMs();
        bool                                supportsTextureFormat(Format format) const;
        bool                                supportsSampleCount(uint32_t samples) const;
        // The widest and tallest 2D texture or render target the device makes, in pixels; 0 without a device.
        uint32_t                            getMaxImageSize() const;
        // At the window's pixel size now.
        [[nodiscard]] SwapchainStatus       recreateSwapchain(const Window& window);
        bool                                beginRendering(const CommandBufferHandle& commandBuffer, const TextureHandle* colorAttachments, uint32_t colorCount, TextureHandle depthAttachment, const glm::vec4& clearColor, const AttachmentOps& ops = {}, uint32_t face = 0);
        bool                                endRendering(const CommandBufferHandle& commandBuffer);
        // A pass with nothing to draw into (a window with no image this frame): its draws record nothing until endRendering.
        void                                beginEmptyPass();
        bool                                setViewport(const CommandBufferHandle& commandBuffer, float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f);
        const Viewport&                     getViewport() const;
        bool                                setScissor(const CommandBufferHandle& commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height);
        bool                                setCullMode(const CommandBufferHandle& commandBuffer, CullMode cullMode);
        bool                                submit(const SubmitInfo& info);
        bool                                hasAsyncCompute() const;
        bool                                hasMeshShaders() const;
        bool                                hasBufferInt64Atomics() const;
        bool                                hasIndependentBlend() const;       // pipelines may blend each color differently (BlendMode::WEIGHTED, WEIGHTED_MOTION, ADDITIVE_COLOR)
        bool                                hasSampleRateShading() const;      // fragment shaders may run once per sample (SV_SampleIndex)
        void                                discardRecording();
        [[nodiscard]] SwapchainStatus       present(const WindowHandle& window = {});
        // The handle is invalid at once; the GPU object, and a texture's bindless slot, are freed once the GPU is done
        // with every frame that may use them (tagReleases, collectReleases).
        void                                destroyTexture(const TextureHandle& texture);
        void                                destroyBuffer(const BufferHandle& buffer);
        // After a frame's submits (dropped or not): what was destroyed since waits for these values and the uploads' last.
        void                                tagReleases(const FenceValue& graphics, const FenceValue& compute);
        // Frees what the GPU is done with, without waiting.
        void                                collectReleases();
        // Runs free once the GPU has finished every frame that may still use what it frees (with the destroys above).
        void                                releaseAfterGpu(std::function<void()> free);
        uint32_t                            getReleasesWaiting() const;
        uint32_t                            getTextureCount() const;
        uint32_t                            getBindlessTextureCount() const;
        // The slots of binding 0: RenderContextDesc::bindlessTextures, lowered to what the device allows.
        uint32_t                            getBindlessTextureCapacity() const;
        // Slots a texture can take now: never used, or freed and done on the GPU (a destroyed texture's waits).
        uint32_t                            getFreeBindlessTextureSlots() const;
        // The bytes of the texture's own memory allocation (its alignment and padding included), 0 for a texture placed
        // in a shared heap (a transient render target) or an invalid handle.
        uint64_t                            getTextureMemoryBytes(const TextureHandle& texture) const;
        bool                                resizeBuffer(const BufferHandle& buffer, uint64_t size);
        void                                bindPipeline(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline);
        void                                bindIndexBuffer(const CommandBufferHandle& commandBuffer, const BufferHandle& buffer, uint64_t offset, Format indexType);
        void                                bindDescriptorSet(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline, const DescriptorSetHandle& descriptorSet, uint32_t setIndex = 0);
        void                                pushConstants(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline, const void* data, uint32_t size, uint32_t offset = 0);
        void                                draw(const CommandBufferHandle& commandBuffer, const uint32_t vertexCount, uint32_t instanceCount = 1);
        void                                drawIndexed(const CommandBufferHandle& commandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t vertexOffset = 0, uint32_t firstIndex = 0, uint32_t firstInstance = 0);
        void                                drawIndexedIndirect(const CommandBufferHandle& commandBuffer, const BufferHandle& drawInfoBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride);
        void                                drawIndexedIndirectCount(const CommandBufferHandle& commandBuffer, const BufferHandle& drawInfoBuffer, uint64_t offset, const BufferHandle& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride);
        void                                dispatch(const CommandBufferHandle& commandBuffer, uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);
        void                                dispatchIndirect(const CommandBufferHandle& commandBuffer, const BufferHandle& buffer, uint64_t offset);
        void                                drawMeshTasksIndirect(const CommandBufferHandle& commandBuffer, const BufferHandle& buffer, uint64_t offset, uint32_t drawCount, uint32_t stride);
        void                                memoryBarrier(const CommandBufferHandle& commandBuffer, PipelineStage before, PipelineStage after);
        void                                aliasingBarrier(const CommandBufferHandle& commandBuffer);

    private:
        struct Impl;

        bool                                createAttachments(RenderTarget& renderTarget, const RenderTargetDesc& desc, rhi::Heap* const* heaps, const uint64_t* offsets);
        void                                destroyAttachments(RenderTarget& renderTarget);
        bool                                placeGroup(uint32_t group);
        bool                                copyTexture(const CommandBufferHandle& commandBuffer, const TextureHandle& texture, uint32_t layer, uint32_t mip, uint32_t width, uint32_t height, const BufferHandle& buffer, uint64_t bufferOffset, const char* caller);

        std::unique_ptr<Impl>   impl;
    };
}
// clang-format on
