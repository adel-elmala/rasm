#include <algorithm>
#include <cassert>
#include <deque>
#include <string>
#include <unordered_map>

#include "rasm/engine.h"
#include "rasm/render/renderContext.h"
#include "rhi/vulkan/vulkanContext.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

namespace rasm
{
    // clang-format off
    struct RenderContext::Impl
    {
        gfx::VulkanContext                                                                          vulkanContext = {};
        // TODO: make the cache non-vulkan specific.
        std::unordered_map<BufferHandle, gfx::BufferVKHandle, HandleHash>                           bufferCache;
        std::unordered_map<TextureHandle, gfx::TextureVKHandle, HandleHash>                         textureCache;
        std::unordered_map<TextureHandle, gfx::TextureVKHandle, HandleHash>                         swapchainImageCache;
        std::unordered_map<ShaderHandle, gfx::ShaderVKHandle, HandleHash>                           shaderCache;
        std::unordered_map<PipelineHandle, gfx::PipelineVKHandle, HandleHash>                       pipelineCache;
        std::unordered_map<CommandPoolHandle, gfx::CommandPoolVKHandle, HandleHash>                 commandPoolCache;
        std::unordered_map<CommandBufferHandle, gfx::CommandBufferVKHandle, HandleHash>             commandBufferCache;
        std::unordered_map<SemaphoreHandle, gfx::SemaphoreVKHandle, HandleHash>                     semaphoreCache;
        std::unordered_map<FenceHandle, gfx::FenceVKHandle, HandleHash>                             fenceCache;
        std::unordered_map<RenderTargetHandle, RenderTarget, HandleHash>                            renderTargetCache;
        std::unordered_map<DescriptorSetLayoutHandle, gfx::DescriptorSetLayoutVKHandle, HandleHash> descriptorSetLayoutCache;
        std::unordered_map<DescriptorPoolHandle, gfx::DescriptorPoolVKHandle, HandleHash>           descriptorPoolCache;
        std::unordered_map<DescriptorSetHandle, gfx::DescriptorSetVKHandle, HandleHash>             descriptorSetCache;

        // Dedup caches, only for immutable objects keyed on their full state. Mutable resources
        // (buffers, textures, descriptor pools/sets) are always created fresh (B1).
        std::unordered_map<ResourceDesc, ShaderHandle, ResourceDescHash>                            shaderDescCache;
        std::deque<std::string>                                                                     shaderSources;  // SPIR-V the shaderDescCache keys point at; a deque keeps those pointers stable
        std::unordered_map<ResourceDesc, PipelineHandle, ResourceDescHash>                          pipelineDescCache;
        std::unordered_map<ResourceDesc, DescriptorSetLayoutHandle, ResourceDescHash>               descriptorSetLayoutDescCache;

        uint32_t                                                                                    nextBindlessTextureIndex = 0;
    };
    // clang-format on

    RenderContext::RenderContext(Engine *owner) : engine(owner), impl(std::make_unique<Impl>())
    {
        impl->bufferCache.reserve(25);   // Pre-allocate for 25 buffers
        impl->textureCache.reserve(25);  // Pre-allocate for 25 textures
        impl->shaderCache.reserve(25);   // Pre-allocate for 25 shaders
        impl->pipelineCache.reserve(25); // Pre-allocate for 25 pipelines
    }

    RenderContext::~RenderContext() = default;

    bool RenderContext::initialize(Backend backend)
    {
        this->backend = backend;

        switch (backend)
        {
        case Backend::VULKAN:
            impl->vulkanContext = gfx::VulkanContext(engine);
            if (!impl->vulkanContext.initialize())
            {
                spdlog::error("Failed to initialize Vulkan context.");
                return false;
            }
            return true;
        case Backend::DX12:
            spdlog::error("DirectX 12 backend is not implemented yet.");
            return false;
        case Backend::METAL:
            spdlog::error("Metal backend is not implemented yet.");
            return false;
        default:
            spdlog::error("Unsupported backend.");
            return false;
        }
    }

    void RenderContext::cleanup()
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            impl->vulkanContext.waitIdle(); // Ensure the device is idle before cleanup

            for (const auto &[handle, descriptorSetLayout] : impl->descriptorSetLayoutCache)
            {
                impl->vulkanContext.removeDescriptorSetLayout(descriptorSetLayout);
            }

            for (const auto &[handle, descriptorPool] : impl->descriptorPoolCache)
            {
                impl->vulkanContext.removeDescriptorPool(descriptorPool);
            }

