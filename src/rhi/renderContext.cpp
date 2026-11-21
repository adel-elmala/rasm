#include "rasm/render/renderContext.h"
#include "rasm/engine.h"
#include "rhi/device.h"
#include "rhi/memoryAliasing.h"
#include "rhi/mipChain.h"
#include "rhi/textureStates.h"
#include "rhi/uploadManager.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <unordered_map>
#include <vector>

namespace rasm
{
    namespace
    {
        struct RenderTargetInfo
        {
            RenderTargetDesc desc;
            bool transient = false;
        };

        // Transient render targets laid out together in heaps, from the passes that use each one.
        struct TransientGroup
        {
            std::vector<RenderTargetHandle> targets;
            std::vector<uint32_t> firstPasses;
            std::vector<uint32_t> lastPasses;
            std::vector<rhi::Heap *> heaps;
        };

        // The colors of a new history copy, cleared before any frame can read it as the previous one.
        struct HistoryClear
        {
            TextureHandle colors[MAX_COLOR_ATTACHMENTS];
            uint32_t colorCount = 0;
            glm::vec4 clearColor = glm::vec4(0.0f);
            const rhi::CommandBuffer *commandBuffer = nullptr; // the clear was recorded into it
        };
    }

    namespace
    {
        // Invalid use of the public API: stop in the debugger in debug builds, fail the call with an error in release.
        bool checkArgument(bool valid, const char *caller, const char *message)
        {
            if (valid)
                return true;
            spdlog::error("{}: {}", caller, message);
            assert(false && "Invalid RenderContext argument, see the log.");
            return false;
        }

        // Every pass draws with a depth attachment, and colors are attachments 0 to n-1, so a gap would be dropped.
        // A target without colors is depth-only, like a shadow map. History keeps only the colors of the previous
        // frame, so a depth-only target has none. A storage target is written by compute passes, which draw nothing, so
        // it has colors and no depth; history copies are cleared by drawing into them, so it has no history either.
        bool checkRenderTargetDesc(const RenderTargetDesc &desc, const char *caller)
        {
            const uint32_t colorCount = colorAttachmentCount(desc.colorFormats);
            const bool contiguous = std::all_of(desc.colorFormats + colorCount, std::end(desc.colorFormats), [](Format format)
                                                { return format == Format::UNKNOWN; });
            if (desc.storage)
                return checkArgument(contiguous, caller, "a render target's color formats can't have a gap.") &&
                       checkArgument(colorCount > 0 && desc.depthFormat == Format::UNKNOWN && !desc.cube, caller, "a storage render target has colors and no depth.") &&
                       checkArgument(!desc.history, caller, "a storage render target can't have history.");
            return checkArgument(contiguous, caller, "a render target's color formats can't have a gap.") &&
                   checkArgument(desc.depthFormat != Format::UNKNOWN, caller, "a render target needs a depth format.") &&
                   checkArgument(colorCount > 0 || !desc.history, caller, "history keeps a render target's colors, so a depth-only target can't have it.") &&
                   checkArgument(!desc.cube || (colorCount == 0 && desc.swapchainScale <= 0.0f && desc.width == desc.height), caller, "a cube render target is depth-only, with a fixed square size.");
        }

        glm::uvec2 renderTargetSize(const RenderTargetDesc &desc, glm::uvec2 swapchainExtent)
        {
            if (desc.swapchainScale <= 0.0f)
                return {desc.width, desc.height};
            const glm::vec2 size = glm::round(glm::vec2(swapchainExtent) * desc.swapchainScale);
            return glm::max(glm::uvec2(size), glm::uvec2(1));
        }

        TextureDesc colorTextureDesc(const RenderTargetDesc &desc, glm::uvec2 size, uint32_t index)
        {
            return {.name = desc.name + " Color Texture" + (index == 0 ? std::string() : " " + std::to_string(index)),
                    .width = size.x,
                    .height = size.y,
                    .format = desc.colorFormats[index],
                    .usage = desc.storage ? TextureUsage::SAMPLED_STORAGE : TextureUsage::SAMPLED_COLOR_ATTACHMENT};
        }

        // Everything but the textures, which createAttachments makes, and the storage slots, which
        // Impl::allocateStorageSlots gives a storage target.
        RenderTarget renderTargetFromDesc(const RenderTargetDesc &desc, glm::uvec2 size)
        {
            RenderTarget rt = {.width = size.x, .height = size.y, .colorCount = colorAttachmentCount(desc.colorFormats), .depthFormat = desc.depthFormat, .clearColor = desc.clearColor, .history = desc.history};
            std::copy(std::begin(desc.colorFormats), std::end(desc.colorFormats), std::begin(rt.colorFormats));
            for (auto &slots : rt.storageSlots)
                std::fill(std::begin(slots), std::end(slots), UINT32_MAX);
            return rt;
        }

        // The Hi-Z pass of occlusion culling samples D32 depth through the bindless set, and so do the readers of a
        // depth-only target's depth (a shadow map). A depth/stencil view can't be sampled.
        TextureDesc depthTextureDesc(const RenderTargetDesc &desc, glm::uvec2 size, bool occlusionCulling)
        {
            const bool sampled = (occlusionCulling || colorAttachmentCount(desc.colorFormats) == 0) && desc.depthFormat == Format::D32_SFLOAT;
            return {.name = desc.name + " Depth Texture",
                    .width = size.x,
                    .height = size.y,
                    .format = desc.depthFormat,
                    .usage = sampled ? TextureUsage::SAMPLED_DEPTH_ATTACHMENT : TextureUsage::DEPTH_STENCIL_ATTACHMENT,
                    .cube = desc.cube};
        }

        // The target's textures at its current size: its colors, then its depth, which a storage target does not have.
        // Returns how many there are.
        uint32_t attachmentTextureDescs(const RenderTargetDesc &desc, glm::uvec2 swapchainExtent, bool occlusionCulling, TextureDesc *textureDescs)
        {
            const glm::uvec2 size = renderTargetSize(desc, swapchainExtent);
            const uint32_t colorCount = colorAttachmentCount(desc.colorFormats);
            for (uint32_t c = 0; c < colorCount; ++c)
                textureDescs[c] = colorTextureDesc(desc, size, c);
            if (desc.depthFormat == Format::UNKNOWN)
                return colorCount;
            textureDescs[colorCount] = depthTextureDesc(desc, size, occlusionCulling);
            return colorCount + 1;
        }

        rhi::SamplerDesc samplerDesc(SamplerType type)
        {
            switch (type)
            {
            case SamplerType::LINEAR_MIRRORED_REPEAT:
                return {.name = "Linear Mirrored Repeat Sampler", .addressMode = rhi::AddressMode::MIRRORED_REPEAT, .maxAnisotropy = 8.0f};
            case SamplerType::LINEAR_CLAMP:
                return {.name = "Linear Clamp Sampler", .addressMode = rhi::AddressMode::CLAMP_TO_EDGE, .maxAnisotropy = 8.0f};
            case SamplerType::NEAREST_REPEAT:
                return {.name = "Nearest Repeat Sampler", .magFilter = rhi::Filter::NEAREST, .minFilter = rhi::Filter::NEAREST, .mipFilter = rhi::Filter::NEAREST};
            case SamplerType::NEAREST_MIRRORED_REPEAT:
                return {.name = "Nearest Mirrored Repeat Sampler", .magFilter = rhi::Filter::NEAREST, .minFilter = rhi::Filter::NEAREST, .mipFilter = rhi::Filter::NEAREST, .addressMode = rhi::AddressMode::MIRRORED_REPEAT};
            case SamplerType::NEAREST_CLAMP:
                return {.name = "Nearest Clamp Sampler", .magFilter = rhi::Filter::NEAREST, .minFilter = rhi::Filter::NEAREST, .mipFilter = rhi::Filter::NEAREST, .addressMode = rhi::AddressMode::CLAMP_TO_EDGE};
            default:
                return {.name = "Linear Repeat Sampler", .maxAnisotropy = 8.0f};
            }
        }

