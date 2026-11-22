#include "rasm/render/renderContext.h"
#include "rasm/engine.h"
#include "rhi/device.h"
#include "rhi/memoryAliasing.h"
#include "rhi/mipChain.h"
#include "rhi/offscreenSwapchain.h"
#include "rhi/releaseQueue.h"
#include "rhi/textureStates.h"
#include "rhi/uploadManager.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <deque>
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
            bool alive = false; // the slot holds a target; destroyed and never used ones don't
        };

        // Transient render targets laid out together in heaps, from the passes that use each one.
        struct TransientGroup
        {
            std::vector<RenderTargetHandle> targets;
            std::vector<uint32_t> firstPasses;
            std::vector<uint32_t> lastPasses;
            std::vector<rhi::Heap *> heaps;
        };

        struct BufferTable
        {
            HandlePool handles;
            std::vector<rhi::Buffer *> objects; // by handle index; null when free
        };

        struct TextureTable
        {
            HandlePool handles;
            std::vector<rhi::Texture *> objects; // by handle index; null when free
            std::vector<uint32_t> slots;         // its bindless slot, or UINT32_MAX
            std::vector<uint8_t> attachments;    // 1 for a render target's texture, which only the target destroys
        };

        struct ShaderTable
        {
            HandlePool handles;
            std::vector<rhi::Shader *> objects; // by handle index; null when free
        };

        struct PipelineTable
        {
            HandlePool handles;
            std::vector<rhi::Pipeline *> objects; // by handle index; null when free
        };

        struct CommandBufferTable
        {
            HandlePool handles;
            std::vector<rhi::CommandBuffer *> objects; // by handle index; null when free
        };

        struct FenceTable
        {
            HandlePool handles;
            std::vector<rhi::Fence *> objects; // by handle index; null when free
        };

        struct QueryPoolTable
        {
            HandlePool handles;
            std::vector<rhi::QueryPool *> objects; // by handle index; null when free
        };

        struct DescriptorSetLayoutTable
        {
            HandlePool handles;
            std::vector<rhi::DescriptorSetLayout *> objects; // by handle index; null when free
        };

        struct DescriptorPoolTable
        {
            HandlePool handles;
            std::vector<rhi::DescriptorPool *> objects; // by handle index; null when free
        };

        struct DescriptorSetTable
        {
            HandlePool handles;
            std::vector<rhi::DescriptorSet *> objects; // by handle index; null when free
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
        // Samples per pixel a render target or pipeline may have: 1, 2, 4 or 8 (the pipeline sets of the scene passes,
        // gpuScene.h's SAMPLE_VARIANTS), and only a count the device has. deviceCounts has bit n set for 2^n samples.
        constexpr uint32_t MAX_SAMPLES = 8;

        bool isSampleCount(uint32_t samples, uint32_t deviceCounts)
        {
            return samples <= MAX_SAMPLES && std::has_single_bit(samples) && (deviceCounts & samples) != 0;
        }

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
        // it has colors and no depth. Its colors are R16G16B16A16_SFLOAT, the format shaders declare binding 1 with
        // ([format("rgba16f")]).
        bool checkRenderTargetDesc(const RenderTargetDesc &desc, uint32_t sampleCounts, const char *caller)
        {
            // A multisampled target is a scene's color and depth, resolved by a compute pass that reads its samples: so
            // it has colors, and no history, storage or cube, which hold or write single samples.
            if (!checkArgument(isSampleCount(desc.samples, sampleCounts), caller, "a render target has 1, 2, 4 or 8 samples per pixel, of those the device has (supportsSampleCount).") ||
                !checkArgument(desc.samples == 1 || (colorAttachmentCount(desc.colorFormats) > 0 && !desc.history && !desc.storage && !desc.cube), caller,
                               "a multisampled render target has colors, and no history, storage or cube."))
                return false;
            const uint32_t colorCount = colorAttachmentCount(desc.colorFormats);
            const bool contiguous = std::all_of(desc.colorFormats + colorCount, std::end(desc.colorFormats), [](Format format)
                                                { return format == Format::UNKNOWN; });
            if (desc.storage)
                return checkArgument(contiguous, caller, "a render target's color formats can't have a gap.") &&
                       checkArgument(colorCount > 0 && desc.depthFormat == Format::UNKNOWN && !desc.cube, caller, "a storage render target has colors and no depth.") &&
                       checkArgument(std::all_of(desc.colorFormats, desc.colorFormats + colorCount, [](Format format)
                                                 { return format == Format::R16G16B16A16_SFLOAT; }),
                                     caller, "a storage render target's colors are R16G16B16A16_SFLOAT, the format of binding 1 in the shaders.");
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
                    .usage = desc.storage ? TextureUsage::SAMPLED_STORAGE : TextureUsage::SAMPLED_COLOR_ATTACHMENT,
                    .samples = desc.samples,
                    .readable = desc.readable};
        }

        // Everything but the textures, which createAttachments makes, and the storage slots, which
        // Impl::allocateStorageSlots gives a storage target.
        RenderTarget renderTargetFromDesc(const RenderTargetDesc &desc, glm::uvec2 size)
        {
            RenderTarget rt = {.width = size.x, .height = size.y, .colorCount = colorAttachmentCount(desc.colorFormats), .depthFormat = desc.depthFormat, .clearColor = desc.clearColor, .history = desc.history, .samples = desc.samples};
            std::copy(std::begin(desc.colorFormats), std::end(desc.colorFormats), std::begin(rt.colorFormats));
            for (auto &slots : rt.storageSlots)
                std::fill(std::begin(slots), std::end(slots), UINT32_MAX);
            return rt;
        }

        // A D32 depth is sampled through the bindless set: by the Hi-Z pass of occlusion culling, by TAA when the target
        // has a MOTION_FORMAT color (the nearest depth around a pixel), and by the readers of a depth-only target's depth
        // (a shadow map). Other depths are not, so they keep the GPU's depth compression. A depth/stencil view can't be
        // sampled.
        TextureDesc depthTextureDesc(const RenderTargetDesc &desc, glm::uvec2 size, bool occlusionCulling)
        {
            const uint32_t colorCount = colorAttachmentCount(desc.colorFormats);
            const bool motion = colorCount == 2 && desc.colorFormats[1] == MOTION_FORMAT;
            const bool sampled = (occlusionCulling || colorCount == 0 || motion) && desc.depthFormat == Format::D32_SFLOAT;
            return {.name = desc.name + " Depth Texture",
                    .width = size.x,
                    .height = size.y,
                    .format = desc.depthFormat,
                    .usage = sampled ? TextureUsage::SAMPLED_DEPTH_ATTACHMENT : TextureUsage::DEPTH_STENCIL_ATTACHMENT,
                    .cube = desc.cube,
                    .samples = desc.samples,
                    .readable = desc.readable};
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

        // A stale handle (its object was destroyed, maybe its slot reused) fails like one that was never made.
        bool checkHandle(const HandlePool &pool, uint64_t index, uint64_t generation, const char *caller, const char *kind)
        {
            if (pool.isAlive(index, generation))
                return true;
            spdlog::error("{}: {} handle {{{}, {}}} is invalid or was destroyed.", caller, kind, index, generation);
            return false;
        }

        rhi::Buffer *lookup(const BufferTable &table, const BufferHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "buffer") ? table.objects[handle.index] : nullptr;
        }

        rhi::Texture *lookup(const TextureTable &table, const TextureHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "texture") ? table.objects[handle.index] : nullptr;
        }

        rhi::Shader *lookup(const ShaderTable &table, const ShaderHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "shader") ? table.objects[handle.index] : nullptr;
        }

        rhi::Pipeline *lookup(const PipelineTable &table, const PipelineHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "pipeline") ? table.objects[handle.index] : nullptr;
        }

        rhi::CommandBuffer *lookup(const CommandBufferTable &table, const CommandBufferHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "command buffer") ? table.objects[handle.index] : nullptr;
        }

        rhi::Fence *lookup(const FenceTable &table, const FenceHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "fence") ? table.objects[handle.index] : nullptr;
        }

        rhi::QueryPool *lookup(const QueryPoolTable &table, const QueryPoolHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "query pool") ? table.objects[handle.index] : nullptr;
        }

        rhi::DescriptorSetLayout *lookup(const DescriptorSetLayoutTable &table, const DescriptorSetLayoutHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "descriptor set layout") ? table.objects[handle.index] : nullptr;
        }

        rhi::DescriptorPool *lookup(const DescriptorPoolTable &table, const DescriptorPoolHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "descriptor pool") ? table.objects[handle.index] : nullptr;
        }

        rhi::DescriptorSet *lookup(const DescriptorSetTable &table, const DescriptorSetHandle &handle, const char *caller)
        {
            return checkHandle(table.handles, handle.index, handle.generation, caller, "descriptor set") ? table.objects[handle.index] : nullptr;
        }
    }

    // clang-format off
    struct RenderContext::Impl
    {
        std::unique_ptr<rhi::Device>                                                                                            device;
        rhi::Swapchain*                                                                                                         swapchain       = nullptr;
        std::unique_ptr<rhi::OffscreenSwapchain>                                                                                offscreen;          // headless: swapchain points to it
        rhi::Sampler*                                                                                                           samplers[static_cast<size_t>(SamplerType::COUNT)] = {};
        rhi::UploadManager                                                                                                      uploads;
        rhi::TextureStates                                                                                                      textureStates;
        std::vector<TextureHandle>                                                                                              swapchainImages;  // also in textures, but owned by the swapchain

        BufferTable                                                                                                             buffers;
        TextureTable                                                                                                            textures;
        ShaderTable                                                                                                             shaders;
        PipelineTable                                                                                                           pipelines;
        CommandBufferTable                                                                                                      commandBuffers;
        FenceTable                                                                                                              fences;
        QueryPoolTable                                                                                                          queryPools;
        DescriptorSetLayoutTable                                                                                                descriptorSetLayouts;
        DescriptorPoolTable                                                                                                     descriptorPools;
        DescriptorSetTable                                                                                                      descriptorSets;
        HandlePool                                                                                                              renderTargetHandles;
        std::deque<RenderTarget>                                                                                                renderTargets;  // by handle index; a deque, so a reference survives a new target
        std::deque<RenderTargetInfo>                                                                                            renderTargetInfos;
        rhi::ReleaseQueue                                                                                                       releases;  // what destroy* freed, once the GPU is done (tagReleases, collectReleases)
        rhi::Fence*                                                                                                             releaseFences[rhi::RELEASE_TIMELINES - 1] = {};  // the frame and compute fences of the last tagReleases
        std::vector<TransientGroup>                                                                                             transientGroups;
        std::vector<HistoryClear>                                                                                               pendingClears;  // made since the last frame began
        std::vector<HistoryClear>                                                                                               recordedClears; // in a frame not yet submitted

        // Dedup caches, only for immutable objects keyed on their full state. Mutable resources
        // (buffers, textures, descriptor pools/sets) are always created fresh (B1).
        std::unordered_map<ShaderDesc, ShaderHandle, ShaderDescHash>                                                            shaderDescCache;
        std::unordered_map<PipelineDesc, PipelineHandle, PipelineDescHash>                                                      pipelineDescCache;
        std::unordered_map<BindlessDescriptorSetLayoutDesc, DescriptorSetLayoutHandle, BindlessDescriptorSetLayoutDescHash>     descriptorSetLayoutDescCache;

        // Binding 0 slots: never used ones from nextBindlessTextureIndex up, freed ones in freeBindlessSlots once the GPU
        // is done with their last texture. slotTextures names each slot's texture, the one whose destroy frees it.
        uint32_t                                                                                                                nextBindlessTextureIndex = 0;
        std::vector<uint32_t>                                                                                                   freeBindlessSlots;
        TextureHandle                                                                                                           slotTextures[MAX_BINDLESS_TEXTURES] = {};
        SamplerType                                                                                                             slotSamplers[MAX_BINDLESS_TEXTURES] = {};
        bool                                                                                                                    slotWaiting[MAX_BINDLESS_TEXTURES] = {};  // released, the GPU may still sample it
        // Binding 1 slots of storage render targets, from RESERVED_STORAGE_IMAGES up. A destroyed target gives its back.
        uint32_t                                                                                                                nextStorageSlot = RESERVED_STORAGE_IMAGES;
        std::vector<uint32_t>                                                                                                   freeStorageSlots;

        // The current pass's attachment formats. A pipeline must match them for its draws to be defined.
        Format                                                                                                                  passColorFormats[MAX_COLOR_ATTACHMENTS] = {};
        Format                                                                                                                  passDepthFormat = Format::UNKNOWN;
        uint32_t                                                                                                                passSamples     = 1;
        // Recording state of the command buffer being recorded. beginCommandBuffer resets it.
        bool                                                                                                                    skipDraws       = false; // a mismatched or missing pipeline was rejected
        bool                                                                                                                    computeBound    = false; // a compute pipeline is bound, so dispatches are defined
        bool                                                                                                                    insidePass      = false; // between beginRendering and endRendering, where draws are allowed and dispatches and barriers are not
        Viewport                                                                                                                viewport;                // the last setViewport

        BufferHandle addBuffer(rhi::Buffer* buffer)
        {
            const HandleSlot slot = buffers.handles.allocate();
            buffers.objects.resize(buffers.handles.size(), nullptr);
            buffers.objects[slot.index] = buffer;
            return {slot.index, slot.generation};
        }

        TextureHandle addTexture(rhi::Texture* texture)
        {
            const HandleSlot slot = textures.handles.allocate();
            textures.objects.resize(textures.handles.size(), nullptr);
            textures.slots.resize(textures.handles.size(), UINT32_MAX);
            textures.attachments.resize(textures.handles.size(), 0);
            textures.objects[slot.index] = texture;
            textures.slots[slot.index] = UINT32_MAX;
            textures.attachments[slot.index] = 0;
            return {slot.index, slot.generation};
        }

        bool isRenderTarget(const RenderTargetHandle& handle) const
        {
            return renderTargetHandles.isAlive(handle.index, handle.generation);
        }

        RenderTargetHandle addRenderTarget(const RenderTarget& target, const RenderTargetInfo& info)
        {
            const HandleSlot slot = renderTargetHandles.allocate();
            renderTargets.resize(renderTargetHandles.size());
            renderTargetInfos.resize(renderTargetHandles.size());
            renderTargets[slot.index] = target;
            renderTargetInfos[slot.index] = info;
            renderTargetInfos[slot.index].alive = true;
            return {slot.index, slot.generation};
        }

        // A slot nobody uses: a freed one first, so the set does not run out while textures come and go.
        uint32_t allocateBindlessSlot()
        {
            if (!freeBindlessSlots.empty())
            {
                const uint32_t slot = freeBindlessSlots.back();
                freeBindlessSlots.pop_back();
                return slot;
            }
            return nextBindlessTextureIndex < MAX_BINDLESS_TEXTURES ? nextBindlessTextureIndex++ : UINT32_MAX;
        }

        // The frames submitted so far, and the one being recorded, may still sample the slot's old texture.
        void releaseBindlessSlot(uint32_t slot)
        {
            slotTextures[slot] = {};
            slotWaiting[slot] = true;
            releases.release([this, slot]()
                             {
                                 slotWaiting[slot] = false;
                                 freeBindlessSlots.push_back(slot); });
        }

        // The texture keeps no slot, so its destroy frees none: a new texture takes the slot (createAttachments).
        uint32_t takeBindlessSlot(const TextureHandle& texture)
        {
            if (!textures.handles.isAlive(texture.index, texture.generation) || textures.slots[texture.index] == UINT32_MAX)
                return UINT32_MAX;
            const uint32_t slot = textures.slots[texture.index];
            textures.slots[texture.index] = UINT32_MAX;
            slotTextures[slot] = {};
            return slot;
        }

        // The handle dies now; the texture and its slot once the GPU is done with every frame that may use them.
        void retireTexture(const TextureHandle& texture)
        {
            rhi::Texture* rhiTexture = textures.objects[texture.index];
            if (textures.slots[texture.index] != UINT32_MAX)
                releaseBindlessSlot(textures.slots[texture.index]);
            textures.slots[texture.index] = UINT32_MAX;
            textures.objects[texture.index] = nullptr;
            textures.handles.release(texture.index, texture.generation);
            releases.release([this, rhiTexture]()
                             {
                                 textureStates.forget(rhiTexture);
                                 uploads.cancelUploads(rhiTexture);
                                 device->destroyTexture(rhiTexture); });
        }

        void retireBuffer(rhi::Buffer* buffer)
        {
            releases.release([this, buffer]()
                             {
                                 uploads.cancelUploads(buffer);
                                 device->destroyBuffer(buffer); });
        }

        void retireHeap(rhi::Heap* heap)
        {
            releases.release([this, heap]() { device->destroyHeap(heap); });
        }

        void registerSwapchainImages()
        {
            for (uint32_t i = 0; i < swapchain->getImageCount(); ++i)
                swapchainImages.push_back(addTexture(swapchain->getImage(i)));
        }

        // The swapchain owns the images, so only their handles go.
        void unregisterSwapchainImages()
        {
            for (const auto& handle : swapchainImages)
            {
                if (textures.slots[handle.index] != UINT32_MAX)
                    releaseBindlessSlot(textures.slots[handle.index]);
                textures.slots[handle.index] = UINT32_MAX;
                textures.objects[handle.index] = nullptr;
                textures.handles.release(handle.index, handle.generation);
            }
            swapchainImages.clear();
        }

        // A slot per color of each copy, kept for the target's life: a resize or a new placement writes the new
        // textures into the same slots. A target without history has one copy, which both frame slots use. All or none,
        // so a target never has only some of them.
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

        // Each slot is given back once, also when both frame slots share it: at once for a target no frame has used,
        // else once the GPU is done with the frames that may write it (afterGpu).
        void releaseStorageSlots(RenderTarget& renderTarget, bool afterGpu)
        {
            std::vector<uint32_t> slots;
            for (uint32_t c = 0; c < MAX_COLOR_ATTACHMENTS; ++c)
            {
                const uint32_t first = renderTarget.storageSlots[0][c];
                const uint32_t second = renderTarget.storageSlots[1][c];
                if (first != UINT32_MAX)
                    slots.push_back(first);
                if (second != UINT32_MAX && second != first)
                    slots.push_back(second);
            }
            for (auto& targetSlots : renderTarget.storageSlots)
                std::fill(std::begin(targetSlots), std::end(targetSlots), UINT32_MAX);
            if (!afterGpu)
                freeStorageSlots.insert(freeStorageSlots.end(), slots.begin(), slots.end());
            else if (!slots.empty())
                releases.release([this, slots]() { freeStorageSlots.insert(freeStorageSlots.end(), slots.begin(), slots.end()); });
        }
    };
    // clang-format on

    RenderContext::RenderContext(Engine *owner) : engine(owner), impl(std::make_unique<Impl>()) {}

    RenderContext::~RenderContext() = default;

    bool RenderContext::initialize(Backend backend)
    {
        const auto config = engine->getConfig();
        const auto &window = engine->getMainWindow();

        const bool headless = config.platform == PlatformType::HEADLESS;
        impl->device = rhi::createDevice(backend, {.appName = config.appName,
                                                   .window = headless ? nullptr : &window,
                                                   .enableValidation = config.enableValidation,
                                                   .enableSyncValidation = config.enableSyncValidation,
                                                   .enableGpuValidation = config.enableGpuValidation,
                                                   .asyncCompute = config.asyncCompute,
                                                   .hostImageCopy = config.hostImageCopy,
                                                   .cacheDir = config.cacheDir});
        if (!impl->device)
            return false;

        if (!impl->uploads.initialize(*impl->device))
            return false;

        // Without a window the frames render into textures of the device, which present nothing.
        if (headless)
        {
            impl->offscreen = std::make_unique<rhi::OffscreenSwapchain>(*impl->device);
            if (impl->offscreen->initialize({.width = window.pixelWidth, .height = window.pixelHeight}))
                impl->swapchain = impl->offscreen.get();
        }
        else
        {
            impl->swapchain = impl->device->createSwapchain({.width = window.pixelWidth, .height = window.pixelHeight});
        }
        if (!impl->swapchain)
        {
            spdlog::error("Failed to create the swapchain.");
            return false;
        }
        impl->registerSwapchainImages();

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

        // Everything destroyed is freed first, while the objects it may refer to (a heap's textures) still exist.
        impl->releases.releaseAll();
        impl->uploads.cleanup();
        for (auto *descriptorSet : impl->descriptorSets.objects)
            if (descriptorSet)
                device.destroyDescriptorSet(descriptorSet);
        for (auto *pool : impl->descriptorPools.objects)
            if (pool)
                device.destroyDescriptorPool(pool);
        for (auto *rhiCommandBuffer : impl->commandBuffers.objects)
            if (rhiCommandBuffer)
                device.destroyCommandBuffer(rhiCommandBuffer);
        for (auto *fence : impl->fences.objects)
            if (fence)
                device.destroyFence(fence);
        for (auto *pool : impl->queryPools.objects)
            if (pool)
                device.destroyQueryPool(pool);
        for (auto *buffer : impl->buffers.objects)
            if (buffer)
                device.destroyBuffer(buffer);
        impl->unregisterSwapchainImages();
        impl->releases.releaseAll();
        for (auto *texture : impl->textures.objects)
            if (texture)
                device.destroyTexture(texture);
        for (const auto &group : impl->transientGroups)
            for (auto *heap : group.heaps)
                device.destroyHeap(heap);
        for (auto *pipeline : impl->pipelines.objects)
            if (pipeline)
                device.destroyPipeline(pipeline);
        for (auto *shader : impl->shaders.objects)
            if (shader)
                device.destroyShader(shader);
        // After the pipelines: the pipeline layout they share is the set layout's.
        for (auto *layout : impl->descriptorSetLayouts.objects)
            if (layout)
                device.destroyDescriptorSetLayout(layout);
        for (auto *sampler : impl->samplers)
            if (sampler)
                device.destroySampler(sampler);
        if (impl->offscreen)
            impl->offscreen.reset();
        else if (impl->swapchain)
            device.destroySwapchain(impl->swapchain);

        impl = std::make_unique<Impl>();
    }

    BufferHandle RenderContext::createBuffer(const BufferDesc &desc)
    {
        auto *buffer = impl->device->createBuffer(desc);
        if (!buffer)
            return {};

        return impl->addBuffer(buffer);
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
        // Samples are drawn: only an attachment has more than one, with one mip and one layer.
        const bool attachment = depthAttachment || desc.usage == TextureUsage::COLOR_ATTACHMENT || desc.usage == TextureUsage::SAMPLED_COLOR_ATTACHMENT;
        if (!checkArgument(desc.samples == 1 || (supportsSampleCount(desc.samples) && attachment && desc.mipLevels <= 1 && !desc.cube), __func__,
                           "only a color or depth attachment of one mip, not a cube, has more than one sample, and only a count the device supports."))
            return {};
        auto *texture = impl->device->createTexture(desc);
        if (!texture)
            return {};

        return impl->addTexture(texture);
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

        const HandleSlot slot = impl->shaders.handles.allocate();
        impl->shaders.objects.resize(impl->shaders.handles.size(), nullptr);
        impl->shaders.objects[slot.index] = shader;
        const ShaderHandle handle{slot.index, slot.generation};
        impl->shaderDescCache[desc] = handle;
        return handle;
    }

    PipelineHandle RenderContext::createPipeline(const PipelineDesc &desc)
    {
        const bool compute = desc.computeShader.isValid();
        if (!checkArgument(!compute || (!desc.vertexShader.isValid() && !desc.fragmentShader.isValid()), __func__, "a pipeline has either a compute shader or vertex and fragment shaders.") ||
            !checkArgument(compute || isSampleCount(desc.samples, impl->device->supportedSampleCounts()), __func__,
                           "a graphics pipeline has 1, 2, 4 or 8 samples per pixel, of those the device has (supportsSampleCount)."))
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

        const HandleSlot slot = impl->pipelines.handles.allocate();
        impl->pipelines.objects.resize(impl->pipelines.handles.size(), nullptr);
        impl->pipelines.objects[slot.index] = pipeline;
        const PipelineHandle handle{slot.index, slot.generation};
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

        const HandleSlot slot = impl->descriptorSetLayouts.handles.allocate();
        impl->descriptorSetLayouts.objects.resize(impl->descriptorSetLayouts.handles.size(), nullptr);
        impl->descriptorSetLayouts.objects[slot.index] = layout;
        const DescriptorSetLayoutHandle handle{slot.index, slot.generation};
        impl->descriptorSetLayoutDescCache[desc] = handle;
        return handle;
    }

    DescriptorPoolHandle RenderContext::createDescriptorPool(const DescriptorPoolDesc &desc)
    {
        auto *pool = impl->device->createDescriptorPool(desc);
        if (!pool)
            return {};

        const HandleSlot slot = impl->descriptorPools.handles.allocate();
        impl->descriptorPools.objects.resize(impl->descriptorPools.handles.size(), nullptr);
        impl->descriptorPools.objects[slot.index] = pool;
        const DescriptorPoolHandle handle{slot.index, slot.generation};
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

        const HandleSlot slot = impl->descriptorSets.handles.allocate();
        impl->descriptorSets.objects.resize(impl->descriptorSets.handles.size(), nullptr);
        impl->descriptorSets.objects[slot.index] = descriptorSet;
        const DescriptorSetHandle handle{slot.index, slot.generation};
        return handle;
    }

    CommandBufferHandle RenderContext::createCommandBuffer(const std::string &name, QueueType queue)
    {
        auto *rhiCommandBuffer = impl->device->createCommandBuffer(queue, name);
        if (!rhiCommandBuffer)
            return {};

        const HandleSlot slot = impl->commandBuffers.handles.allocate();
        impl->commandBuffers.objects.resize(impl->commandBuffers.handles.size(), nullptr);
        impl->commandBuffers.objects[slot.index] = rhiCommandBuffer;
        const CommandBufferHandle handle{slot.index, slot.generation};
        return handle;
    }

    FenceHandle RenderContext::createFence(const std::string &name, uint64_t initialValue)
    {
        auto *fence = impl->device->createFence(initialValue, name);
        if (!fence)
            return {};

        const HandleSlot slot = impl->fences.handles.allocate();
        impl->fences.objects.resize(impl->fences.handles.size(), nullptr);
        impl->fences.objects[slot.index] = fence;
        const FenceHandle handle{slot.index, slot.generation};
        return handle;
    }

    QueryPoolHandle RenderContext::createTimestampQueryPool(const std::string &name, uint32_t count)
    {
        auto *pool = impl->device->createTimestampQueryPool(count, name);
        if (!pool)
            return {};

        const HandleSlot slot = impl->queryPools.handles.allocate();
        impl->queryPools.objects.resize(impl->queryPools.handles.size(), nullptr);
        impl->queryPools.objects[slot.index] = pool;
        const QueryPoolHandle handle{slot.index, slot.generation};
        return handle;
    }

    RenderTargetHandle RenderContext::createRenderTarget(const RenderTargetDesc &desc)
    {
        if (!checkRenderTargetDesc(desc, impl->device->supportedSampleCounts(), __func__))
            return {};

        RenderTarget rt = renderTargetFromDesc(desc, renderTargetSize(desc, getSwapchainExtent()));
        if (!impl->allocateStorageSlots(rt, desc))
            return {};
        if (!createAttachments(rt, desc, nullptr, nullptr))
        {
            impl->releaseStorageSlots(rt, false);
            return {};
        }

        return impl->addRenderTarget(rt, {.desc = desc});
    }

    // The handle is usable at once, but the textures exist only after placeTransientRenderTargets.
    RenderTargetHandle RenderContext::createTransientRenderTarget(const RenderTargetDesc &desc)
    {
        if (!checkArgument(!desc.history, __func__, "a history render target lives across frames, so it can't be transient.") ||
            !checkArgument(!desc.readable, __func__, "a readable render target is read after the frame, so it can't share memory as a transient one does.") ||
            !checkRenderTargetDesc(desc, impl->device->supportedSampleCounts(), __func__))
            return {};

        RenderTarget rt = renderTargetFromDesc(desc, renderTargetSize(desc, getSwapchainExtent()));
        if (!impl->allocateStorageSlots(rt, desc))
            return {};
        return impl->addRenderTarget(rt, {.desc = desc, .transient = true});
    }

    // A group that already holds one of the targets is replaced, after the GPU is done with its memory.
    bool RenderContext::placeTransientRenderTargets(const RenderTargetHandle *renderTargets, const uint32_t *firstPasses, const uint32_t *lastPasses, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!checkArgument(impl->isRenderTarget(renderTargets[i]) && impl->renderTargetInfos[renderTargets[i].index].transient, __func__, "only transient render targets can be placed."))
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
                    destroyAttachments(impl->renderTargets[target.index]);
            }
            retired.insert(retired.end(), group.heaps.begin(), group.heaps.end());
            impl->transientGroups.erase(impl->transientGroups.begin() + static_cast<std::ptrdiff_t>(g));
        }

        impl->transientGroups.push_back({.targets = std::vector<RenderTargetHandle>(renderTargets, end),
                                         .firstPasses = std::vector<uint32_t>(firstPasses, firstPasses + count),
                                         .lastPasses = std::vector<uint32_t>(lastPasses, lastPasses + count)});
        const bool placed = placeGroup(static_cast<uint32_t>(impl->transientGroups.size() - 1));

        // placeGroup destroyed the targets' old textures, so nothing lives in these heaps once they are freed.
        for (auto *heap : retired)
            impl->retireHeap(heap);
        return placed;
    }

    // A render graph's targets, transient or with history, go with the graph. Their handles die at once, and their
    // textures, slots and heaps once the GPU is done with the frames that may use them, so nothing waits here. A
    // transient group none of whose targets is left frees its heaps, after the textures placed in them.
    void RenderContext::destroyRenderTargets(const RenderTargetHandle *renderTargets, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!impl->isRenderTarget(renderTargets[i]))
                continue;
            RenderTarget &renderTarget = impl->renderTargets[renderTargets[i].index];
            destroyAttachments(renderTarget);
            impl->releaseStorageSlots(renderTarget, true);
            renderTarget = {};
            impl->renderTargetInfos[renderTargets[i].index] = {};
            impl->renderTargetHandles.release(renderTargets[i].index, renderTargets[i].generation);
            // Its index may go to a new target at once, so no group may name it any more.
            for (auto &group : impl->transientGroups)
            {
                for (size_t t = group.targets.size(); t-- > 0;)
                {
                    if (!(group.targets[t] == renderTargets[i]))
                        continue;
                    group.targets.erase(group.targets.begin() + static_cast<std::ptrdiff_t>(t));
                    group.firstPasses.erase(group.firstPasses.begin() + static_cast<std::ptrdiff_t>(t));
                    group.lastPasses.erase(group.lastPasses.begin() + static_cast<std::ptrdiff_t>(t));
                }
            }
        }

        std::erase_if(impl->transientGroups, [this](const TransientGroup &group)
                      {
                          const bool empty = group.targets.empty();
                          if (empty)
                              for (auto *heap : group.heaps)
                                  impl->retireHeap(heap);
                          return empty; });
    }

    void RenderContext::destroyRenderTarget(const RenderTargetHandle &renderTarget)
    {
        if (!checkArgument(impl->isRenderTarget(renderTarget), __func__, "the render target is invalid or was destroyed."))
            return;
        destroyRenderTargets(&renderTarget, 1);
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
        for (uint64_t i = 1; i < impl->renderTargetHandles.size(); ++i)
        {
            const RenderTargetInfo &info = impl->renderTargetInfos[i];
            if (info.alive && !info.transient && info.desc.swapchainScale > 0.0f)
                createAttachments(impl->renderTargets[i], info.desc, nullptr, nullptr);
        }
        for (uint32_t g = 0; g < impl->transientGroups.size(); ++g)
        {
            const auto &targets = impl->transientGroups[g].targets;
            const bool scaled = std::any_of(targets.begin(), targets.end(), [this](const RenderTargetHandle &target)
                                            { return impl->renderTargetInfos[target.index].desc.swapchainScale > 0.0f; });
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
            const uint32_t textureCount = attachmentTextureDescs(impl->renderTargetInfos[group.targets[i].index].desc, extent, engine->config.occlusionCulling, textureDescs);
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
            auto &rt = impl->renderTargets[group.targets[i].index];
            const auto &desc = impl->renderTargetInfos[group.targets[i].index].desc;
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
            impl->retireHeap(heap);
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

        // The old textures give up their slots before they are destroyed, so the slots stay taken for the new ones.
        auto slotOf = [this](TextureHandle texture)
        {
            const uint32_t slot = impl->takeBindlessSlot(texture);
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
        // On failure, the taken slots that no new texture holds go back to the set.
        auto releaseTakenSlots = [this, &colorSlots, &depthSlots]()
        {
            std::vector<uint32_t> released; // frame slots without history share their slots
            for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
            {
                for (uint32_t c = 0; c <= MAX_COLOR_ATTACHMENTS; ++c)
                {
                    const uint32_t slot = c < MAX_COLOR_ATTACHMENTS ? colorSlots[i][c] : depthSlots[i];
                    if (slot < MAX_BINDLESS_TEXTURES && !impl->slotTextures[slot].isValid() && std::find(released.begin(), released.end(), slot) == released.end())
                    {
                        released.push_back(slot);
                        impl->releaseBindlessSlot(slot);
                    }
                }
            }
        };
        destroyAttachments(renderTarget);
        renderTarget.width = textureDescs[0].width;
        renderTarget.height = textureDescs[0].height;

        auto makeTexture = [this, heaps, offsets](const TextureDesc &textureDesc, uint32_t attachment) -> TextureHandle
        {
            TextureHandle handle;
            if (!heaps)
                handle = createTexture(textureDesc);
            else if (auto *texture = impl->device->createPlacedTexture(textureDesc, heaps[attachment], offsets[attachment]))
                handle = impl->addTexture(texture);
            if (handle.isValid())
                impl->textures.attachments[handle.index] = 1;
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
                releaseTakenSlots();
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
                releaseTakenSlots();
                destroyAttachments(renderTarget);
                return false;
            }
        }
        for (uint32_t i = copies; i < MAX_FRAMES_IN_FLIGHT; ++i)
            renderTarget.attachments[i] = renderTarget.attachments[0];

        // Each frame reads the copy the frame before drew, so on the first frame that copy holds nothing yet. A copy is
        // cleared by drawing into it; a storage target's colors are not attachments, so its writer starts it over
        // instead (addTaaPass drops its history on its first frame and after a resize).
        if (desc.history && !desc.storage)
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
            if (impl->textures.handles.isAlive(texture.index, texture.generation))
                impl->retireTexture(texture);
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
        if (!impl->isRenderTarget(handle))
        {
            spdlog::error("Render target handle {{{}, {}}} is invalid or was destroyed.", handle.index, handle.generation);
            static RenderTarget emptyRenderTarget{};
            return emptyRenderTarget;
        }
        return impl->renderTargets[handle.index];
    }

    RenderTargetDesc RenderContext::getRenderTargetDesc(const RenderTargetHandle &handle)
    {
        if (!impl->isRenderTarget(handle))
        {
            spdlog::error("Render target handle {{{}, {}}} is invalid or was destroyed.", handle.index, handle.generation);
            return {};
        }
        return impl->renderTargetInfos[handle.index].desc;
    }

    void RenderContext::destroyTexture(const TextureHandle &texture)
    {
        if (std::find(impl->swapchainImages.begin(), impl->swapchainImages.end(), texture) != impl->swapchainImages.end())
        {
            spdlog::error("Swapchain images belong to the swapchain and can't be destroyed.");
            return;
        }

        if (!lookup(impl->textures, texture, __func__))
            return;
        if (!checkArgument(impl->textures.attachments[texture.index] == 0, __func__, "the texture is a render target's; destroy the render target (destroyRenderTarget) instead."))
            return;
        impl->retireTexture(texture);
    }

    void RenderContext::destroyBuffer(const BufferHandle &buffer)
    {
        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        if (!rhiBuffer)
            return;
        impl->buffers.objects[buffer.index] = nullptr;
        impl->buffers.handles.release(buffer.index, buffer.generation);
        impl->retireBuffer(rhiBuffer);
    }

    // A new buffer with the old desc and the new size, under the same handle, so whatever holds the handle (a render
    // graph's declarations) stays valid. The contents and the device address change; the old buffer is freed once the
    // GPU is done with the frames that may use it. On failure the old buffer stays.
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
        impl->retireBuffer(oldBuffer);
        impl->buffers.objects[buffer.index] = newBuffer;
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

    Format RenderContext::getSwapchainEncodedFormat()
    {
        return impl->swapchain->getEncodedFormat();
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

    bool RenderContext::supportsSampleCount(uint32_t samples) const
    {
        return impl->device && isSampleCount(samples, impl->device->supportedSampleCounts());
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

    // A texture holds one slot. Asked for any slot (MAX_BINDLESS_TEXTURES), it keeps the one it has, with the new
    // sampler, or takes a free one. Asked for a slot, it takes that one from the texture that held it, which then has
    // none, and gives back the one it had. A slot is free again once the GPU is done with its last texture.
    uint32_t RenderContext::updateBindlessDescriptorSet(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, SamplerType sampler, uint32_t slot)
    {
        if (!checkArgument(sampler < SamplerType::COUNT, __func__, "invalid sampler type.") ||
            !checkArgument(slot <= MAX_BINDLESS_TEXTURES, __func__, "the slot is past MAX_BINDLESS_TEXTURES."))
            return UINT32_MAX;

        auto *descriptorSet = lookup(impl->descriptorSets, bindlessSet, __func__);
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        if (!descriptorSet || !rhiTexture)
            return UINT32_MAX;

        uint32_t &textureSlot = impl->textures.slots[texture.index];
        // A frame in flight may sample the slot, so its descriptor stays as it is.
        if (slot == MAX_BINDLESS_TEXTURES && textureSlot != UINT32_MAX)
        {
            if (impl->slotSamplers[textureSlot] != sampler)
                spdlog::error("updateBindlessDescriptorSet: texture '{}' already has slot {} with another sampler; a texture holds one slot. Make a second texture to sample it another way.",
                              rhiTexture->desc.name, textureSlot);
            return textureSlot;
        }
        if (!checkArgument(slot == MAX_BINDLESS_TEXTURES || !impl->slotWaiting[slot], __func__, "the slot was freed, and frames in flight may still sample its old texture."))
            return UINT32_MAX;
        uint32_t targetSlot = slot;
        if (slot == MAX_BINDLESS_TEXTURES)
        {
            targetSlot = impl->allocateBindlessSlot();
            if (targetSlot == UINT32_MAX)
            {
                spdlog::error("All {} bindless texture slots are in use ({} more wait for the GPU to finish with them). Destroy textures that are no longer needed.",
                              MAX_BINDLESS_TEXTURES, impl->releases.waitingCount());
                return UINT32_MAX;
            }
        }
        else
        {
            // Never handed out yet: the slots below it stay free for automatic allocation.
            for (; impl->nextBindlessTextureIndex <= slot; ++impl->nextBindlessTextureIndex)
            {
                if (impl->nextBindlessTextureIndex != slot)
                    impl->freeBindlessSlots.push_back(impl->nextBindlessTextureIndex);
            }
            std::erase(impl->freeBindlessSlots, slot);
            const TextureHandle owner = impl->slotTextures[slot];
            if (owner.isValid() && !(owner == texture) && impl->textures.handles.isAlive(owner.index, owner.generation))
                impl->textures.slots[owner.index] = UINT32_MAX;
            if (textureSlot != UINT32_MAX && textureSlot != slot)
                impl->releaseBindlessSlot(textureSlot);
        }

        impl->device->updateBindlessTexture(descriptorSet, targetSlot, rhiTexture, impl->samplers[static_cast<size_t>(sampler)]);
        textureSlot = targetSlot;
        impl->slotTextures[targetSlot] = texture;
        impl->slotSamplers[targetSlot] = sampler;
        return targetSlot;
    }

    uint32_t RenderContext::getBindlessTextureIndex(const TextureHandle &texture) const
    {
        if (!impl->textures.handles.isAlive(texture.index, texture.generation))
            return UINT32_MAX;
        return impl->textures.slots[texture.index];
    }

    bool RenderContext::isAlive(const TextureHandle &texture) const
    {
        return impl->textures.handles.isAlive(texture.index, texture.generation);
    }

    bool RenderContext::isAlive(const BufferHandle &buffer) const
    {
        return impl->buffers.handles.isAlive(buffer.index, buffer.generation);
    }

    // Called at the end of every frame, after its submits, also for a frame that was dropped: what was destroyed since
    // the last call waits for the values the queues and the uploads signaled last. The graphics value covers the frame's
    // compute work (its last graphics batch waits for it), but a frame whose later batches failed leaves compute work
    // the graphics value does not cover, so the compute value counts too.
    void RenderContext::tagReleases(const FenceValue &graphics, const FenceValue &compute)
    {
        impl->releaseFences[0] = graphics.fence.isValid() ? lookup(impl->fences, graphics.fence, __func__) : nullptr;
        impl->releaseFences[1] = compute.fence.isValid() ? lookup(impl->fences, compute.fence, __func__) : nullptr;
        const uint64_t values[rhi::RELEASE_TIMELINES] = {impl->releaseFences[0] ? graphics.value : 0, impl->releaseFences[1] ? compute.value : 0,
                                                         impl->uploads.lastSubmit().value};
        impl->releases.tag(values);
    }

    void RenderContext::releaseAfterGpu(std::function<void()> free)
    {
        impl->releases.release(std::move(free));
    }

    uint32_t RenderContext::getReleasesWaiting() const
    {
        return impl->releases.waitingCount();
    }

    uint32_t RenderContext::getTextureCount() const
    {
        return static_cast<uint32_t>(impl->textures.handles.liveCount());
    }

    uint32_t RenderContext::getBindlessTextureCount() const
    {
        uint32_t count = 0;
        for (const TextureHandle &texture : impl->slotTextures)
            count += texture.isValid() ? 1 : 0;
        return count;
    }

    // Frees what every queue is done with. Nothing waits: the fence values are only read.
    void RenderContext::collectReleases()
    {
        if (!impl->releases.hasTagged())
            return;
        ZoneScoped;
        const rhi::FenceWait uploads = impl->uploads.lastSubmit();
        const uint64_t completed[rhi::RELEASE_TIMELINES] = {impl->releaseFences[0] ? impl->device->getFenceValue(impl->releaseFences[0]) : UINT64_MAX,
                                                            impl->releaseFences[1] ? impl->device->getFenceValue(impl->releaseFences[1]) : UINT64_MAX,
                                                            uploads.fence ? impl->device->getFenceValue(uploads.fence) : UINT64_MAX};
        impl->releases.collect(completed);
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

    // The texture keeps its state: it goes to TRANSFER_SRC and back inside the copy.
    bool RenderContext::copyTextureToBuffer(const CommandBufferHandle &commandBuffer, const TextureHandle &texture, uint32_t layer, uint32_t mip, const BufferHandle &buffer)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        if (!rhiCommandBuffer || !rhiTexture || !rhiBuffer || !checkArgument(!impl->insidePass, __func__, "copies must be recorded outside a render pass.") ||
            !checkArgument(rhiTexture->desc.readable, __func__, "the texture is not readable (TextureDesc::readable, RenderTargetDesc::readable).") ||
            !checkArgument(rhiTexture->desc.samples == 1, __func__, "a multisampled texture can't be copied; copy its resolve.") ||
            !checkArgument(layer < (rhiTexture->desc.cube ? CUBE_FACES : 1) && mip < rhiTexture->mipLevels, __func__, "the layer or mip is past the texture's."))
            return false;
        const TextureUsage state = rhiTexture->state;
        if (!checkArgument(state != TextureUsage::UNKNOWN && state != TextureUsage::PRESENT_SRC, __func__, "the texture holds nothing to copy yet."))
            return false;
        // A depth/stencil texture gives only its depth, 4 bytes a texel.
        const TextureDesc &desc = rhiTexture->desc;
        const uint32_t width = std::max(1u, desc.width >> mip);
        const uint32_t height = std::max(1u, desc.height >> mip);
        const bool depthStencil = desc.format == Format::D32_SFLOAT_S8_UINT || desc.format == Format::D24_UNORM_S8_UINT;
        const uint64_t bytes = depthStencil ? uint64_t{4} * width * height : rhi::getTextureLevelSize(desc.format, width, height);
        if (!checkArgument(bytes > 0 && rhiBuffer->desc.size >= bytes, __func__, "the buffer is smaller than the copy."))
            return false;
        rhiCommandBuffer->copyFromTexture(rhiTexture, state, layer, mip, rhiBuffer, 0);
        return true;
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

        // Vulkan draws every attachment of a pass with one sample count.
        const uint32_t samples = colorCount > 0 ? colors[0]->desc.samples : depth->desc.samples;
        bool sameSamples = !depth || depth->desc.samples == samples;
        for (uint32_t i = 0; i < colorCount; ++i)
            sameSamples = sameSamples && colors[i]->desc.samples == samples;
        if (!checkArgument(sameSamples, __func__, "every attachment of a pass has the same samples per pixel."))
            return false;

        rhiCommandBuffer->beginRendering(colors, colorCount, depth, clearColor, ops, face);
        for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENTS; ++i)
            impl->passColorFormats[i] = i < colorCount ? colors[i]->desc.format : Format::UNKNOWN;
        // A swapchain image or a texture with TextureDesc::encodedView drawn through its UNORM view takes pipelines made
        // for that view's format.
        const bool swapchainColor = colorCount > 0 && std::find(impl->swapchainImages.begin(), impl->swapchainImages.end(), colorAttachments[0]) != impl->swapchainImages.end();
        if (ops.encodedColor && swapchainColor && impl->swapchain->getEncodedFormat() != Format::UNKNOWN)
            impl->passColorFormats[0] = impl->swapchain->getEncodedFormat();
        else if (ops.encodedColor && colorCount > 0 && colors[0]->desc.encodedView && colors[0]->desc.format == Format::R8G8B8A8_SRGB && !colors[0]->desc.cube)
            impl->passColorFormats[0] = Format::R8G8B8A8_UNORM;
        impl->passDepthFormat = depth ? depth->desc.format : Format::UNKNOWN;
        impl->passSamples = samples;
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
            .acquireSwapchain = info.waitsForSwapchainImage && !impl->offscreen ? impl->swapchain : nullptr,
            .presentSwapchain = info.presentsSwapchainImage && !impl->offscreen ? impl->swapchain : nullptr,
            .signalFence = signalFence,
            .signalValue = info.signal.value,
        };
        if (!impl->device->getQueue(info.queue)->submit(submitDesc))
            return false;
        // The present submit is the image's last use this frame, on the one timeline of the graphics queue.
        if (impl->offscreen && info.presentsSwapchainImage && signalFence != nullptr)
            impl->offscreen->submitted(signalFence, info.signal.value);
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
            impl->registerSwapchainImages();
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
            desc.depthStencilAttachmentFormat != impl->passDepthFormat || desc.samples != impl->passSamples)
        {
            spdlog::error("Pipeline '{}' does not match the formats or the sample count of the pass's attachments, so its draws are skipped.", desc.name);
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

    void RenderContext::pushConstants(const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline, const void *data, uint32_t size, uint32_t offset)
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

        rhiCommandBuffer->pushConstants(rhiPipeline, data, size, offset);
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