            for (const auto &[handle, commandPool] : impl->commandPoolCache)
            {
                impl->vulkanContext.removeCommandPool(commandPool);
            }

            for (const auto &[handle, fence] : impl->fenceCache)
            {
                impl->vulkanContext.removeFence(fence);
            }

            for (const auto &[handle, semaphore] : impl->semaphoreCache)
            {
                impl->vulkanContext.removeSemaphore(semaphore);
            }

            for (const auto &[handle, buffer] : impl->bufferCache)
            {
                impl->vulkanContext.removeBuffer(buffer);
            }

            for (const auto &[handle, texture] : impl->textureCache)
            {
                if (impl->swapchainImageCache.find(handle) == impl->swapchainImageCache.end()) // Only remove non-swapchain textures
                {
                    impl->vulkanContext.removeTexture(texture);
                }
            }

            for (const auto &[handle, shader] : impl->shaderCache)
            {
                impl->vulkanContext.removeShader(shader);
            }

            for (const auto &[handle, pipeline] : impl->pipelineCache)
            {
                impl->vulkanContext.removePipeline(pipeline);
            }
            impl->vulkanContext.cleanup();
            impl->bufferCache.clear();
            impl->textureCache.clear();
            impl->shaderCache.clear();
            impl->pipelineCache.clear();
            impl->commandPoolCache.clear();
            impl->commandBufferCache.clear();
            impl->fenceCache.clear();
            impl->semaphoreCache.clear();
            impl->descriptorSetLayoutCache.clear();
            impl->descriptorPoolCache.clear();
            impl->descriptorSetCache.clear();
            impl->swapchainImageCache.clear();
            impl->renderTargetCache.clear();
            impl->shaderDescCache.clear();
            impl->shaderSources.clear();
            impl->pipelineDescCache.clear();
            impl->descriptorSetLayoutDescCache.clear();
            impl->nextBindlessTextureIndex = 0;
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during cleanup.");
            break;
        }
    }

    BufferHandle RenderContext::createBuffer(ResourceDesc desc)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto buffer = impl->vulkanContext.createBuffer(desc);
            if (!buffer)
            {
                spdlog::error("Failed to create Vulkan buffer.");
                return {};
            }
            BufferHandle handle = engine->handleManager.getNextBufferHandle();
            buffer->handle = handle;

            impl->bufferCache[handle] = buffer.value();
            return handle;
        }
        case Backend::DX12:
            return {}; // Placeholder
        case Backend::METAL:
            return {}; // Placeholder
        default:
            spdlog::error("Unsupported backend during buffer creation.");
            return {}; // Placeholder
        }
    }

    TextureHandle RenderContext::createTexture(ResourceDesc desc)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto texture = impl->vulkanContext.createTexture(desc);
            if (!texture)
            {
                spdlog::error("Failed to create Vulkan texture.");
                return {};
            }
            TextureHandle handle = engine->handleManager.getNextTextureHandle();
            texture->handle = handle;
            impl->textureCache[handle] = texture.value();
            return handle;
        }
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during texture creation.");
            return {};
        }
    }

    ShaderHandle RenderContext::createShader(ResourceDesc desc)
    {
        auto cachedIt = impl->shaderDescCache.find(desc);
        if (cachedIt != impl->shaderDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto shader = impl->vulkanContext.createShader(desc);
            if (!shader)
            {
                spdlog::error("Failed to create Vulkan shader.");
                return {};
            }
            ShaderHandle handle = engine->handleManager.getNextShaderHandle();
            shader->handle = handle;

            // Callers pass temporary SPIR-V, but the cache key is compared by content on every later lookup.
            impl->shaderSources.emplace_back(desc.shader.source, desc.shader.sourceSize);
            desc.shader.source = impl->shaderSources.back().data();
            shader->desc.shader.source = desc.shader.source;

            impl->shaderCache[handle] = shader.value();
            impl->shaderDescCache[desc] = handle;

            return handle;
        }
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during shader creation.");
            return {};
        }
    }

    PipelineHandle RenderContext::createPipeline(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::GRAPHICS_PIPELINE || desc.type == ResourceType::COMPUTE_PIPELINE);

        auto cachedIt = impl->pipelineDescCache.find(desc);
        if (cachedIt != impl->pipelineDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            // TODO: only graphics pipelines are supported in this implementation, need to add compute pipeline support later.
            if (desc.type != ResourceType::GRAPHICS_PIPELINE)
            {
                spdlog::error("Only graphics pipelines are supported in this implementation.");
                return {};
            }

            auto vertexIt = impl->shaderCache.find(desc.pipeline.vertexShader);
            auto fragmentIt = impl->shaderCache.find(desc.pipeline.fragmentShader);
            auto bindlessLayoutIt = impl->descriptorSetLayoutCache.find(desc.pipeline.descriptorSetLayout);
            if (vertexIt == impl->shaderCache.end() || fragmentIt == impl->shaderCache.end() || bindlessLayoutIt == impl->descriptorSetLayoutCache.end())
            {
                spdlog::error("Shader or descriptor set layout handle not found in cache during pipeline creation.");
                return {};
            }

            auto pipeline = impl->vulkanContext.createGraphicsPipeline(desc, vertexIt->second, fragmentIt->second, bindlessLayoutIt->second);
            if (!pipeline)
            {
                spdlog::error("Failed to create Vulkan pipeline.");
                return {};
            }
            PipelineHandle handle = engine->handleManager.getNextPipelineHandle();
            pipeline->handle = handle;

            impl->pipelineCache[handle] = pipeline.value();
            impl->pipelineDescCache[desc] = handle;
            return handle;
        }
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during pipeline creation.");
            return {};
        }
    }

    DescriptorSetLayoutHandle RenderContext::createBindlessDescriptorSetLayout(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT);

        auto cachedIt = impl->descriptorSetLayoutDescCache.find(desc);
        if (cachedIt != impl->descriptorSetLayoutDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto layout = impl->vulkanContext.createBindlessDescriptorSetLayout(desc);
            if (!layout)
            {
                spdlog::error("Failed to create Vulkan bindless descriptor set layout.");
                return {};
            }
            DescriptorSetLayoutHandle handle = engine->handleManager.getNextDescriptorSetLayoutHandle();
            layout->handle = handle;

            impl->descriptorSetLayoutCache[handle] = layout.value();
            impl->descriptorSetLayoutDescCache[desc] = handle;

            return handle;
        }
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during bindless descriptor set layout creation.");
            return {};
        }
    }

    DescriptorPoolHandle RenderContext::createDescriptorPool(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::DESCRIPTOR_POOL);

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto pool = impl->vulkanContext.createDescriptorPool(desc);
            if (!pool)
            {
                spdlog::error("Failed to create Vulkan descriptor pool.");
                return {};
            }
            DescriptorPoolHandle handle = engine->handleManager.getNextDescriptorPoolHandle();
            pool->handle = handle;

            impl->descriptorPoolCache[handle] = pool.value();

            return handle;
        }
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during descriptor pool creation.");
            return {};
        }
    }

    DescriptorSetHandle RenderContext::allocateDescriptorSet(ResourceDesc desc, const DescriptorPoolHandle &pool, const DescriptorSetLayoutHandle &layout)
    {
        assert(desc.type == ResourceType::DESCRIPTOR_SET);

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto poolIt = impl->descriptorPoolCache.find(pool);
            auto layoutIt = impl->descriptorSetLayoutCache.find(layout);
            if (poolIt == impl->descriptorPoolCache.end() || layoutIt == impl->descriptorSetLayoutCache.end())
            {
                spdlog::error("Descriptor pool or layout handle not found in cache during descriptor set allocation.");
                return {};
            }

            auto descriptorSet = impl->vulkanContext.allocateDescriptorSet(desc, poolIt->second, layoutIt->second);
            if (!descriptorSet)
            {
                spdlog::error("Failed to allocate Vulkan descriptor set.");
                return {};
            }
            DescriptorSetHandle handle = engine->handleManager.getNextDescriptorSetHandle();
            descriptorSet->handle = handle;

            impl->descriptorSetCache[handle] = descriptorSet.value();

            return handle;
        }
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during descriptor set allocation.");
            return {};
        }
    }

    CommandPoolHandle RenderContext::createCommandPool()
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandPool = impl->vulkanContext.createCommandPool();
            if (!commandPool)
            {
                spdlog::error("Failed to create Vulkan command pool.");
                return {};
            }

            CommandPoolHandle handle = engine->handleManager.getNextCommandPoolHandle();
            commandPool->handle = handle;

            impl->commandPoolCache[handle] = commandPool.value();

            return handle;
        }
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during command pool creation.");
            return {};
        }
    }

    CommandBufferHandle RenderContext::createCommandBuffer(const CommandPoolHandle &commandPool)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->commandPoolCache.find(commandPool);
            if (it == impl->commandPoolCache.end())
            {
                spdlog::error("Failed to create Vulkan command buffer: command pool handle not found.");
                return {};
            }

            auto commandBuffer = impl->vulkanContext.createCommandBuffer(it->second);
            if (!commandBuffer)
            {
                spdlog::error("Failed to create Vulkan command buffer.");
                return {};
            }

            CommandBufferHandle handle = engine->handleManager.getNextCommandBufferHandle();
            commandBuffer->handle = handle;

            impl->commandBufferCache[handle] = commandBuffer.value();

            return handle;
        }
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during command buffer creation.");
            return {};
        }
    }

    SemaphoreHandle RenderContext::createSemaphore(bool timeline, uint64_t initialValue)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto semaphore = impl->vulkanContext.createSemaphore(timeline, initialValue);
            if (!semaphore)
            {
                spdlog::error("Failed to create Vulkan semaphore.");
                return {};
            }

            SemaphoreHandle handle = engine->handleManager.getNextSemaphoreHandle();
            semaphore->handle = handle; // Semaphores might not need a handle in the same way as buffers/textures

            // Optionally store in a cache if you want to manage semaphores similarly
            impl->semaphoreCache[handle] = semaphore.value();

            return handle;
        }
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during semaphore creation.");
            return {};
        }
    }

    FenceHandle RenderContext::createFence(bool signaled)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto fence = impl->vulkanContext.createFence(signaled);
            if (!fence)
            {
                spdlog::error("Failed to create Vulkan fence.");
                return {};
            }

            FenceHandle handle = engine->handleManager.getNextFenceHandle();
            fence->handle = handle; // Fences might not need a handle in the same way as buffers/textures

            impl->fenceCache[handle] = fence.value();

            return handle;
        }
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during fence creation.");
            return {};
        }
    }

    RenderTargetHandle RenderContext::createRenderTarget(const ResourceDesc &desc)
    {
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
        impl->renderTargetCache[rtHandle] = rt;

        return rtHandle;
    }

    const RenderTarget &RenderContext::getRenderTarget(const RenderTargetHandle &handle)
    {
        auto it = impl->renderTargetCache.find(handle);
        if (it == impl->renderTargetCache.end())
        {
            spdlog::error("Render target handle not found in cache.");
            static RenderTarget emptyRenderTarget{};
            return emptyRenderTarget;
        }
        return it->second;
    }

    void RenderContext::destroyTexture(const TextureHandle &texture)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->textureCache.find(texture);
            if (it == impl->textureCache.end())
            {
                spdlog::error("Texture handle not found in cache during destroyTexture.");
                return;
            }
            impl->vulkanContext.removeTexture(it->second);
            impl->textureCache.erase(it);
            break;
        }
        case Backend::DX12:
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during texture destruction.");
            break;
        }
    }

    void RenderContext::destroyBuffer(const BufferHandle &buffer)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->bufferCache.find(buffer);
            if (it == impl->bufferCache.end())
            {
                spdlog::error("Buffer handle not found in cache during destroyBuffer.");
                return;
            }
            impl->vulkanContext.removeBuffer(it->second);
            impl->bufferCache.erase(it);
            break;
        }
        case Backend::DX12:
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during buffer destruction.");
            break;
        }
    }

    void RenderContext::destroySemaphore(const SemaphoreHandle &semaphore)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->semaphoreCache.find(semaphore);
            if (it == impl->semaphoreCache.end())
            {
                spdlog::error("Semaphore handle not found in cache during destroySemaphore.");
                return;
            }
            impl->vulkanContext.removeSemaphore(it->second);
            impl->semaphoreCache.erase(it);
            break;
        }
        case Backend::DX12:
            // Implement DX12 semaphore destruction if needed
            break;
        case Backend::METAL:
            // Implement Metal semaphore destruction if needed
            break;
        default:
            spdlog::error("Unsupported backend during semaphore destruction.");
            break;
        }
    }

    std::vector<TextureHandle> RenderContext::getSwapchainImages()
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto images = impl->vulkanContext.getSwapchainImages();
            std::vector<TextureHandle> handles;
            for (auto &image : images)
            {
                auto handle = engine->handleManager.getNextTextureHandle();
                image.handle = handle;
                impl->textureCache[handle] = image;
                impl->swapchainImageCache[handle] = image; // Store in swapchain-specific cache
                handles.push_back(handle);
            }
            return handles;
        }
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during swapchain image retrieval.");
            return {};
        }
    }

    bool RenderContext::fillBuffer(const BufferHandle &buffer, const void *data, size_t size, size_t offset)
    {
        assert(data != nullptr && "Data pointer cannot be null.");
        assert(size > 0 && "Size must be greater than zero.");
        assert(offset >= 0 && "Offset must be non-negative.");
        assert(buffer.isValid() && "Buffer handle must be valid.");

        auto it = impl->bufferCache.find(buffer);
        if (it == impl->bufferCache.end())
        {
            spdlog::error("Buffer handle not found in cache during fillBuffer.");
            return false;
        }

        auto gpuBuffer = it->second;

        switch (backend)
        {
        case Backend::VULKAN:
        {
            impl->vulkanContext.fillBuffer(gpuBuffer, data, size, offset);
            return true;
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during fillBuffer.");
            return false;
        }
    }

    bool RenderContext::fillTexture(const TextureHandle &textureHandle, const void *data)
    {
        ZoneScoped;

        assert(data != nullptr && "Data pointer cannot be null.");
        assert(textureHandle.isValid() && "Texture handle must be valid.");

        auto it = impl->textureCache.find(textureHandle);
        if (it == impl->textureCache.end())
        {
            spdlog::error("Texture handle not found in cache during fillTexture.");
            return false;
        }

        auto gpuTexture = it->second;

        switch (backend)
        {
        case Backend::VULKAN:
            impl->vulkanContext.fillTexture(gpuTexture, data);
            return true;
        case Backend::DX12:
            spdlog::error("DirectX 12 backend is not implemented yet for fillTexture.");
            return false;
        case Backend::METAL:
            spdlog::error("Metal backend is not implemented yet for fillTexture.");
            return false;
        default:
            spdlog::error("Unsupported backend during fillTexture.");
            return false;
        }
    }

    Format RenderContext::getSwapchainImageFormat()
    {
        switch (backend)
        {
        case Backend::VULKAN:
            return impl->vulkanContext.getSwapchainImageFormat();
        case Backend::DX12:
            return Format::UNKNOWN;
        case Backend::METAL:
            return Format::UNKNOWN;
        default:
            spdlog::error("Unsupported backend during swapchain image format retrieval.");
            return Format::UNKNOWN;
        }
    }

    glm::uvec2 RenderContext::getSwapchainExtent()
    {
        switch (backend)
        {
        case Backend::VULKAN:
            return impl->vulkanContext.getSwapchainExtent();
        case Backend::DX12:
            return {};
        case Backend::METAL:
            return {};
        default:
            spdlog::error("Unsupported backend during swapchain extent retrieval.");
            return {};
        }
    }

    uint64_t RenderContext::getBufferDeviceAddress(const BufferHandle &buffer)
    {
        auto it = impl->bufferCache.find(buffer);
        if (it == impl->bufferCache.end())
        {
            spdlog::error("Buffer handle not found in cache during getBufferDeviceAddress.");
            return 0;
        }

        auto gpuBuffer = it->second;

        if (gpuBuffer.desc.type != ResourceType::BUFFER)
        {
            spdlog::error("Handle is not a buffer during getBufferDeviceAddress.");
            return 0;
        }

        if (gpuBuffer.desc.buffer.deviceAddress == 0)
        {
            spdlog::error("Buffer '{}' has no device address.", gpuBuffer.desc.name);
            return 0;
        }

        return gpuBuffer.desc.buffer.deviceAddress;
    }

    ResourceDesc RenderContext::getResourceDesc(const BufferHandle &buffer)
    {
        auto it = impl->bufferCache.find(buffer);
        if (it == impl->bufferCache.end())
        {
            spdlog::error("Buffer handle not found in cache during getResourceDesc.");
            return {};
        }
        return it->second.desc;
    }

    ResourceDesc RenderContext::getResourceDesc(const TextureHandle &texture)
    {
        auto it = impl->textureCache.find(texture);
        if (it == impl->textureCache.end())
        {
            spdlog::error("Texture handle not found in cache during getResourceDesc.");
            return {};
        }
        return it->second.desc;
    }

    VRAMStats RenderContext::queryVRAMStats()
    {
        switch (backend)
        {
        case Backend::VULKAN:
            return impl->vulkanContext.queryVRAMStats();
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during VRAM stats query.");
            return {};
        }
    }

    uint32_t RenderContext::updateBindlessDescriptorSet(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, uint32_t slot)
    {
        if (slot > MAX_BINDLESS_TEXTURES || (slot == MAX_BINDLESS_TEXTURES && impl->nextBindlessTextureIndex >= MAX_BINDLESS_TEXTURES))
        {
            spdlog::error("Slot index exceeds maximum bindless textures.");
            return UINT32_MAX;
        }

        auto bindlessIt = impl->descriptorSetCache.find(bindlessSet);
        auto textureIt = impl->textureCache.find(texture);
        if (bindlessIt == impl->descriptorSetCache.end())
        {
            spdlog::error("Bindless descriptor set handle not found in cache during updateBindlessDescriptorSet.");
            return UINT32_MAX;
        }
        if (textureIt == impl->textureCache.end())
        {
            spdlog::error("Texture handle not found in cache during updateBindlessDescriptorSet.");
            return UINT32_MAX;
        }

        const uint32_t targetSlot = slot == MAX_BINDLESS_TEXTURES ? impl->nextBindlessTextureIndex : slot;

        switch (backend)
        {
        case Backend::VULKAN:
            impl->vulkanContext.updateBindlessDescriptorSet(bindlessIt->second, textureIt->second, targetSlot);
            break;
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during bindless descriptor set update.");
            return UINT32_MAX;
        }

        // Explicit slots count too: automatic allocation must never land on one of them.
        impl->nextBindlessTextureIndex = std::max(impl->nextBindlessTextureIndex, targetSlot + 1);
        return targetSlot;
    }

    bool RenderContext::waitForFence(FenceHandle fence, uint64_t timeout)
    {
        ZoneScoped;

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->fenceCache.find(fence);
            if (it == impl->fenceCache.end())
            {
                spdlog::error("Fence handle not found in cache during waitForFence.");
                return false;
            }
            auto fenceVKHandle = it->second;
            return impl->vulkanContext.waitForFence(fenceVKHandle, timeout);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during fence wait.");
            return false;
        }
    }

    bool RenderContext::resetFence(FenceHandle fence)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->fenceCache.find(fence);
            if (it == impl->fenceCache.end())
            {
                spdlog::error("Fence handle not found in cache during resetFence.");
                return false;
            }
            auto fenceVKHandle = it->second;
            return impl->vulkanContext.resetFence(fenceVKHandle);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during fence reset.");
            return false;
        }
    }

    SwapchainStatus RenderContext::acquireNextImage(SemaphoreHandle signalSemaphore, uint64_t timeout, uint32_t &imageIndex)
    {
        ZoneScoped;

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->semaphoreCache.find(signalSemaphore);
            if (it == impl->semaphoreCache.end())
            {
                spdlog::error("Semaphore handle not found in cache during acquireNextImage.");
                return SwapchainStatus::FAILURE;
            }
            auto semaphoreVKHandle = it->second;
            return impl->vulkanContext.acquireNextImage(semaphoreVKHandle, timeout, imageIndex);
        }
        case Backend::DX12:
            return SwapchainStatus::FAILURE;
        case Backend::METAL:
            return SwapchainStatus::FAILURE;
        default:
            spdlog::error("Unsupported backend during image acquisition.");
            return SwapchainStatus::FAILURE;
        }
    }

    bool RenderContext::beginCommandBuffer(const CommandBufferHandle &commandBuffer)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->commandBufferCache.find(commandBuffer);
            if (it == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during beginCommandBuffer.");
                return false;
            }
            return impl->vulkanContext.beginCommandBuffer(it->second);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during beginCommandBuffer.");
            return false;
        }
    }

    bool RenderContext::endCommandBuffer(const CommandBufferHandle &commandBuffer)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = impl->commandBufferCache.find(commandBuffer);
            if (it == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during endCommandBuffer.");
                return false;
            }
            return impl->vulkanContext.endCommandBuffer(it->second);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during endCommandBuffer.");
            return false;
        }
    }

    bool RenderContext::transitionImageLayout(const CommandBufferHandle &commandBuffer, TextureHandle texture, TextureUsage oldUsage, TextureUsage newUsage)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            auto textureIt = impl->textureCache.find(texture);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during transitionImageLayout.");
                return false;
            }
            if (textureIt == impl->textureCache.end())
            {
                spdlog::error("Texture handle not found in cache during transitionImageLayout.");
                return false;
            }

            return impl->vulkanContext.transitionImageLayout(commandBufferIt->second, textureIt->second, oldUsage, newUsage);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during image layout transition.");
            return false;
        }
    }

    bool RenderContext::beginRendering(const CommandBufferHandle &commandBuffer, TextureHandle colorAttachment, TextureHandle depthAttachment, const glm::vec4 &clearColor)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto colorIt = impl->textureCache.find(colorAttachment);
            auto depthIt = impl->textureCache.find(depthAttachment);
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during beginRendering.");
                return false;
            }
            if (colorIt == impl->textureCache.end())
            {
                spdlog::error("Color attachment texture handle not found in cache during beginRendering.");
                return false;
            }
            if (depthIt == impl->textureCache.end())
            {
                spdlog::error("Depth attachment texture handle not found in cache during beginRendering.");
                return false;
            }

            return impl->vulkanContext.beginRendering(commandBufferIt->second, colorIt->second, depthIt->second, clearColor);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during beginRendering.");
            return false;
        }
    }

    bool RenderContext::endRendering(const CommandBufferHandle &commandBuffer)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during endRendering.");
                return false;
            }
            return impl->vulkanContext.endRendering(commandBufferIt->second);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during endRendering.");
            return false;
        }
    }

    bool RenderContext::setViewport(const CommandBufferHandle &commandBuffer, float x, float y, float width, float height, float minDepth, float maxDepth)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during setViewport.");
                return false;
            }
            return impl->vulkanContext.setViewport(commandBufferIt->second, x, y, width, height, minDepth, maxDepth);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during setViewport.");
            return false;
        }
    }

    bool RenderContext::setScissor(const CommandBufferHandle &commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during setScissor.");
                return false;
            }
            return impl->vulkanContext.setScissor(commandBufferIt->second, x, y, width, height);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during setScissor.");
            return false;
        }
    }

    bool RenderContext::submit(const CommandBufferHandle &commandBuffer, const std::vector<SemaphoreHandle> &waitSemaphores, const std::vector<SemaphoreHandle> &signalSemaphores, FenceHandle fence)
    {
        ZoneScoped;

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during submit.");
                return false;
            }

            std::vector<gfx::SemaphoreVKHandle> waitSemaphoresVK;
            for (const auto &sem : waitSemaphores)
            {
                auto it = impl->semaphoreCache.find(sem);
                if (it == impl->semaphoreCache.end())
                {
                    spdlog::error("Wait semaphore handle not found in cache during submit.");
                    return false;
                }
                waitSemaphoresVK.push_back(it->second);
            }
            std::vector<gfx::SemaphoreVKHandle> signalSemaphoresVK;
            for (const auto &sem : signalSemaphores)
            {
                auto it = impl->semaphoreCache.find(sem);
                if (it == impl->semaphoreCache.end())
                {
                    spdlog::error("Signal semaphore handle not found in cache during submit.");
                    return false;
                }
                signalSemaphoresVK.push_back(it->second);
            }

            auto it = impl->fenceCache.find(fence);
            if (it == impl->fenceCache.end())
            {
                spdlog::error("Fence handle not found in cache during submit.");
                return false;
            }
            auto fenceVK = it->second;
            return impl->vulkanContext.submit(commandBufferIt->second, waitSemaphoresVK, signalSemaphoresVK, fenceVK);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during submit.");
            return false;
        }
    }

    SwapchainStatus RenderContext::present(uint32_t imageIndex, SemaphoreHandle waitSemaphore)
    {
        ZoneScoped;

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto semIt = impl->semaphoreCache.find(waitSemaphore);
            if (semIt == impl->semaphoreCache.end())
            {
                spdlog::error("Wait semaphore handle not found in cache during present.");
                return SwapchainStatus::FAILURE;
            }
            return impl->vulkanContext.present(imageIndex, semIt->second);
        }
        case Backend::DX12:
            return SwapchainStatus::FAILURE;
        case Backend::METAL:
            return SwapchainStatus::FAILURE;
        default:
            spdlog::error("Unsupported backend during present.");
            return SwapchainStatus::FAILURE;
        }
    }

    SwapchainStatus RenderContext::recreateSwapchain()
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto status = impl->vulkanContext.recreateSwapchain();
            if (status == SwapchainStatus::FAILURE)
                spdlog::error("Failed to recreate Vulkan swapchain.");

            // OUT_OF_DATE means the old swapchain was kept. Otherwise its images are gone, so drop
            // their handles (B5).
            if (status != SwapchainStatus::OUT_OF_DATE)
            {
                for (const auto &[handle, image] : impl->swapchainImageCache)
                    impl->textureCache.erase(handle);
                impl->swapchainImageCache.clear();
            }
            return status;
        }
        case Backend::DX12:
            return SwapchainStatus::FAILURE;
        case Backend::METAL:
            return SwapchainStatus::FAILURE;
        default:
            spdlog::error("Unsupported backend during swapchain recreation.");
            return SwapchainStatus::FAILURE;
        }
    }

    void RenderContext::bindPipeline(const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto pipelineIt = impl->pipelineCache.find(pipeline);
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (pipelineIt == impl->pipelineCache.end())
            {
                spdlog::error("Pipeline handle not found in cache during binding.");
                return;
            }
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during binding.");
                return;
            }

            impl->vulkanContext.bindPipeline(commandBufferIt->second, pipelineIt->second);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during pipeline binding.");
            break;
        }
    }

    void RenderContext::bindIndexBuffer(const CommandBufferHandle &commandBuffer, const BufferHandle &buffer, uint64_t offset, Format indexType)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto bufferIt = impl->bufferCache.find(buffer);
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (bufferIt == impl->bufferCache.end())
            {
                spdlog::error("Buffer handle not found in cache during index buffer binding.");
                return;
            }
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during index buffer binding.");
                return;
            }

            impl->vulkanContext.bindIndexBuffer(commandBufferIt->second, bufferIt->second, offset, indexType);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during index buffer binding.");
            break;
        }
    }

    void RenderContext::bindDescriptorSet(const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline, const DescriptorSetHandle &descriptorSet, uint32_t setIndex)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            auto pipelineIt = impl->pipelineCache.find(pipeline);
            auto descriptorSetIt = impl->descriptorSetCache.find(descriptorSet);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during descriptor set binding.");
                return;
            }
            if (pipelineIt == impl->pipelineCache.end())
            {
                spdlog::error("Pipeline handle not found in cache during descriptor set binding.");
                return;
            }
            if (descriptorSetIt == impl->descriptorSetCache.end())
            {
                spdlog::error("Descriptor set handle not found in cache during descriptor set binding.");
                return;
            }

            impl->vulkanContext.bindDescriptorSet(commandBufferIt->second, pipelineIt->second, descriptorSetIt->second, setIndex);
            break;
        }
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during descriptor set binding.");
            break;
        }
    }

    void RenderContext::pushConstants(const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline, ShaderType stage, const void *data, uint32_t size, uint32_t offset)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            auto pipelineIt = impl->pipelineCache.find(pipeline);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during push constants.");
                return;
            }
            if (pipelineIt == impl->pipelineCache.end())
            {
                spdlog::error("Pipeline handle not found in cache during push constants.");
                return;
            }

            impl->vulkanContext.pushConstants(commandBufferIt->second, pipelineIt->second, stage, data, size, offset);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during push constants.");
            break;
        }
    }

    void RenderContext::setUniform(const CommandBufferHandle &commandBuffer, const std::string &name, const void *data, size_t size)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during uniform setting.");
                return;
            }
            impl->vulkanContext.setUniform(commandBufferIt->second, name, data, size);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during uniform setting.");
            break;
        }
    }

    void RenderContext::draw(const CommandBufferHandle &commandBuffer, uint32_t vertexCount, uint32_t instanceCount)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during draw call.");
                return;
            }
            impl->vulkanContext.draw(commandBufferIt->second, vertexCount, instanceCount);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during draw call.");
            break;
        }
    }

    void RenderContext::drawIndexed(const CommandBufferHandle &commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t vertexOffset, uint32_t firstIndex, uint32_t firstInstance)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during indexed draw call.");
                return;
            }
            impl->vulkanContext.drawIndexed(commandBufferIt->second, indexCount, instanceCount, vertexOffset, firstIndex, firstInstance);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during indexed draw call.");
            break;
        }
    }

    void RenderContext::drawIndexedIndirect(const CommandBufferHandle &commandBuffer, const BufferHandle &drawInfoBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = impl->commandBufferCache.find(commandBuffer);
            if (commandBufferIt == impl->commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during indexed indirect draw call.");
                return;
            }
            auto drawInfoBufferIt = impl->bufferCache.find(drawInfoBuffer);
            if (drawInfoBufferIt == impl->bufferCache.end())
            {
                spdlog::error("Draw info buffer handle not found in cache during indexed indirect draw call.");
                return;
            }
            impl->vulkanContext.drawIndexedIndirect(commandBufferIt->second, drawInfoBufferIt->second, offset, drawCount, stride);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;

        default:
            break;
        }
    }
}