        rhi::Buffer *lookup(const std::unordered_map<BufferHandle, rhi::Buffer *, HandleHash> &objects, const BufferHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: buffer handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::Texture *lookup(const std::unordered_map<TextureHandle, rhi::Texture *, HandleHash> &objects, const TextureHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: texture handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::Shader *lookup(const std::unordered_map<ShaderHandle, rhi::Shader *, HandleHash> &objects, const ShaderHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: shader handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::Pipeline *lookup(const std::unordered_map<PipelineHandle, rhi::Pipeline *, HandleHash> &objects, const PipelineHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: pipeline handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::CommandBuffer *lookup(const std::unordered_map<CommandBufferHandle, rhi::CommandBuffer *, HandleHash> &objects, const CommandBufferHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: command buffer handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::Fence *lookup(const std::unordered_map<FenceHandle, rhi::Fence *, HandleHash> &objects, const FenceHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: fence handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::QueryPool *lookup(const std::unordered_map<QueryPoolHandle, rhi::QueryPool *, HandleHash> &objects, const QueryPoolHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: query pool handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::DescriptorSetLayout *lookup(const std::unordered_map<DescriptorSetLayoutHandle, rhi::DescriptorSetLayout *, HandleHash> &objects, const DescriptorSetLayoutHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: descriptor set layout handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::DescriptorPool *lookup(const std::unordered_map<DescriptorPoolHandle, rhi::DescriptorPool *, HandleHash> &objects, const DescriptorPoolHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: descriptor pool handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }

        rhi::DescriptorSet *lookup(const std::unordered_map<DescriptorSetHandle, rhi::DescriptorSet *, HandleHash> &objects, const DescriptorSetHandle &handle, const char *caller)
        {
            auto it = objects.find(handle);
            if (it == objects.end())
            {
                spdlog::error("{}: descriptor set handle not found.", caller);
                return nullptr;
            }
            return it->second;
        }
    }

    // clang-format off
    struct RenderContext::Impl
    {
        std::unique_ptr<rhi::Device>                                                                                            device;
        rhi::Swapchain*                                                                                                         swapchain       = nullptr;
        rhi::Sampler*                                                                                                           samplers[static_cast<size_t>(SamplerType::COUNT)] = {};
        rhi::UploadManager                                                                                                      uploads;
        rhi::TextureStates                                                                                                      textureStates;
        std::vector<TextureHandle>                                                                                              swapchainImages;  // also in textures, but owned by the swapchain

        std::unordered_map<BufferHandle, rhi::Buffer*, HandleHash>                                                              buffers;
        std::unordered_map<TextureHandle, rhi::Texture*, HandleHash>                                                            textures;
        std::unordered_map<ShaderHandle, rhi::Shader*, HandleHash>                                                              shaders;
        std::unordered_map<PipelineHandle, rhi::Pipeline*, HandleHash>                                                          pipelines;
        std::unordered_map<CommandBufferHandle, rhi::CommandBuffer*, HandleHash>                                                commandBuffers;
        std::unordered_map<FenceHandle, rhi::Fence*, HandleHash>                                                                fences;
        std::unordered_map<QueryPoolHandle, rhi::QueryPool*, HandleHash>                                                        queryPools;
        std::unordered_map<DescriptorSetLayoutHandle, rhi::DescriptorSetLayout*, HandleHash>                                    descriptorSetLayouts;
        std::unordered_map<DescriptorPoolHandle, rhi::DescriptorPool*, HandleHash>                                              descriptorPools;
        std::unordered_map<DescriptorSetHandle, rhi::DescriptorSet*, HandleHash>                                                descriptorSets;
        std::unordered_map<RenderTargetHandle, RenderTarget, HandleHash>                                                        renderTargets;
        std::unordered_map<RenderTargetHandle, RenderTargetInfo, HandleHash>                                                    renderTargetInfos;
        std::vector<TransientGroup>                                                                                             transientGroups;
        std::vector<HistoryClear>                                                                                               pendingClears;  // made since the last frame began
        std::vector<HistoryClear>                                                                                               recordedClears; // in a frame not yet submitted

        // Dedup caches, only for immutable objects keyed on their full state. Mutable resources
        // (buffers, textures, descriptor pools/sets) are always created fresh (B1).
        std::unordered_map<ShaderDesc, ShaderHandle, ShaderDescHash>                                                            shaderDescCache;
        std::unordered_map<PipelineDesc, PipelineHandle, PipelineDescHash>                                                      pipelineDescCache;
        std::unordered_map<BindlessDescriptorSetLayoutDesc, DescriptorSetLayoutHandle, BindlessDescriptorSetLayoutDescHash>     descriptorSetLayoutDescCache;

        uint32_t                                                                                                                nextBindlessTextureIndex = 0;
        // Binding 1 slots of storage render targets, from RESERVED_STORAGE_IMAGES up. A destroyed target gives its back.
        uint32_t                                                                                                                nextStorageSlot = RESERVED_STORAGE_IMAGES;
        std::vector<uint32_t>                                                                                                   freeStorageSlots;

        // The current pass's attachment formats. A pipeline must match them for its draws to be defined.
        Format                                                                                                                  passColorFormats[MAX_COLOR_ATTACHMENTS] = {};
        Format                                                                                                                  passDepthFormat = Format::UNKNOWN;
        // Recording state of the command buffer being recorded. beginCommandBuffer resets it.
        bool                                                                                                                    skipDraws       = false; // a mismatched or missing pipeline was rejected
        bool                                                                                                                    computeBound    = false; // a compute pipeline is bound, so dispatches are defined
        bool                                                                                                                    insidePass      = false; // between beginRendering and endRendering, where draws are allowed and dispatches and barriers are not
        Viewport                                                                                                                viewport;                // the last setViewport

        void registerSwapchainImages(HandleManager& handleManager)
        {
            for (uint32_t i = 0; i < swapchain->getImageCount(); ++i)
            {
                auto handle = handleManager.getNextTextureHandle();
                textures[handle] = swapchain->getImage(i);
                swapchainImages.push_back(handle);
            }
        }

        void unregisterSwapchainImages()
        {
            for (const auto& handle : swapchainImages)
                textures.erase(handle);
            swapchainImages.clear();
        }

        // A slot per color of each copy, kept for the target's life: a resize or a new placement writes the new
        // textures into the same slots. All or none, so a target never has only some of them.
        bool allocateStorageSlots(RenderTarget& renderTarget, const RenderTargetDesc& desc)
        {
            if (!desc.storage)
                return true;
            const uint32_t copies = desc.history ? MAX_FRAMES_IN_FLIGHT : 1;
            const uint32_t needed = copies * renderTarget.colorCount;
            const uint32_t available = static_cast<uint32_t>(freeStorageSlots.size()) + (MAX_BINDLESS_STORAGE_IMAGES - nextStorageSlot);
            if (needed > available)
            {
                spdlog::error("Storage render target '{}' needs {} storage image slots, and {} of MAX_BINDLESS_STORAGE_IMAGES ({}, {} of them kept for the environment bake) are free.",
                              desc.name, needed, available, MAX_BINDLESS_STORAGE_IMAGES, RESERVED_STORAGE_IMAGES);
                return false;
            }
            for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
            {
                for (uint32_t c = 0; c < renderTarget.colorCount; ++c)
                {
                    if (i >= copies)
                        renderTarget.storageSlots[i][c] = renderTarget.storageSlots[0][c];
                    else if (freeStorageSlots.empty())
                        renderTarget.storageSlots[i][c] = nextStorageSlot++;
                    else
                    {
                        renderTarget.storageSlots[i][c] = freeStorageSlots.back();
                        freeStorageSlots.pop_back();
                    }
                }
            }
            return true;
        }

        // Called once the GPU is done with the target, so its slots can take other textures at once. A target without
        // history shares its slots between the frame slots, so each is given back once.
        void releaseStorageSlots(RenderTarget& renderTarget)
        {
            for (uint32_t c = 0; c < MAX_COLOR_ATTACHMENTS; ++c)
            {
                for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
                {
                    const uint32_t slot = renderTarget.storageSlots[i][c];
                    bool shared = false;
                    for (uint32_t j = 0; j < i; ++j)
                        shared = shared || renderTarget.storageSlots[j][c] == slot;
                    if (slot != UINT32_MAX && !shared)
                        freeStorageSlots.push_back(slot);
                }
            }
            for (auto& slots : renderTarget.storageSlots)
                std::fill(std::begin(slots), std::end(slots), UINT32_MAX);
        }
    };
    // clang-format on

    RenderContext::RenderContext(Engine *owner) : engine(owner), impl(std::make_unique<Impl>()) {}

    RenderContext::~RenderContext() = default;

    bool RenderContext::initialize(Backend backend)
    {
        const auto config = engine->getConfig();
        const auto &window = engine->getMainWindow();

        impl->device = rhi::createDevice(backend, {.appName = config.appName,
                                                   .window = &window,
                                                   .enableValidation = config.enableValidation,
                                                   .enableSyncValidation = config.enableSyncValidation,
                                                   .enableGpuValidation = config.enableGpuValidation,
                                                   .asyncCompute = config.asyncCompute,
                                                   .hostImageCopy = config.hostImageCopy});
        if (!impl->device)
            return false;

        if (!impl->uploads.initialize(*impl->device))
            return false;

        impl->swapchain = impl->device->createSwapchain({.width = window.pixelWidth, .height = window.pixelHeight});
        if (!impl->swapchain)
        {
            spdlog::error("Failed to create the swapchain.");
            return false;
        }
        impl->registerSwapchainImages(engine->handleManager);

        // A small fixed set shared by every bindless texture, instead of one sampler per texture (P2).
        for (size_t i = 0; i < static_cast<size_t>(SamplerType::COUNT); ++i)
        {
            impl->samplers[i] = impl->device->createSampler(samplerDesc(static_cast<SamplerType>(i)));
            if (!impl->samplers[i])
                return false;
        }

        return true;
    }

    void RenderContext::cleanup()
    {
        if (!impl->device)
            return;

        auto &device = *impl->device;
        device.waitIdle();

        impl->uploads.cleanup();
        for (const auto &[handle, descriptorSet] : impl->descriptorSets)
            device.destroyDescriptorSet(descriptorSet);
        for (const auto &[handle, pool] : impl->descriptorPools)
            device.destroyDescriptorPool(pool);
        for (const auto &[handle, layout] : impl->descriptorSetLayouts)
            device.destroyDescriptorSetLayout(layout);
        for (const auto &[handle, rhiCommandBuffer] : impl->commandBuffers)
            device.destroyCommandBuffer(rhiCommandBuffer);
        for (const auto &[handle, fence] : impl->fences)
            device.destroyFence(fence);
        for (const auto &[handle, pool] : impl->queryPools)
            device.destroyQueryPool(pool);
        for (const auto &[handle, buffer] : impl->buffers)
            device.destroyBuffer(buffer);
        impl->unregisterSwapchainImages();
        for (const auto &[handle, texture] : impl->textures)
            device.destroyTexture(texture);
        for (const auto &group : impl->transientGroups)
            for (auto *heap : group.heaps)
                device.destroyHeap(heap);
        for (const auto &[handle, pipeline] : impl->pipelines)
            device.destroyPipeline(pipeline);
        for (const auto &[handle, shader] : impl->shaders)
            device.destroyShader(shader);
        for (auto *sampler : impl->samplers)
            if (sampler)
                device.destroySampler(sampler);
        if (impl->swapchain)
            device.destroySwapchain(impl->swapchain);

        impl = std::make_unique<Impl>();
    }

    BufferHandle RenderContext::createBuffer(const BufferDesc &desc)
    {
        auto *buffer = impl->device->createBuffer(desc);
        if (!buffer)
            return {};

        BufferHandle handle = engine->handleManager.getNextBufferHandle();
        impl->buffers[handle] = buffer;
        return handle;
    }

    // A cube is drawn one face at a time through its face views, which only a depth attachment has, and nothing
    // uploads its faces, so it is a depth attachment.
    TextureHandle RenderContext::createTexture(const TextureDesc &desc)
    {
        const bool depthAttachment = desc.usage == TextureUsage::DEPTH_STENCIL_ATTACHMENT || desc.usage == TextureUsage::SAMPLED_DEPTH_ATTACHMENT;
        if (!checkArgument(!desc.cube || depthAttachment || desc.usage == TextureUsage::SAMPLED_STORAGE, __func__, "a cube texture is a depth attachment or a storage texture."))
            return {};
        // Attachments are drawn into one mip, so only storage textures (whose mips compute writes) and sampled ones
        // (whose mips fillTexture copies or, for RGBA8, makes) take a mip count.
        if (!checkArgument(desc.mipLevels <= 1 || desc.usage == TextureUsage::SAMPLED_STORAGE || desc.usage == TextureUsage::SAMPLED, __func__,
                           "only a SAMPLED_STORAGE or a SAMPLED texture takes more than one mip."))
            return {};
        if (!checkArgument(desc.usage != TextureUsage::SAMPLED || impl->device->supportsTextureFormat(desc.format), __func__,
                           "the device can't sample this format (supportsTextureFormat)."))
            return {};
        auto *texture = impl->device->createTexture(desc);
        if (!texture)
            return {};

        TextureHandle handle = engine->handleManager.getNextTextureHandle();
        impl->textures[handle] = texture;
        return handle;
    }

    ShaderHandle RenderContext::createShader(const ShaderDesc &desc)
    {
        if (!checkArgument(!desc.code.empty(), __func__, "the shader has no code.") || !checkArgument(!desc.entryPoint.empty(), __func__, "the shader has no entry point."))
            return {};

        auto cachedIt = impl->shaderDescCache.find(desc);
        if (cachedIt != impl->shaderDescCache.end())
            return cachedIt->second;

        auto *shader = impl->device->createShader(desc);
        if (!shader)
            return {};

        ShaderHandle handle = engine->handleManager.getNextShaderHandle();
        impl->shaders[handle] = shader;
        impl->shaderDescCache[desc] = handle;
        return handle;
    }

    PipelineHandle RenderContext::createPipeline(const PipelineDesc &desc)
    {
        const bool compute = desc.computeShader.isValid();
        if (!checkArgument(!compute || (!desc.vertexShader.isValid() && !desc.fragmentShader.isValid()), __func__, "a pipeline has either a compute shader or vertex and fragment shaders."))
            return {};

        auto cachedIt = impl->pipelineDescCache.find(desc);
        if (cachedIt != impl->pipelineDescCache.end())
            return cachedIt->second;

        auto *layout = lookup(impl->descriptorSetLayouts, desc.descriptorSetLayout, __func__);
        if (!layout)
            return {};

        rhi::Pipeline *pipeline = nullptr;
        if (compute)
        {
            auto *computeShader = lookup(impl->shaders, desc.computeShader, __func__);
            if (!computeShader || !checkArgument(computeShader->desc.type == ShaderType::COMPUTE, __func__, "the compute shader is not a COMPUTE shader."))
                return {};
            pipeline = impl->device->createComputePipeline(desc, computeShader, layout);
        }
        else
        {
            auto *vertexShader = lookup(impl->shaders, desc.vertexShader, __func__);
            auto *fragmentShader = lookup(impl->shaders, desc.fragmentShader, __func__);
            if (!vertexShader || !fragmentShader)
                return {};
            if (!checkArgument(vertexShader->desc.type == ShaderType::VERTEX && fragmentShader->desc.type == ShaderType::FRAGMENT, __func__, "the vertex and fragment shaders must be VERTEX and FRAGMENT shaders."))
                return {};
            pipeline = impl->device->createGraphicsPipeline(desc, vertexShader, fragmentShader, layout);
        }
        if (!pipeline)
            return {};

        PipelineHandle handle = engine->handleManager.getNextPipelineHandle();
        impl->pipelines[handle] = pipeline;
        impl->pipelineDescCache[desc] = handle;
        return handle;
    }

    DescriptorSetLayoutHandle RenderContext::createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc &desc)
    {
        auto cachedIt = impl->descriptorSetLayoutDescCache.find(desc);
        if (cachedIt != impl->descriptorSetLayoutDescCache.end())
            return cachedIt->second;

        auto *layout = impl->device->createBindlessDescriptorSetLayout(desc);
        if (!layout)
            return {};

        DescriptorSetLayoutHandle handle = engine->handleManager.getNextDescriptorSetLayoutHandle();
        impl->descriptorSetLayouts[handle] = layout;
        impl->descriptorSetLayoutDescCache[desc] = handle;
        return handle;
    }

    DescriptorPoolHandle RenderContext::createDescriptorPool(const DescriptorPoolDesc &desc)
    {
        auto *pool = impl->device->createDescriptorPool(desc);
        if (!pool)
            return {};

        DescriptorPoolHandle handle = engine->handleManager.getNextDescriptorPoolHandle();
        impl->descriptorPools[handle] = pool;
        return handle;
    }

    DescriptorSetHandle RenderContext::allocateDescriptorSet(const std::string &name, const DescriptorPoolHandle &pool, const DescriptorSetLayoutHandle &layout)
    {
        auto *rhiPool = lookup(impl->descriptorPools, pool, __func__);
        auto *rhiLayout = lookup(impl->descriptorSetLayouts, layout, __func__);
        if (!rhiPool || !rhiLayout)
            return {};

        auto *descriptorSet = impl->device->allocateDescriptorSet(name, rhiPool, rhiLayout);
        if (!descriptorSet)
            return {};

        DescriptorSetHandle handle = engine->handleManager.getNextDescriptorSetHandle();
        impl->descriptorSets[handle] = descriptorSet;
        return handle;
    }

    CommandBufferHandle RenderContext::createCommandBuffer(const std::string &name, QueueType queue)
    {
        auto *rhiCommandBuffer = impl->device->createCommandBuffer(queue, name);
        if (!rhiCommandBuffer)
            return {};

        CommandBufferHandle handle = engine->handleManager.getNextCommandBufferHandle();
        impl->commandBuffers[handle] = rhiCommandBuffer;
        return handle;
    }

    FenceHandle RenderContext::createFence(const std::string &name, uint64_t initialValue)
    {
        auto *fence = impl->device->createFence(initialValue, name);
        if (!fence)
            return {};

        FenceHandle handle = engine->handleManager.getNextFenceHandle();
        impl->fences[handle] = fence;
        return handle;
    }

    QueryPoolHandle RenderContext::createTimestampQueryPool(const std::string &name, uint32_t count)
    {
        auto *pool = impl->device->createTimestampQueryPool(count, name);
        if (!pool)
            return {};

        QueryPoolHandle handle = engine->handleManager.getNextQueryPoolHandle();
        impl->queryPools[handle] = pool;
        return handle;
    }

    RenderTargetHandle RenderContext::createRenderTarget(const RenderTargetDesc &desc)
    {
        if (!checkRenderTargetDesc(desc, __func__))
            return {};

        RenderTarget rt = renderTargetFromDesc(desc, renderTargetSize(desc, getSwapchainExtent()));
        if (!impl->allocateStorageSlots(rt, desc))
            return {};
        if (!createAttachments(rt, desc, nullptr, nullptr))
        {
            impl->releaseStorageSlots(rt);
            return {};
        }

        RenderTargetHandle rtHandle = engine->handleManager.getNextRenderTargetHandle();
        impl->renderTargets[rtHandle] = rt;
        impl->renderTargetInfos[rtHandle] = {.desc = desc};
        return rtHandle;
    }

    // The handle is usable at once, but the textures exist only after placeTransientRenderTargets.
    RenderTargetHandle RenderContext::createTransientRenderTarget(const RenderTargetDesc &desc)
    {
        if (!checkArgument(!desc.history, __func__, "a history render target lives across frames, so it can't be transient.") ||
            !checkRenderTargetDesc(desc, __func__))
            return {};

        RenderTarget rt = renderTargetFromDesc(desc, renderTargetSize(desc, getSwapchainExtent()));
        if (!impl->allocateStorageSlots(rt, desc))
            return {};
        RenderTargetHandle rtHandle = engine->handleManager.getNextRenderTargetHandle();
        impl->renderTargets[rtHandle] = rt;
        impl->renderTargetInfos[rtHandle] = {.desc = desc, .transient = true};
        return rtHandle;
    }

    // A group that already holds one of the targets is replaced, after the GPU is done with its memory.
    bool RenderContext::placeTransientRenderTargets(const RenderTargetHandle *renderTargets, const uint32_t *firstPasses, const uint32_t *lastPasses, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            auto it = impl->renderTargetInfos.find(renderTargets[i]);
            if (!checkArgument(it != impl->renderTargetInfos.end() && it->second.transient, __func__, "only transient render targets can be placed."))
                return false;
        }

        const RenderTargetHandle *end = renderTargets + count;
        std::vector<rhi::Heap *> retired;
        for (size_t g = impl->transientGroups.size(); g-- > 0;)
        {
            auto &group = impl->transientGroups[g];
            const bool replaced = std::any_of(group.targets.begin(), group.targets.end(), [renderTargets, end](const RenderTargetHandle &target)
                                              { return std::find(renderTargets, end, target) != end; });
            if (!replaced)
                continue;
            if (retired.empty())
                impl->device->waitIdle();
            for (const auto &target : group.targets)
            {
                if (std::find(renderTargets, end, target) == end)
                    destroyAttachments(impl->renderTargets[target]);
            }
            retired.insert(retired.end(), group.heaps.begin(), group.heaps.end());
            impl->transientGroups.erase(impl->transientGroups.begin() + static_cast<std::ptrdiff_t>(g));
        }

        impl->transientGroups.push_back({.targets = std::vector<RenderTargetHandle>(renderTargets, end),
                                         .firstPasses = std::vector<uint32_t>(firstPasses, firstPasses + count),
                                         .lastPasses = std::vector<uint32_t>(lastPasses, lastPasses + count)});
        const bool placed = placeGroup(static_cast<uint32_t>(impl->transientGroups.size() - 1));

        // placeGroup destroyed the targets' old textures, so nothing lives in these heaps any more.
        for (auto *heap : retired)
            impl->device->destroyHeap(heap);
        return placed;
    }

    // The GPU may still use them, so it is waited for. A group none of whose targets is left frees its heaps.
    void RenderContext::destroyTransientRenderTargets(const RenderTargetHandle *renderTargets, uint32_t count)
    {
        bool waited = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            auto it = impl->renderTargetInfos.find(renderTargets[i]);
            if (it == impl->renderTargetInfos.end() || !it->second.transient)
                continue;
            if (!waited)
                impl->device->waitIdle();
            waited = true;
            destroyAttachments(impl->renderTargets[renderTargets[i]]);
            impl->releaseStorageSlots(impl->renderTargets[renderTargets[i]]);
            impl->renderTargets.erase(renderTargets[i]);
            impl->renderTargetInfos.erase(it);
        }

        std::erase_if(impl->transientGroups, [this](const TransientGroup &group)
                      {
                          const bool empty = std::none_of(group.targets.begin(), group.targets.end(), [this](const RenderTargetHandle &target)
                                                          { return impl->renderTargets.contains(target); });
                          if (empty)
                              for (auto *heap : group.heaps)
                                  impl->device->destroyHeap(heap);
                          return empty; });
    }

    // The clears run before the frame's passes. A frame that is not submitted gives them back for the next one.
    void RenderContext::clearNewHistory(const CommandBufferHandle &commandBuffer)
    {
        const auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!rhiCommandBuffer)
            return;
        for (auto &clear : impl->pendingClears)
        {
            clear.commandBuffer = rhiCommandBuffer;
            for (uint32_t i = 0; i < clear.colorCount; ++i)
                transitionImageLayout(commandBuffer, clear.colors[i], TextureUsage::COLOR_ATTACHMENT, true);
            beginRendering(commandBuffer, clear.colors, clear.colorCount, {}, clear.clearColor);
            endRendering(commandBuffer);
            for (uint32_t i = 0; i < clear.colorCount; ++i)
                transitionImageLayout(commandBuffer, clear.colors[i], TextureUsage::SAMPLED);
        }
        impl->recordedClears.insert(impl->recordedClears.end(), impl->pendingClears.begin(), impl->pendingClears.end());
        impl->pendingClears.clear();
    }

    // Called with the device idle, after the swapchain changed size.
    void RenderContext::resizeRenderTargets()
    {
        for (auto &[handle, info] : impl->renderTargetInfos)
        {
            if (!info.transient && info.desc.swapchainScale > 0.0f)
                createAttachments(impl->renderTargets[handle], info.desc, nullptr, nullptr);
        }
        for (uint32_t g = 0; g < impl->transientGroups.size(); ++g)
        {
            const auto &targets = impl->transientGroups[g].targets;
            const bool scaled = std::any_of(targets.begin(), targets.end(), [this](const RenderTargetHandle &target)
                                            { return impl->renderTargetInfos[target].desc.swapchainScale > 0.0f; });
            if (scaled)
                placeGroup(g);
        }
    }

    // One request per texture of each target, its colors then its depth. The old textures are destroyed by
    // createAttachments, then the old heaps.
    bool RenderContext::placeGroup(uint32_t groupIndex)
    {
        auto &group = impl->transientGroups[groupIndex];
        const glm::uvec2 extent = getSwapchainExtent();
        const auto count = static_cast<uint32_t>(group.targets.size());

        std::vector<rhi::AliasRequest> requests;
        std::vector<uint32_t> firstRequests(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            TextureDesc textureDescs[MAX_COLOR_ATTACHMENTS + 1];
            const uint32_t textureCount = attachmentTextureDescs(impl->renderTargetInfos[group.targets[i]].desc, extent, engine->config.occlusionCulling, textureDescs);
            firstRequests[i] = static_cast<uint32_t>(requests.size());
            for (uint32_t t = 0; t < textureCount; ++t)
            {
                const auto requirements = impl->device->getTextureMemoryRequirements(textureDescs[t]);
                requests.push_back({.size = requirements.size,
                                    .alignment = requirements.alignment,
                                    .memoryTypeBits = requirements.memoryTypeBits,
                                    .firstPass = group.firstPasses[i],
                                    .lastPass = group.lastPasses[i]});
            }
        }

        std::vector<rhi::AliasPlacement> placements(requests.size());
        std::vector<rhi::AliasHeap> layout;
        rhi::packAliased(requests.data(), static_cast<uint32_t>(requests.size()), placements.data(), layout);

        const std::vector<rhi::Heap *> oldHeaps = std::move(group.heaps);
        group.heaps.clear();
        bool placed = true;
        for (uint32_t h = 0; h < layout.size() && placed; ++h)
        {
            auto *heap = impl->device->createHeap(layout[h].size, layout[h].alignment, layout[h].memoryTypeBits, "Transient Render Targets " + std::to_string(h));
            placed = heap != nullptr;
            if (heap)
                group.heaps.push_back(heap);
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            auto &rt = impl->renderTargets[group.targets[i]];
            const auto &desc = impl->renderTargetInfos[group.targets[i]].desc;
            if (!placed)
            {
                destroyAttachments(rt);
                continue;
            }
            rhi::Heap *heaps[MAX_COLOR_ATTACHMENTS + 1] = {};
            uint64_t offsets[MAX_COLOR_ATTACHMENTS + 1] = {};
            const uint32_t textureCount = (i + 1 < count ? firstRequests[i + 1] : static_cast<uint32_t>(requests.size())) - firstRequests[i];
            for (uint32_t t = 0; t < textureCount; ++t)
            {
                const rhi::AliasPlacement &placement = placements[firstRequests[i] + t];
                heaps[t] = group.heaps[placement.heap];
                offsets[t] = placement.offset;
            }
            placed = createAttachments(rt, desc, heaps, offsets) && placed;
        }

        for (auto *heap : oldHeaps)
            impl->device->destroyHeap(heap);
        if (!placed)
        {
            spdlog::error("Failed to place {} transient render targets.", count);
            return false;
        }

        uint64_t separate = 0;
        uint64_t aliased = 0;
        for (const auto &request : requests)
            separate += request.size;
        for (const auto &heap : layout)
            aliased += heap.size;
        spdlog::info("Placed {} transient render targets in {} heap(s): {:.1f} MiB instead of {:.1f} MiB.", count, layout.size(),
                     static_cast<double>(aliased) / (1024.0 * 1024.0), static_cast<double>(separate) / (1024.0 * 1024.0));
        return true;
    }

    // Builds the target's textures at its current size, in heaps when given (its colors, then depth), and destroys the
    // old ones. The new textures take the old ones' bindless slots, so a resize or a new layout does not use up the set.
    bool RenderContext::createAttachments(RenderTarget &renderTarget, const RenderTargetDesc &desc, rhi::Heap *const *heaps, const uint64_t *offsets)
    {
        // A frame's passes all run before the next frame's on the one queue, and the texture states give the next
        // frame's first use a barrier, so one copy serves every slot. Only history needs the previous frame's copy.
        const uint32_t copies = desc.history ? MAX_FRAMES_IN_FLIGHT : 1;
        const uint32_t colorCount = renderTarget.colorCount;
        TextureDesc textureDescs[MAX_COLOR_ATTACHMENTS + 1];
        const bool hasDepth = attachmentTextureDescs(desc, getSwapchainExtent(), engine->config.occlusionCulling, textureDescs) > colorCount;
        const TextureDesc &depthDesc = textureDescs[colorCount];
        const bool sampledDepth = hasDepth && depthDesc.usage == TextureUsage::SAMPLED_DEPTH_ATTACHMENT;

        auto slotOf = [this](TextureHandle texture)
        {
            const uint32_t slot = engine->getBindlessTextureIndex(texture);
            return slot == UINT32_MAX ? MAX_BINDLESS_TEXTURES : slot;
        };
        uint32_t colorSlots[MAX_FRAMES_IN_FLIGHT][MAX_COLOR_ATTACHMENTS];
        uint32_t depthSlots[MAX_FRAMES_IN_FLIGHT];
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            for (uint32_t c = 0; c < MAX_COLOR_ATTACHMENTS; ++c)
                colorSlots[i][c] = slotOf(renderTarget.attachments[i].color[c]);
            depthSlots[i] = slotOf(renderTarget.attachments[i].depth);
        }
        destroyAttachments(renderTarget);
        renderTarget.width = textureDescs[0].width;
        renderTarget.height = textureDescs[0].height;

        auto makeTexture = [this, heaps, offsets](const TextureDesc &textureDesc, uint32_t attachment) -> TextureHandle
        {
            if (!heaps)
                return createTexture(textureDesc);
            auto *texture = impl->device->createPlacedTexture(textureDesc, heaps[attachment], offsets[attachment]);
            if (!texture)
                return {};
            TextureHandle handle = engine->handleManager.getNextTextureHandle();
            impl->textures[handle] = texture;
            return handle;
        };

        for (uint32_t i = 0; i < copies; ++i)
        {
            auto &attachment = renderTarget.attachments[i];
            bool created = true;
            for (uint32_t c = 0; c < colorCount; ++c)
            {
                attachment.color[c] = makeTexture(textureDescs[c], c);
                created = created && attachment.color[c].isValid();
            }
            if (hasDepth)
                attachment.depth = makeTexture(depthDesc, colorCount);
            if (!created || (hasDepth && !attachment.depth.isValid()))
            {
                spdlog::error("Failed to create the attachments of render target '{}'.", desc.name);
                destroyAttachments(renderTarget);
                return false;
            }
        }

        // Only once every attachment exists, so that a failed creation leaves the bindless set untouched.
        for (uint32_t i = 0; i < copies; ++i)
        {
            const auto &attachment = renderTarget.attachments[i];
            bool bindless = true;
            for (uint32_t c = 0; c < colorCount && bindless; ++c)
                bindless = engine->addBindlessTexture(engine->getBindlessDescriptorSet(), attachment.color[c], SamplerType::LINEAR_CLAMP, colorSlots[i][c]) != UINT32_MAX;
            if (bindless && sampledDepth)
                bindless = engine->addBindlessTexture(engine->getBindlessDescriptorSet(), attachment.depth, SamplerType::NEAREST_CLAMP, depthSlots[i]) != UINT32_MAX;
            for (uint32_t c = 0; c < colorCount && bindless && desc.storage; ++c)
                bindless = updateBindlessStorageImage(engine->getBindlessDescriptorSet(), renderTarget.storageSlots[i][c], attachment.color[c], 0);
            if (!bindless)
            {
                spdlog::error("Failed to make render target '{}' bindless.", desc.name);
                destroyAttachments(renderTarget);
                return false;
            }
        }
        for (uint32_t i = copies; i < MAX_FRAMES_IN_FLIGHT; ++i)
            renderTarget.attachments[i] = renderTarget.attachments[0];

        // Each frame reads the copy the frame before drew, so on the first frame that copy holds nothing yet.
        if (desc.history)
        {
            for (uint32_t i = 0; i < copies; ++i)
            {
                HistoryClear clear = {.colorCount = colorCount, .clearColor = desc.clearColor};
                std::copy(std::begin(renderTarget.attachments[i].color), std::end(renderTarget.attachments[i].color), std::begin(clear.colors));
                impl->pendingClears.push_back(clear);
            }
        }
        return true;
    }

    // Slots of a target without history all hold the same textures, so each texture is destroyed once.
    void RenderContext::destroyAttachments(RenderTarget &renderTarget)
    {
        auto destroy = [this](TextureHandle texture)
        {
            if (!texture.isValid())
                return;
            auto clears = [texture](const HistoryClear &clear)
            { return std::find(std::begin(clear.colors), std::end(clear.colors), texture) != std::end(clear.colors); };
            std::erase_if(impl->pendingClears, clears);
            std::erase_if(impl->recordedClears, clears);
            engine->registry.bindlessTextureIndexMap.erase(texture);
            destroyTexture(texture);
        };
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            const RenderTargetAttachments attachment = renderTarget.attachments[i];
            for (uint32_t j = i; j < MAX_FRAMES_IN_FLIGHT; ++j)
            {
                for (uint32_t c = 0; c < MAX_COLOR_ATTACHMENTS; ++c)
                {
                    if (renderTarget.attachments[j].color[c] == attachment.color[c])
                        renderTarget.attachments[j].color[c] = {};
                }
                if (renderTarget.attachments[j].depth == attachment.depth)
                    renderTarget.attachments[j].depth = {};
            }
            for (const TextureHandle texture : attachment.color)
                destroy(texture);
            destroy(attachment.depth);
        }
    }

    const RenderTarget &RenderContext::getRenderTarget(const RenderTargetHandle &handle)
    {
        auto it = impl->renderTargets.find(handle);
        if (it == impl->renderTargets.end())
        {
            spdlog::error("Render target handle not found.");
            static RenderTarget emptyRenderTarget{};
            return emptyRenderTarget;
        }
        return it->second;
    }

    RenderTargetDesc RenderContext::getRenderTargetDesc(const RenderTargetHandle &handle)
    {
        auto it = impl->renderTargetInfos.find(handle);
        if (it == impl->renderTargetInfos.end())
        {
            spdlog::error("Render target handle not found.");
            return {};
        }
        return it->second.desc;
    }

    void RenderContext::destroyTexture(const TextureHandle &texture)
    {
        if (std::find(impl->swapchainImages.begin(), impl->swapchainImages.end(), texture) != impl->swapchainImages.end())
        {
            spdlog::error("Swapchain images belong to the swapchain and can't be destroyed.");
            return;
        }

        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        if (!rhiTexture)
            return;
        impl->textureStates.forget(rhiTexture);
        impl->uploads.cancelUploads(rhiTexture);
        impl->device->destroyTexture(rhiTexture);
        impl->textures.erase(texture);
    }

    void RenderContext::destroyBuffer(const BufferHandle &buffer)
    {
        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        if (!rhiBuffer)
            return;
        impl->uploads.cancelUploads(rhiBuffer);
        impl->device->destroyBuffer(rhiBuffer);
        impl->buffers.erase(buffer);
    }

    // A new buffer with the old desc and the new size, under the same handle, so whatever holds the handle (a render
    // graph's declarations) stays valid. The contents and the device address change, and the GPU must be done with
    // the old buffer. On failure the old buffer stays.
    bool RenderContext::resizeBuffer(const BufferHandle &buffer, uint64_t size)
    {
        auto *oldBuffer = lookup(impl->buffers, buffer, __func__);
        if (!oldBuffer)
            return false;

        BufferDesc desc = oldBuffer->desc;
        desc.size = size;
        auto *newBuffer = impl->device->createBuffer(desc);
        if (!newBuffer)
            return false;
        impl->uploads.cancelUploads(oldBuffer);
        impl->device->destroyBuffer(oldBuffer);
        impl->buffers[buffer] = newBuffer;
        return true;
    }

    std::vector<TextureHandle> RenderContext::getSwapchainImages()
    {
        return impl->swapchainImages;
    }

    Format RenderContext::getSwapchainImageFormat()
    {
        return impl->swapchain->getFormat();
    }

    glm::uvec2 RenderContext::getSwapchainExtent()
    {
        return impl->swapchain->getExtent();
    }

    bool RenderContext::fillBuffer(const BufferHandle &buffer, const void *data, size_t size, size_t offset)
    {
        if (!checkArgument(data != nullptr && size > 0, __func__, "data must be non-null and size greater than zero."))
            return false;

        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        if (!rhiBuffer || !checkArgument(offset + size <= rhiBuffer->desc.size, __func__, "the range is outside the buffer."))
            return false;

        // Unmapped buffers are device-local: the data reaches them with the next submit (P1).
        if (rhiBuffer->mapped)
            return impl->device->fillBuffer(rhiBuffer, data, size, offset);
        return impl->uploads.upload(rhiBuffer, data, size, offset);
    }

    bool RenderContext::readBuffer(const BufferHandle &buffer, void *data, size_t size, size_t offset)
    {
        if (!checkArgument(data != nullptr && size > 0, __func__, "data must be non-null and size greater than zero."))
            return false;

        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        if (!rhiBuffer || !checkArgument(offset + size <= rhiBuffer->desc.size, __func__, "the range is outside the buffer.") ||
            !checkArgument(rhiBuffer->desc.hostReadable, __func__, "only buffers created with hostReadable can be read."))
            return false;
        return impl->device->readBuffer(rhiBuffer, data, size, offset);
    }

    void RenderContext::resetQueries(const QueryPoolHandle &pool)
    {
        auto *rhiPool = lookup(impl->queryPools, pool, __func__);
        if (rhiPool)
            impl->device->resetQueries(rhiPool);
    }

    bool RenderContext::getTimestamps(const QueryPoolHandle &pool, uint32_t firstQuery, uint32_t count, uint64_t *nanoseconds)
    {
        auto *rhiPool = lookup(impl->queryPools, pool, __func__);
        if (!rhiPool || !checkArgument(firstQuery <= rhiPool->count && count <= rhiPool->count - firstQuery && (count == 0 || nanoseconds != nullptr), __func__, "the queries are past the pool's."))
            return false;
        return impl->device->getTimestamps(rhiPool, firstQuery, count, nanoseconds);
    }

    void RenderContext::writeTimestamp(const CommandBufferHandle &commandBuffer, const QueryPoolHandle &pool, uint32_t index)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiPool = lookup(impl->queryPools, pool, __func__);
        if (rhiCommandBuffer && rhiPool && checkArgument(index < rhiPool->count, __func__, "index is past the pool's queries."))
            rhiCommandBuffer->writeTimestamp(rhiPool, index);
    }

