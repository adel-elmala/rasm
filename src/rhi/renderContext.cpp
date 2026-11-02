#include "rasm/render/renderContext.h"
#include "rasm/engine.h"
#include "rhi/device.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cassert>
#include <deque>
#include <string>
#include <unordered_map>

namespace rasm
{
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
        std::unique_ptr<rhi::Device>                                                            device;
        rhi::Swapchain*                                                                         swapchain       = nullptr;
        rhi::Sampler*                                                                           defaultSampler  = nullptr;
        std::vector<TextureHandle>                                                              swapchainImages;  // also in textures, but owned by the swapchain

        std::unordered_map<BufferHandle, rhi::Buffer*, HandleHash>                              buffers;
        std::unordered_map<TextureHandle, rhi::Texture*, HandleHash>                            textures;
        std::unordered_map<ShaderHandle, rhi::Shader*, HandleHash>                              shaders;
        std::unordered_map<PipelineHandle, rhi::Pipeline*, HandleHash>                          pipelines;
        std::unordered_map<CommandBufferHandle, rhi::CommandBuffer*, HandleHash>                commandBuffers;
        std::unordered_map<FenceHandle, rhi::Fence*, HandleHash>                                fences;
        std::unordered_map<DescriptorSetLayoutHandle, rhi::DescriptorSetLayout*, HandleHash>    descriptorSetLayouts;
        std::unordered_map<DescriptorPoolHandle, rhi::DescriptorPool*, HandleHash>              descriptorPools;
        std::unordered_map<DescriptorSetHandle, rhi::DescriptorSet*, HandleHash>                descriptorSets;
        std::unordered_map<RenderTargetHandle, RenderTarget, HandleHash>                        renderTargets;

        // Dedup caches, only for immutable objects keyed on their full state. Mutable resources
        // (buffers, textures, descriptor pools/sets) are always created fresh (B1).
        std::unordered_map<ResourceDesc, ShaderHandle, ResourceDescHash>                        shaderDescCache;
        std::deque<std::string>                                                                 shaderSources;  // SPIR-V the shaderDescCache keys point at; a deque keeps those pointers stable
        std::unordered_map<ResourceDesc, PipelineHandle, ResourceDescHash>                      pipelineDescCache;
        std::unordered_map<ResourceDesc, DescriptorSetLayoutHandle, ResourceDescHash>           descriptorSetLayoutDescCache;