    // RGBA8 gets its mips made on the CPU; any other format must have one mip (environment images).
    bool RenderContext::fillTexture(const TextureHandle &texture, const void *data)
    {
        ZoneScoped;

        if (!checkArgument(data != nullptr, __func__, "data must be non-null."))
            return false;
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        if (!rhiTexture)
            return false;

        const TextureDesc &desc = rhiTexture->desc;
        const bool rgba8 = desc.format == Format::R8G8B8A8_UNORM || desc.format == Format::R8G8B8A8_SRGB || desc.format == Format::B8G8R8A8_SRGB;
        if (!checkArgument(rgba8 || rhiTexture->mipLevels == 1, __func__, "only an RGBA8 texture gets its mips made; fill the others with every level."))
            return false;

        std::vector<unsigned char> storage;
        std::vector<TextureLevel> levels(rhiTexture->mipLevels);
        if (rgba8)
        {
            if (!rhi::buildMipChain(static_cast<const unsigned char *>(data), desc.width, desc.height, desc.format != Format::R8G8B8A8_UNORM, rhiTexture->mipLevels, storage, levels.data()))
            {
                spdlog::error("Failed to make the mips of '{}'.", desc.name);
                return false;
            }
        }
        else
        {
            levels[0] = {.data = data, .size = rhi::getTextureLevelSize(desc.format, desc.width, desc.height)};
        }
        return fillTexture(texture, levels.data(), static_cast<uint32_t>(levels.size()));
    }

    // A texture the device copies on the host has its data at once. The others are copied from the staging ring with
    // the next submit, which comes before any frame that samples them, on the same queue (P2).
    bool RenderContext::fillTexture(const TextureHandle &texture, const TextureLevel *levels, uint32_t levelCount)
    {
        ZoneScoped;

        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        if (!rhiTexture || !checkArgument(levels != nullptr, __func__, "levels must be non-null.") ||
            !checkArgument(rhiTexture->desc.usage == TextureUsage::SAMPLED, __func__, "only a SAMPLED texture is filled.") ||
            !checkArgument(levelCount == rhiTexture->mipLevels, __func__, "levelCount must be the texture's mip count."))
            return false;
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            const uint64_t size = rhi::getTextureLevelSize(rhiTexture->desc.format, std::max(1u, rhiTexture->desc.width >> level), std::max(1u, rhiTexture->desc.height >> level));
            if (!checkArgument(levels[level].data != nullptr && levels[level].size == size, __func__, "a level's data is missing or has the wrong size."))
                return false;
        }

        const bool filled = rhiTexture->hostCopy ? impl->device->fillTexture(rhiTexture, levels, levelCount) : impl->uploads.uploadTexture(rhiTexture, levels, levelCount);
        if (!filled)
            return false;
        rhiTexture->state = TextureUsage::SAMPLED;
        return true;
    }