        uint32_t                                                                                nextBindlessTextureIndex = 0;

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
    };
    // clang-format on

    RenderContext::RenderContext(Engine *owner) : engine(owner), impl(std::make_unique<Impl>()) {}

    RenderContext::~RenderContext() = default;

    bool RenderContext::initialize(Backend backend)
    {
        const auto config = engine->getConfig();
        const auto &window = engine->getMainWindow();

        impl->device = rhi::createDevice(backend, {.appName = config.appName, .window = &window, .enableValidation = config.enableValidation});
        if (!impl->device)
            return false;

        impl->swapchain = impl->device->createSwapchain({.width = window.pixelWidth, .height = window.pixelHeight});
        if (!impl->swapchain)
        {
            spdlog::error("Failed to create the swapchain.");
            return false;
        }
        impl->registerSwapchainImages(engine->handleManager);

        // Every bindless texture is sampled through this one sampler.
        impl->defaultSampler = impl->device->createSampler({.name = "Default Sampler", .maxAnisotropy = 8.0f});
        if (!impl->defaultSampler)
            return false;

        return true;
    }

    void RenderContext::cleanup()
    {
        if (!impl->device)
            return;

        auto &device = *impl->device;
        device.waitIdle();

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
        for (const auto &[handle, buffer] : impl->buffers)
            device.destroyBuffer(buffer);
        impl->unregisterSwapchainImages();
        for (const auto &[handle, texture] : impl->textures)
            device.destroyTexture(texture);
        for (const auto &[handle, pipeline] : impl->pipelines)
            device.destroyPipeline(pipeline);
        for (const auto &[handle, shader] : impl->shaders)
            device.destroyShader(shader);
        if (impl->defaultSampler)
            device.destroySampler(impl->defaultSampler);
        if (impl->swapchain)
            device.destroySwapchain(impl->swapchain);

        impl = std::make_unique<Impl>();
    }

    BufferHandle RenderContext::createBuffer(ResourceDesc desc)
    {
        if (!checkArgument(desc.type == ResourceType::BUFFER, __func__, "desc.type must be BUFFER."))
            return {};

        auto *buffer = impl->device->createBuffer(desc);
        if (!buffer)
            return {};

        BufferHandle handle = engine->handleManager.getNextBufferHandle();
        impl->buffers[handle] = buffer;
        return handle;
    }

    TextureHandle RenderContext::createTexture(ResourceDesc desc)
    {
        if (!checkArgument(desc.type == ResourceType::TEXTURE, __func__, "desc.type must be TEXTURE."))
            return {};

        auto *texture = impl->device->createTexture(desc);
        if (!texture)
            return {};

        TextureHandle handle = engine->handleManager.getNextTextureHandle();
        impl->textures[handle] = texture;
        return handle;
    }

    ShaderHandle RenderContext::createShader(ResourceDesc desc)
    {
        if (!checkArgument(desc.type == ResourceType::SHADER, __func__, "desc.type must be SHADER.") ||
            !checkArgument(desc.shader.source != nullptr && desc.shader.sourceSize > 0, __func__, "the shader has no source."))
            return {};

        auto cachedIt = impl->shaderDescCache.find(desc);
        if (cachedIt != impl->shaderDescCache.end())
            return cachedIt->second;

        auto *shader = impl->device->createShader(desc);
        if (!shader)
            return {};

        ShaderHandle handle = engine->handleManager.getNextShaderHandle();

        // Callers pass temporary SPIR-V, but the cache key is compared by content on every later lookup.
        impl->shaderSources.emplace_back(desc.shader.source, desc.shader.sourceSize);
        desc.shader.source = impl->shaderSources.back().data();
        shader->desc.shader.source = desc.shader.source;

        impl->shaders[handle] = shader;
        impl->shaderDescCache[desc] = handle;
        return handle;
    }

    PipelineHandle RenderContext::createPipeline(ResourceDesc desc)
    {
        if (!checkArgument(desc.type == ResourceType::GRAPHICS_PIPELINE || desc.type == ResourceType::COMPUTE_PIPELINE, __func__, "desc.type must be GRAPHICS_PIPELINE or COMPUTE_PIPELINE."))
            return {};

        auto cachedIt = impl->pipelineDescCache.find(desc);
        if (cachedIt != impl->pipelineDescCache.end())
            return cachedIt->second;

        // TODO: compute pipelines (phase 2).
        if (desc.type != ResourceType::GRAPHICS_PIPELINE)
        {
            spdlog::error("Only graphics pipelines are supported.");
            return {};
        }

        auto *vertexShader = lookup(impl->shaders, desc.pipeline.vertexShader, __func__);
        auto *fragmentShader = lookup(impl->shaders, desc.pipeline.fragmentShader, __func__);
        auto *layout = lookup(impl->descriptorSetLayouts, desc.pipeline.descriptorSetLayout, __func__);
        if (!vertexShader || !fragmentShader || !layout)
            return {};

        auto *pipeline = impl->device->createGraphicsPipeline(desc, vertexShader, fragmentShader, layout);
        if (!pipeline)
            return {};

        PipelineHandle handle = engine->handleManager.getNextPipelineHandle();
        impl->pipelines[handle] = pipeline;
        impl->pipelineDescCache[desc] = handle;
        return handle;
    }

    DescriptorSetLayoutHandle RenderContext::createBindlessDescriptorSetLayout(ResourceDesc desc)
    {
        if (!checkArgument(desc.type == ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT, __func__, "desc.type must be BINDLESS_DESCRIPTOR_SET_LAYOUT."))
            return {};

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

    DescriptorPoolHandle RenderContext::createDescriptorPool(ResourceDesc desc)
    {
        if (!checkArgument(desc.type == ResourceType::DESCRIPTOR_POOL, __func__, "desc.type must be DESCRIPTOR_POOL."))
            return {};

        auto *pool = impl->device->createDescriptorPool(desc);
        if (!pool)
            return {};

        DescriptorPoolHandle handle = engine->handleManager.getNextDescriptorPoolHandle();
        impl->descriptorPools[handle] = pool;
        return handle;
    }

    DescriptorSetHandle RenderContext::allocateDescriptorSet(ResourceDesc desc, const DescriptorPoolHandle &pool, const DescriptorSetLayoutHandle &layout)
    {
        if (!checkArgument(desc.type == ResourceType::DESCRIPTOR_SET, __func__, "desc.type must be DESCRIPTOR_SET."))
            return {};

        auto *rhiPool = lookup(impl->descriptorPools, pool, __func__);
        auto *rhiLayout = lookup(impl->descriptorSetLayouts, layout, __func__);
        if (!rhiPool || !rhiLayout)
            return {};

        auto *descriptorSet = impl->device->allocateDescriptorSet(desc, rhiPool, rhiLayout);
        if (!descriptorSet)
            return {};

        DescriptorSetHandle handle = engine->handleManager.getNextDescriptorSetHandle();
        impl->descriptorSets[handle] = descriptorSet;
        return handle;
    }

    CommandBufferHandle RenderContext::createCommandBuffer(const std::string &name)
    {
        auto *rhiCommandBuffer = impl->device->createCommandBuffer(rhi::QueueType::GRAPHICS, name);
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

    RenderTargetHandle RenderContext::createRenderTarget(const ResourceDesc &desc)
    {
        if (!checkArgument(desc.type == ResourceType::RENDER_TARGET, __func__, "desc.type must be RENDER_TARGET."))
            return {};

        ResourceDesc colorDesc{};
        colorDesc.type = ResourceType::TEXTURE;
        colorDesc.name = desc.name + " Color Texture";
        colorDesc.texture.width = desc.renderTarget.width;
        colorDesc.texture.height = desc.renderTarget.height;
        colorDesc.texture.format = desc.renderTarget.colorFormat;
        colorDesc.texture.usage = TextureUsage::SAMPLED_COLOR_ATTACHMENT;

        ResourceDesc depthDesc{};
        depthDesc.type = ResourceType::TEXTURE;
        depthDesc.name = desc.name + " Depth Texture";
        depthDesc.texture.width = desc.renderTarget.width;
        depthDesc.texture.height = desc.renderTarget.height;
        depthDesc.texture.format = desc.renderTarget.depthFormat;
        depthDesc.texture.usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT;

        RenderTarget rt = {
            .attachments = {},
            .width = desc.renderTarget.width,
            .height = desc.renderTarget.height,
            .colorFormat = desc.renderTarget.colorFormat,
            .depthFormat = desc.renderTarget.depthFormat,
            .clearColor = desc.renderTarget.clearColor};

        auto destroyAttachments = [&]()
        {
            for (const auto &attachment : rt.attachments)
            {
                if (attachment.color.isValid())
                {
                    engine->registry.bindlessTextureIndexMap.erase(attachment.color);
                    destroyTexture(attachment.color);
                }
                if (attachment.depth.isValid())
                    destroyTexture(attachment.depth);
            }
        };

        for (auto &attachment : rt.attachments)
        {
            attachment.color = createTexture(colorDesc);
            attachment.depth = createTexture(depthDesc);
            if (!attachment.color.isValid() || !attachment.depth.isValid())
            {
                spdlog::error("Failed to create the attachments of render target '{}'.", desc.name);
                destroyAttachments();
                return {};
            }
        }

        // Only once every attachment exists, so that a failed creation leaves the bindless set untouched.
        for (const auto &attachment : rt.attachments)
        {
            if (engine->addBindlessTexture(engine->getBindlessDescriptorSet(), attachment.color) == UINT32_MAX)
            {
                spdlog::error("Failed to make render target '{}' bindless.", desc.name);
                destroyAttachments();
                return {};
            }
        }

        RenderTargetHandle rtHandle = engine->handleManager.getNextRenderTargetHandle();
        impl->renderTargets[rtHandle] = rt;
        return rtHandle;
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
        impl->device->destroyTexture(rhiTexture);
        impl->textures.erase(texture);
    }

    void RenderContext::destroyBuffer(const BufferHandle &buffer)
    {
        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        if (!rhiBuffer)
            return;
        impl->device->destroyBuffer(rhiBuffer);
        impl->buffers.erase(buffer);
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
        if (!rhiBuffer)
            return false;
        return impl->device->fillBuffer(rhiBuffer, data, size, offset);
    }

    bool RenderContext::fillTexture(const TextureHandle &texture, const void *data)
    {
        ZoneScoped;

        if (!checkArgument(data != nullptr, __func__, "data must be non-null."))
            return false;

        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        if (!rhiTexture)
            return false;
        return impl->device->fillTexture(rhiTexture, data);
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

    ResourceDesc RenderContext::getResourceDesc(const BufferHandle &buffer)
    {
        auto *rhiBuffer = lookup(impl->buffers, buffer, __func__);
        return rhiBuffer ? rhiBuffer->desc : ResourceDesc{};
    }

    ResourceDesc RenderContext::getResourceDesc(const TextureHandle &texture)
    {
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        return rhiTexture ? rhiTexture->desc : ResourceDesc{};
    }

    VRAMStats RenderContext::queryVRAMStats()
    {
        return impl->device->queryVRAMStats();
    }

    uint32_t RenderContext::updateBindlessDescriptorSet(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, uint32_t slot)
    {
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
        impl->device->updateBindlessTexture(descriptorSet, targetSlot, rhiTexture, impl->defaultSampler);

        // Explicit slots count too: automatic allocation must never land on one of them.
        impl->nextBindlessTextureIndex = std::max(impl->nextBindlessTextureIndex, targetSlot + 1);
        return targetSlot;
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

        return impl->swapchain->acquireNextImage(imageIndex);
    }

    bool RenderContext::beginCommandBuffer(const CommandBufferHandle &commandBuffer)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        return rhiCommandBuffer && rhiCommandBuffer->begin();
    }

    bool RenderContext::endCommandBuffer(const CommandBufferHandle &commandBuffer)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        return rhiCommandBuffer && rhiCommandBuffer->end();
    }

    bool RenderContext::transitionImageLayout(const CommandBufferHandle &commandBuffer, TextureHandle texture, TextureUsage oldUsage, TextureUsage newUsage)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiTexture = lookup(impl->textures, texture, __func__);
        return rhiCommandBuffer && rhiTexture && rhiCommandBuffer->transitionTexture(rhiTexture, oldUsage, newUsage);
    }

    bool RenderContext::beginRendering(const CommandBufferHandle &commandBuffer, TextureHandle colorAttachment, TextureHandle depthAttachment, const glm::vec4 &clearColor)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *color = lookup(impl->textures, colorAttachment, __func__);
        auto *depth = lookup(impl->textures, depthAttachment, __func__);
        if (!rhiCommandBuffer || !color || !depth)
            return false;

        rhiCommandBuffer->beginRendering(color, depth, clearColor);
        return true;
    }

    bool RenderContext::endRendering(const CommandBufferHandle &commandBuffer)
    {
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
        return true;
    }

    bool RenderContext::setScissor(const CommandBufferHandle &commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!rhiCommandBuffer)
            return false;

        rhiCommandBuffer->setScissor(x, y, width, height);
        return true;
    }

    bool RenderContext::submit(const CommandBufferHandle &commandBuffer, FenceHandle signalFence, uint64_t signalValue, bool presentsSwapchainImage)
    {
        ZoneScoped;

        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (!rhiCommandBuffer)
            return false;

        rhi::Fence *fence = nullptr;
        if (signalFence.isValid())
        {
            fence = lookup(impl->fences, signalFence, __func__);
            if (!fence)
                return false;
        }

        rhi::CommandBuffer *commandBuffers[] = {rhiCommandBuffer};
        rhi::SubmitDesc submitDesc = {
            .commandBuffers = commandBuffers,
            .swapchain = presentsSwapchainImage ? impl->swapchain : nullptr,
            .signalFence = fence,
            .signalValue = signalValue,
        };
        return impl->device->getQueue(rhi::QueueType::GRAPHICS)->submit(submitDesc);
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
        if (rhiCommandBuffer && rhiPipeline)
            rhiCommandBuffer->bindPipeline(rhiPipeline);
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
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiPipeline = lookup(impl->pipelines, pipeline, __func__);
        if (rhiCommandBuffer && rhiPipeline)
            rhiCommandBuffer->pushConstants(rhiPipeline, stage, data, size, offset);
    }

    void RenderContext::draw(const CommandBufferHandle &commandBuffer, uint32_t vertexCount, uint32_t instanceCount)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (rhiCommandBuffer)
            rhiCommandBuffer->draw(vertexCount, instanceCount);
    }

    void RenderContext::drawIndexed(const CommandBufferHandle &commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t vertexOffset, uint32_t firstIndex, uint32_t firstInstance)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        if (rhiCommandBuffer)
            rhiCommandBuffer->drawIndexed(indexCount, instanceCount, vertexOffset, firstIndex, firstInstance);
    }

    void RenderContext::drawIndexedIndirect(const CommandBufferHandle &commandBuffer, const BufferHandle &drawInfoBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride)
    {
        auto *rhiCommandBuffer = lookup(impl->commandBuffers, commandBuffer, __func__);
        auto *rhiBuffer = lookup(impl->buffers, drawInfoBuffer, __func__);
        if (rhiCommandBuffer && rhiBuffer)
            rhiCommandBuffer->drawIndexedIndirect(rhiBuffer, offset, drawCount, stride);
    }
}