    bool RenderContext::supportsTextureFormat(Format format) const
    {
        return impl->device && impl->device->supportsTextureFormat(format);
    }

    uint64_t RenderContext::getBufferDeviceAddress(const BufferHandle &buffer)
    {
        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        if (!rhiBuffer)
            return 0;

        if (rhiBuffer->deviceAddress == 0)
        {
            spdlog::error("Buffer '{}' has no device address.", rhiBuffer->desc.name);
            return 0;
        }
        return rhiBuffer->deviceAddress;
    }

    BufferDesc RenderContext::getBufferDesc(const BufferHandle &buffer)
    {
        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        return rhiBuffer ? rhiBuffer->desc : BufferDesc{};
    }

    TextureDesc RenderContext::getTextureDesc(const TextureHandle &texture)
    {
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        return rhiTexture ? rhiTexture->desc : TextureDesc{};
    }

    VRAMStats RenderContext::queryVRAMStats()
    {
        return impl->device->queryVRAMStats();
    }

    uint32_t RenderContext::updateBindlessDescriptorSet(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, SamplerType sampler, uint32_t slot)
    {
        if (!checkArgument(sampler < SamplerType::COUNT, __func__, "invalid sampler type."))
            return UINT32_MAX;

        if (slot > MAX_BINDLESS_TEXTURES || (slot == MAX_BINDLESS_TEXTURES && impl->nextBindlessTextureIndex >= MAX_BINDLESS_TEXTURES))
        {
            spdlog::error("Slot index exceeds maximum bindless textures.");
            return UINT32_MAX;
        }

        auto *descriptorSet = lookup(impl->descriptorSets, bindlessSet, __func__);
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        if (!descriptorSet || !rhiTexture)
            return UINT32_MAX;

        const uint32_t targetSlot = slot == MAX_BINDLESS_TEXTURES ? impl->nextBindlessTextureIndex : slot;
        impl->device->updateBindlessTexture(descriptorSet, targetSlot, rhiTexture, impl->samplers[static_cast<size_t>(sampler)]);

        // Explicit slots count too: automatic allocation must never land on one of them.
        impl->nextBindlessTextureIndex = std::max(impl->nextBindlessTextureIndex, targetSlot + 1);
        return targetSlot;
    }

    // A storage slot holds one mip of a texture, written by compute in the STORAGE state. A slot the GPU may still use
    // must not be rewritten: the caller waits for the frames that used it.
    bool RenderContext::updateBindlessStorageImage(const DescriptorSetHandle &bindlessSet, uint32_t slot, const TextureHandle &texture, uint32_t mip)
    {
        auto *descriptorSet = lookup(impl->descriptorSets, bindlessSet, __func__);
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        if (!descriptorSet || !rhiTexture || !checkArgument(slot < descriptorSet->storageImageCount, __func__, "the slot is past the set layout's storageImageCount.") ||
            !checkArgument(rhiTexture->desc.usage == TextureUsage::SAMPLED_STORAGE, __func__, "the texture is not a SAMPLED_STORAGE texture.") ||
            !checkArgument(mip < rhiTexture->mipLevels, __func__, "the texture has no such mip."))
            return false;
        impl->device->updateBindlessStorageImage(descriptorSet, slot, rhiTexture, mip);
        return true;
    }

    TextureUsage RenderContext::getTextureState(const TextureHandle &texture)
    {
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        return rhiTexture ? rhiTexture->state : TextureUsage::UNKNOWN;
    }

    uint64_t RenderContext::getFenceValue(FenceHandle fence)
    {
        auto *rhiFence = lookup(impl->fences, fence, __func__);
        return rhiFence ? impl->device->getFenceValue(rhiFence) : 0;
    }

    bool RenderContext::waitForFence(FenceHandle fence, uint64_t value, uint64_t timeout)
    {
        ZoneScoped;

        auto *rhiFence = lookup(impl->fences, fence, __func__);
        if (!rhiFence)
            return false;
        return impl->device->waitForFence(rhiFence, value, timeout);
    }

    SwapchainStatus RenderContext::acquireNextImage(uint32_t &imageIndex)
    {
        ZoneScoped;

        const SwapchainStatus status = impl->swapchain->acquireNextImage(imageIndex);
        if (status != SwapchainStatus::OK && status != SwapchainStatus::SUBOPTIMAL)
            return status;

        // An acquired image holds nothing worth keeping, and the transition out of UNKNOWN waits for the acquire.
        auto *image = imageIndex < impl->swapchainImages.size() ? lookup(impl->textures, impl->swapchainImages[imageIndex], __func__) : nullptr;
        if (!image)
            return SwapchainStatus::FAILURE;
        image->state = TextureUsage::UNKNOWN;
        return status;
    }

    bool RenderContext::beginCommandBuffer(const CommandBufferHandle &commandBuffer)
    {
        impl->skipDraws = false;
        impl->computeBound = false;
        impl->insidePass = false;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        return rhiCommandBuffer && rhiCommandBuffer->begin();
    }

    bool RenderContext::endCommandBuffer(const CommandBufferHandle &commandBuffer)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        return rhiCommandBuffer && rhiCommandBuffer->end();
    }

    bool RenderContext::transitionImageLayout(const CommandBufferHandle &commandBuffer, TextureHandle texture, TextureUsage newUsage, bool discard)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        if (!rhiCommandBuffer || !rhiTexture || !checkArgument(!impl->insidePass, __func__, "barriers must be recorded outside a render pass.") ||
            !checkArgument(newUsage != TextureUsage::STORAGE || rhiTexture->desc.usage == TextureUsage::SAMPLED_STORAGE, __func__, "only a SAMPLED_STORAGE texture can be a storage image."))
            return false;
        return impl->textureStates.transition(*rhiCommandBuffer, rhiTexture, newUsage, discard);
    }

    bool RenderContext::beginRendering(const CommandBufferHandle &commandBuffer, const TextureHandle *colorAttachments, uint32_t colorCount, TextureHandle depthAttachment, const glm::vec4 &clearColor, const AttachmentOps &ops, uint32_t face)
    {
        // A depth-only pass (a shadow map) has no colors.
        if (!checkArgument(colorCount <= MAX_COLOR_ATTACHMENTS, __func__, "a pass has at most MAX_COLOR_ATTACHMENTS color attachments.") ||
            !checkArgument(colorCount > 0 || depthAttachment.isValid(), __func__, "a pass needs a color or a depth attachment.") ||
            !checkArgument(face < CUBE_FACES, __func__, "a cube has CUBE_FACES faces."))
            return false;
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        // An invalid depth handle means the pass has no depth attachment.
        rhi::Texture *depth = depthAttachment.isValid() ? lookup(impl->textures, depthAttachment, __func__) : nullptr;
        rhi::Texture *colors[MAX_COLOR_ATTACHMENTS] = {};
        for (uint32_t i = 0; i < colorCount; ++i)
        {
            colors[i] = lookup(impl->textures, colorAttachments[i], __func__);
            if (!colors[i])
                return false;
        }
        if (!rhiCommandBuffer || (depthAttachment.isValid() && !depth) ||
            !checkArgument(face == 0 || (depth && depth->desc.cube), __func__, "only a cube depth has faces other than 0."))
            return false;

        rhiCommandBuffer->beginRendering(colors, colorCount, depth, clearColor, ops, face);
        for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENTS; ++i)
            impl->passColorFormats[i] = i < colorCount ? colors[i]->desc.format : Format::UNKNOWN;
        impl->passDepthFormat = depth ? depth->desc.format : Format::UNKNOWN;
        impl->skipDraws = false;
        impl->insidePass = true;
        return true;
    }

    bool RenderContext::endRendering(const CommandBufferHandle &commandBuffer)
    {
        impl->insidePass = false;
        std::fill(std::begin(impl->passColorFormats), std::end(impl->passColorFormats), Format::UNKNOWN);
        impl->passDepthFormat = Format::UNKNOWN;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!rhiCommandBuffer)
            return false;

        rhiCommandBuffer->endRendering();
        return true;
    }

    bool RenderContext::setViewport(const CommandBufferHandle &commandBuffer, float x, float y, float width, float height, float minDepth, float maxDepth)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!rhiCommandBuffer)
            return false;

        rhiCommandBuffer->setViewport(x, y, width, height, minDepth, maxDepth);
        impl->viewport = {.x = x, .y = y, .width = width, .height = height, .minDepth = minDepth, .maxDepth = maxDepth};
        return true;
    }

    const Viewport &RenderContext::getViewport() const
    {
        return impl->viewport;
    }

    bool RenderContext::setScissor(const CommandBufferHandle &commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!rhiCommandBuffer)
            return false;

        rhiCommandBuffer->setScissor(x, y, width, height);
        return true;
    }

    bool RenderContext::setCullMode(const CommandBufferHandle &commandBuffer, CullMode cullMode)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!rhiCommandBuffer)
            return false;

        rhiCommandBuffer->setCullMode(cullMode);
        return true;
    }

    bool RenderContext::submit(const SubmitInfo &info)
    {
        ZoneScoped;

        // They run in this order on the queue, as one submission.
        if (!checkArgument(info.commandBufferCount <= rhi::MAX_SUBMIT_COMMAND_BUFFERS, __func__, "too many command buffers."))
            return false;
        // A compute submit keeps one wait for the uploads.
        const bool computeQueue = info.queue == QueueType::COMPUTE && impl->device->hasAsyncCompute();
        if (!checkArgument(info.waitCount + (computeQueue ? 1 : 0) <= rhi::MAX_SUBMIT_WAITS, __func__, "too many fence waits."))
            return false;
        rhi::CommandBuffer *rhiCommandBuffers[rhi::MAX_SUBMIT_COMMAND_BUFFERS] = {};
        for (uint32_t i = 0; i < info.commandBufferCount; ++i)
        {
            rhiCommandBuffers[i] = lookup(impl->commandBuffers, info.commandBuffers[i], __func__);
            if (!rhiCommandBuffers[i])
                return false;
        }

        rhi::FenceWait waits[rhi::MAX_SUBMIT_WAITS] = {};
        uint32_t waitCount = 0;
        for (uint32_t i = 0; i < info.waitCount; ++i)
        {
            auto *fence = lookup(impl->fences, info.waits[i].fence, __func__);
            if (!fence)
                return false;
            waits[waitCount++] = {.fence = fence, .value = info.waits[i].value};
        }

        rhi::Fence *signalFence = nullptr;
        if (info.signal.fence.isValid())
        {
            signalFence = lookup(impl->fences, info.signal.fence, __func__);
            if (!signalFence)
                return false;
        }

        // Uploads go first, on the graphics queue, so this submit sees their data: the same queue through the
        // barriers in the copies, the compute queue by waiting for them. Every upload submit also waits for the
        // last compute submit, which may still read what the copies overwrite (UploadManager::waitFor). A failed
        // batch is logged and retried with the next submit; the frame still goes out so the swapchain keeps moving.
        impl->uploads.flush();
        const rhi::FenceWait uploads = impl->uploads.lastSubmit();
        if (computeQueue && uploads.value > 0)
            waits[waitCount++] = uploads;

        rhi::SubmitDesc submitDesc = {
            .commandBuffers = rhiCommandBuffers,
            .commandBufferCount = info.commandBufferCount,
            .waits = waits,
            .waitCount = waitCount,
            .acquireSwapchain = info.waitsForSwapchainImage ? impl->swapchain : nullptr,
            .presentSwapchain = info.presentsSwapchainImage ? impl->swapchain : nullptr,
            .signalFence = signalFence,
            .signalValue = info.signal.value,
        };
        if (!impl->device->getQueue(info.queue)->submit(submitDesc))
            return false;
        if (computeQueue && signalFence != nullptr)
            impl->uploads.waitFor({.fence = signalFence, .value = info.signal.value});

        // Only what these command buffers recorded is on the GPU now; a frame may have more submits to go.
        impl->textureStates.submitted(rhiCommandBuffers, info.commandBufferCount);
        std::erase_if(impl->recordedClears, [&rhiCommandBuffers, &info](const HistoryClear &clear)
                      { return std::find(rhiCommandBuffers, rhiCommandBuffers + info.commandBufferCount, clear.commandBuffer) != rhiCommandBuffers + info.commandBufferCount; });
        return true;
    }

    bool RenderContext::hasAsyncCompute() const
    {
        return impl->device->hasAsyncCompute();
    }

    // For commands that were recorded but will not run, a failed submit included: the textures go back to
    // the states they had before them, which is still what the GPU has.
    void RenderContext::discardRecording()
    {
        impl->textureStates.discardUnsubmitted();
        impl->pendingClears.insert(impl->pendingClears.end(), impl->recordedClears.begin(), impl->recordedClears.end());
        impl->recordedClears.clear();
    }

    SwapchainStatus RenderContext::present()
    {
        ZoneScoped;

        return impl->swapchain->present();
    }

    SwapchainStatus RenderContext::recreateSwapchain()
    {
        const auto &window = engine->getMainWindow();
        auto status = impl->swapchain->resize(window.pixelWidth, window.pixelHeight);
        if (status == SwapchainStatus::FAILURE)
            spdlog::error("Failed to recreate the swapchain.");

        // Only a success replaces the images. A failure either kept the old swapchain or left none,
        // and in the latter case its handles must go too (B5).
        if (status == SwapchainStatus::OK || impl->swapchain->getImageCount() == 0)
        {
            impl->unregisterSwapchainImages();
            impl->registerSwapchainImages(engine->handleManager);
        }
        return status;
    }

    void RenderContext::bindPipeline(const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiPipeline = lookup(impl->pipelines, pipeline, __func__);
        if (!rhiCommandBuffer || !rhiPipeline)
        {
            // Whatever was bound before is not the pipeline the caller wants to draw or dispatch with.
            impl->skipDraws = true;
            impl->computeBound = false;
            return;
        }

        // Compute has its own bind point and no attachments, so it leaves the pass's graphics pipeline alone.
        const auto &desc = rhiPipeline->desc;
        if (desc.computeShader.isValid())
        {
            rhiCommandBuffer->bindPipeline(rhiPipeline);
            impl->computeBound = true;
            return;
        }

        if (!std::equal(std::begin(desc.colorAttachmentFormats), std::end(desc.colorAttachmentFormats), std::begin(impl->passColorFormats)) ||
            desc.depthStencilAttachmentFormat != impl->passDepthFormat)
        {
            spdlog::error("Pipeline '{}' does not match the formats of the pass's attachments, so its draws are skipped.", desc.name);
            impl->skipDraws = true;
            return;
        }

        rhiCommandBuffer->bindPipeline(rhiPipeline);
        impl->skipDraws = false;
    }

    void RenderContext::bindIndexBuffer(const CommandBufferHandle &commandBuffer, const BufferHandle &buffer, uint64_t offset, Format indexType)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        if (rhiCommandBuffer && rhiBuffer)
            rhiCommandBuffer->bindIndexBuffer(rhiBuffer, offset, indexType);
    }

    void RenderContext::bindDescriptorSet(const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline, const DescriptorSetHandle &descriptorSet, uint32_t setIndex)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiPipeline = lookup(impl->pipelines, pipeline, __func__);
        auto *rhiDescriptorSet = lookup(impl->descriptorSets, descriptorSet, __func__);
        if (rhiCommandBuffer && rhiPipeline && rhiDescriptorSet)
            rhiCommandBuffer->bindDescriptorSet(rhiPipeline, rhiDescriptorSet, setIndex);
    }

    void RenderContext::pushConstants(const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline, ShaderType stage, const void *data, uint32_t size, uint32_t offset)
    {
        // Every pipeline's push-constant range is PUSH_CONSTANTS_SIZE bytes (C12).
        if (offset + size > PUSH_CONSTANTS_SIZE)
        {
            spdlog::error("pushConstants: {} bytes at offset {} do not fit in the {}-byte range.", size, offset, PUSH_CONSTANTS_SIZE);
            return;
        }

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiPipeline = lookup(impl->pipelines, pipeline, __func__);
        if (!rhiCommandBuffer || !rhiPipeline)
            return;

        // The stages must be exactly the ones of the pipeline's push-constant range.
        const ShaderType layoutStage = rhiPipeline->desc.computeShader.isValid() ? ShaderType::COMPUTE : ShaderType::VERTEX_FRAGMENT;
        if (!checkArgument(stage == layoutStage, __func__, "the stage must be COMPUTE for a compute pipeline and VERTEX_FRAGMENT for a graphics pipeline."))
            return;
        rhiCommandBuffer->pushConstants(rhiPipeline, stage, data, size, offset);
    }

    void RenderContext::draw(const CommandBufferHandle &commandBuffer, uint32_t vertexCount, uint32_t instanceCount)
    {
        if (!checkArgument(impl->insidePass, __func__, "draws must be recorded inside a render pass."))
            return;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!impl->skipDraws && rhiCommandBuffer)
            rhiCommandBuffer->draw(vertexCount, instanceCount);
    }

    void RenderContext::drawIndexed(const CommandBufferHandle &commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t vertexOffset, uint32_t firstIndex, uint32_t firstInstance)
    {
        if (!checkArgument(impl->insidePass, __func__, "draws must be recorded inside a render pass."))
            return;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!impl->skipDraws && rhiCommandBuffer)
            rhiCommandBuffer->drawIndexed(indexCount, instanceCount, vertexOffset, firstIndex, firstInstance);
    }

    void RenderContext::drawIndexedIndirect(const CommandBufferHandle &commandBuffer, const BufferHandle &drawInfoBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride)
    {
        if (!checkArgument(impl->insidePass, __func__, "draws must be recorded inside a render pass."))
            return;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiBuffer = lookup(impl->buffers, drawInfoBuffer, __func__);
        if (!impl->skipDraws && rhiCommandBuffer && rhiBuffer)
            rhiCommandBuffer->drawIndexedIndirect(rhiBuffer, offset, drawCount, stride);
    }

    void RenderContext::drawIndexedIndirectCount(const CommandBufferHandle &commandBuffer, const BufferHandle &drawInfoBuffer, uint64_t offset, const BufferHandle &countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride)
    {
        if (!checkArgument(impl->insidePass, __func__, "draws must be recorded inside a render pass."))
            return;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiBuffer = lookup(impl->buffers, drawInfoBuffer, __func__);
        auto *rhiCountBuffer = lookup(impl->buffers, countBuffer, __func__);
        if (!impl->skipDraws && rhiCommandBuffer && rhiBuffer && rhiCountBuffer)
            rhiCommandBuffer->drawIndexedIndirectCount(rhiBuffer, offset, rhiCountBuffer, countOffset, maxDrawCount, stride);
    }

    void RenderContext::dispatch(const CommandBufferHandle &commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        if (!checkArgument(!impl->insidePass, __func__, "dispatches must be recorded outside a render pass (use RenderGraph::addComputePass)."))
            return;

        if (!checkArgument(impl->computeBound, __func__, "no compute pipeline is bound."))
            return;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (rhiCommandBuffer)
            rhiCommandBuffer->dispatch(groupCountX, groupCountY, groupCountZ);
    }

    void RenderContext::aliasingBarrier(const CommandBufferHandle &commandBuffer)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (rhiCommandBuffer && checkArgument(!impl->insidePass, __func__, "barriers must be recorded outside a render pass."))
            rhiCommandBuffer->aliasingBarrier();
    }

    void RenderContext::memoryBarrier(const CommandBufferHandle &commandBuffer, PipelineStage before, PipelineStage after)
    {
        if (!checkArgument(!impl->insidePass, __func__, "barriers must be recorded outside a render pass."))
            return;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (rhiCommandBuffer)
            rhiCommandBuffer->memoryBarrier(before, after);
    }
}
