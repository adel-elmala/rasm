#include <cassert>

#include "rasm/core/engine.h"
#include "rasm/gfx/context.h"
#include "rasm/gfx/vulkan.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    RenderContext::RenderContext(Engine *owner) : engine(owner)
    {
        bufferCache.reserve(25);   // Pre-allocate for 25 buffers
        textureCache.reserve(25);  // Pre-allocate for 25 textures
        shaderCache.reserve(25);   // Pre-allocate for 25 shaders
        pipelineCache.reserve(25); // Pre-allocate for 25 pipelines
    }

    bool RenderContext::initialize(Backend _backend)
    {
        this->backend = _backend;

        switch (backend)
        {
        case Backend::VULKAN:
            vulkanContext = gfx::VulkanContext(engine);
            if (!vulkanContext.initialize())
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
            vulkanContext.waitIdle(); // Ensure the device is idle before cleanup

            for (const auto &[handle, descriptorSetLayout] : descriptorSetLayoutCache)
            {
                vulkanContext.removeDescriptorSetLayout(descriptorSetLayout);
            }

            for (const auto &[handle, descriptorPool] : descriptorPoolCache)
            {
                vulkanContext.removeDescriptorPool(descriptorPool);
            }

            for (const auto &[handle, commandPool] : commandPoolCache)
            {
                vulkanContext.removeCommandPool(commandPool);
            }

            for (const auto &[handle, fence] : fenceCache)
            {
                vulkanContext.removeFence(fence);
            }

            for (const auto &[handle, semaphore] : semaphoreCache)
            {
                vulkanContext.removeSemaphore(semaphore);
            }

            for (const auto &[handle, buffer] : bufferCache)
            {
                vulkanContext.removeBuffer(buffer);
            }

            for (const auto &[handle, texture] : textureCache)
            {
                if (swapchainImageCache.find(handle) == swapchainImageCache.end()) // Only remove non-swapchain textures
                {
                    vulkanContext.removeTexture(texture);
                }
            }

            for (const auto &[handle, shader] : shaderCache)
            {
                vulkanContext.removeShader(shader);
            }

            for (const auto &[handle, pipeline] : pipelineCache)
            {
                vulkanContext.removePipeline(pipeline);
            }
            vulkanContext.cleanup();
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
        auto cachedIt = bufferDescCache.find(desc);
        if (cachedIt != bufferDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto buffer = vulkanContext.createBuffer(desc);
            if (!buffer)
            {
                spdlog::error("Failed to create Vulkan buffer.");
                return {};
            }
            BufferHandle handle = engine->handleManager.getNextBufferHandle();
            buffer->handle = handle;

            bufferCache[handle] = buffer.value();
            bufferDescCache[desc] = handle;
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
        auto cachedIt = textureDescCache.find(desc);
        if (cachedIt != textureDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto texture = vulkanContext.createTexture(desc);
            if (!texture)
            {
                spdlog::error("Failed to create Vulkan texture.");
                return {};
            }
            TextureHandle handle = engine->handleManager.getNextTextureHandle();
            texture->handle = handle;
            textureCache[handle] = texture.value();
            textureDescCache[desc] = handle;
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
        auto cachedIt = shaderDescCache.find(desc);
        if (cachedIt != shaderDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto shader = vulkanContext.createShader(desc);
            if (!shader)
            {
                spdlog::error("Failed to create Vulkan shader.");
                return {};
            }
            ShaderHandle handle = engine->handleManager.getNextShaderHandle();
            shader->handle = handle;

            shaderCache[handle] = shader.value();
            shaderDescCache[desc] = handle;

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

        auto cachedIt = pipelineDescCache.find(desc);
        if (cachedIt != pipelineDescCache.end())
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

            auto vertexIt = shaderCache.find(desc.pipeline.vertexShader);
            auto fragmentIt = shaderCache.find(desc.pipeline.fragmentShader);
            auto bindlessLayoutIt = descriptorSetLayoutCache.find(desc.pipeline.descriptorSetLayout);
            if (vertexIt == shaderCache.end() || fragmentIt == shaderCache.end() || bindlessLayoutIt == descriptorSetLayoutCache.end())
            {
                spdlog::error("Shader or descriptor set layout handle not found in cache during pipeline creation.");
                return {};
            }

            auto pipeline = vulkanContext.createGraphicsPipeline(desc, vertexIt->second, fragmentIt->second, bindlessLayoutIt->second);
            if (!pipeline)
            {
                spdlog::error("Failed to create Vulkan pipeline.");
                return {};
            }
            PipelineHandle handle = engine->handleManager.getNextPipelineHandle();
            pipeline->handle = handle;

            pipelineCache[handle] = pipeline.value();
            pipelineDescCache[desc] = handle;
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

        auto cachedIt = descriptorSetLayoutDescCache.find(desc);
        if (cachedIt != descriptorSetLayoutDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto layout = vulkanContext.createBindlessDescriptorSetLayout(desc);
            if (!layout)
            {
                spdlog::error("Failed to create Vulkan bindless descriptor set layout.");
                return {};
            }
            DescriptorSetLayoutHandle handle = engine->handleManager.getNextDescriptorSetLayoutHandle();
            layout->handle = handle;

            descriptorSetLayoutCache[handle] = layout.value();
            descriptorSetLayoutDescCache[desc] = handle;

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

        auto cachedIt = descriptorPoolDescCache.find(desc);
        if (cachedIt != descriptorPoolDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto pool = vulkanContext.createDescriptorPool(desc);
            if (!pool)
            {
                spdlog::error("Failed to create Vulkan descriptor pool.");
                return {};
            }
            DescriptorPoolHandle handle = engine->handleManager.getNextDescriptorPoolHandle();
            pool->handle = handle;

            descriptorPoolCache[handle] = pool.value();
            descriptorPoolDescCache[desc] = handle;

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

        auto cachedIt = descriptorSetDescCache.find(desc);
        if (cachedIt != descriptorSetDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto poolIt = descriptorPoolCache.find(pool);
            auto layoutIt = descriptorSetLayoutCache.find(layout);
            if (poolIt == descriptorPoolCache.end() || layoutIt == descriptorSetLayoutCache.end())
            {
                spdlog::error("Descriptor pool or layout handle not found in cache during descriptor set allocation.");
                return {};
            }

            auto descriptorSet = vulkanContext.allocateDescriptorSet(desc, poolIt->second, layoutIt->second);
            if (!descriptorSet)
            {
                spdlog::error("Failed to allocate Vulkan descriptor set.");
                return {};
            }
            DescriptorSetHandle handle = engine->handleManager.getNextDescriptorSetHandle();
            descriptorSet->handle = handle;

            descriptorSetCache[handle] = descriptorSet.value();
            descriptorSetDescCache[desc] = handle;

            return handle;
        }
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during descriptor set allocation.");
            return {};
        }
    }

    CommandPoolHandle RenderContext::createCommandPool(vkb::QueueType type)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandPool = vulkanContext.createCommandPool(type);
            if (!commandPool)
            {
                spdlog::error("Failed to create Vulkan command pool.");
                return {};
            }

            CommandPoolHandle handle = engine->handleManager.getNextCommandPoolHandle();
            commandPool->handle = handle;

            commandPoolCache[handle] = commandPool.value();

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
            auto it = commandPoolCache.find(commandPool);
            if (it == commandPoolCache.end())
            {
                spdlog::error("Failed to create Vulkan command buffer: command pool handle not found.");
                return {};
            }

            auto commandBuffer = vulkanContext.createCommandBuffer(it->second);
            if (!commandBuffer)
            {
                spdlog::error("Failed to create Vulkan command buffer.");
                return {};
            }

            CommandBufferHandle handle = engine->handleManager.getNextCommandBufferHandle();
            commandBuffer->handle = handle;

            commandBufferCache[handle] = commandBuffer.value();

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
            auto semaphore = vulkanContext.createSemaphore(timeline, initialValue);
            if (!semaphore)
            {
                spdlog::error("Failed to create Vulkan semaphore.");
                return {};
            }

            SemaphoreHandle handle = engine->handleManager.getNextSemaphoreHandle();
            semaphore->handle = handle; // Semaphores might not need a handle in the same way as buffers/textures

            // Optionally store in a cache if you want to manage semaphores similarly
            semaphoreCache[handle] = semaphore.value();

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
            auto fence = vulkanContext.createFence(signaled);
            if (!fence)
            {
                spdlog::error("Failed to create Vulkan fence.");
                return {};
            }

            FenceHandle handle = engine->handleManager.getNextFenceHandle();
            fence->handle = handle; // Fences might not need a handle in the same way as buffers/textures

            fenceCache[handle] = fence.value();

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
        depthDesc.texture.usage = TextureUsage::SAMPLED_DEPTH_STENCIL_ATTACHMENT;

        RenderTargetHandle rtHandle = engine->handleManager.getNextRenderTargetHandle();

        RenderTarget rt = {
            .attachments = {},
            .width = desc.renderTarget.width,
            .height = desc.renderTarget.height,
            .colorFormat = desc.renderTarget.colorFormat,
            .depthFormat = desc.renderTarget.depthFormat};

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            auto colorHandle = createTexture(colorDesc);
            auto depthHandle = createTexture(depthDesc);

            rt.attachments[i].color = colorHandle;
            rt.attachments[i].depth = depthHandle;

            auto colorAttachmentIndex = updateBindlessDescriptorSet(engine->getBindlessDescriptorSet(), colorHandle);
            auto depthAttachmentIndex = updateBindlessDescriptorSet(engine->getBindlessDescriptorSet(), depthHandle);

            engine->registery.bindlessTextureIndexMap[colorHandle] = colorAttachmentIndex;
            engine->registery.bindlessTextureIndexMap[depthHandle] = depthAttachmentIndex;
        }

        renderTargetCache[rtHandle] = rt;

        return rtHandle;
    }

    const RenderTarget &RenderContext::getRenderTarget(const RenderTargetHandle &handle)
    {
        auto it = renderTargetCache.find(handle);
        if (it == renderTargetCache.end())
        {
            spdlog::error("Render target handle not found in cache.");
            static RenderTarget emptyRenderTarget{};
            return emptyRenderTarget;
        }
        return it->second;
    }

    void RenderContext::destroySemaphore(const SemaphoreHandle &semaphore)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = semaphoreCache.find(semaphore);
            if (it == semaphoreCache.end())
            {
                spdlog::error("Semaphore handle not found in cache during destroySemaphore.");
                return;
            }
            vulkanContext.removeSemaphore(it->second);
            semaphoreCache.erase(it);
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
            auto images = vulkanContext.getSwapchainImages();
            std::vector<TextureHandle> handles;
            for (auto &image : images)
            {
                auto handle = engine->handleManager.getNextTextureHandle();
                image.handle = handle;
                textureCache[handle] = image;
                swapchainImageCache[handle] = image; // Store in swapchain-specific cache
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

        auto it = bufferCache.find(buffer);
        if (it == bufferCache.end())
        {
            spdlog::error("Buffer handle not found in cache during fillBuffer.");
            return false;
        }

        auto gpuBuffer = it->second;

        switch (backend)
        {
        case Backend::VULKAN:
        {
            vulkanContext.fillBuffer(gpuBuffer, data, size, offset);
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
        assert(data != nullptr && "Data pointer cannot be null.");
        assert(textureHandle.isValid() && "Texture handle must be valid.");

        auto it = textureCache.find(textureHandle);
        if (it == textureCache.end())
        {
            spdlog::error("Texture handle not found in cache during fillTexture.");
            return false;
        }

        auto gpuTexture = it->second;

        switch (backend)
        {
        case Backend::VULKAN:
            vulkanContext.fillTexture(gpuTexture, data);
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
            return vulkanContext.getSwapchainImageFormat();
        case Backend::DX12:
            return Format::UNKNOWN;
        case Backend::METAL:
            return Format::UNKNOWN;
        default:
            spdlog::error("Unsupported backend during swapchain image format retrieval.");
            return Format::UNKNOWN;
        }
    }

    uint64_t RenderContext::getBufferDeviceAddress(const BufferHandle &buffer)
    {
        auto it = bufferCache.find(buffer);
        if (it == bufferCache.end())
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
        auto it = bufferCache.find(buffer);
        if (it == bufferCache.end())
        {
            spdlog::error("Buffer handle not found in cache during getResourceDesc.");
            return {};
        }
        return it->second.desc;
    }

    ResourceDesc RenderContext::getResourceDesc(const TextureHandle &texture)
    {
        auto it = textureCache.find(texture);
        if (it == textureCache.end())
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
            return vulkanContext.queryVRAMStats();
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during VRAM stats query.");
            return {};
        }
    }

    uint32_t RenderContext::updateBindlessDescriptorSet(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, uint32_t slot)
    {
        assert(slot <= MAX_BINDLESS_TEXTURES && "Slot index exceeds maximum bindless textures.");

        auto bindlessIt = descriptorSetCache.find(bindlessSet);
        auto textureIt = textureCache.find(texture);
        if (bindlessIt == descriptorSetCache.end())
        {
            spdlog::error("Bindless descriptor set handle not found in cache during updateBindlessDescriptorSet.");
            return false;
        }
        if (textureIt == textureCache.end())
        {
            spdlog::error("Texture handle not found in cache during updateBindlessDescriptorSet.");
            return false;
        }

        switch (backend)
        {
        case Backend::VULKAN:
            vulkanContext.updateBindlessDescriptorSet(bindlessIt->second, textureIt->second, slot == MAX_BINDLESS_TEXTURES ? nextBindlessTextureIndex++ : slot);
            break;
        case Backend::DX12:
        case Backend::METAL:
        default:
            spdlog::error("Unsupported backend during bindless descriptor set update.");
            return MAX_BINDLESS_TEXTURES;
        }
        return slot == MAX_BINDLESS_TEXTURES ? nextBindlessTextureIndex - 1 : slot;
    }

    // bool RenderContext::updateBindlessDescriptorSet(const DescriptorSetHandle &bindlessSet, const RenderTargetHandle &renderTarget, uint32_t slot)
    // {
    //     assert(slot < MAX_BINDLESS_TEXTURES && "Slot index exceeds maximum bindless textures.");

    //     auto renderTargetIt = renderTargetCache.find(renderTarget);
    //     if (renderTargetIt == renderTargetCache.end())
    //     {
    //         spdlog::error("Render target handle not found in cache during updateBindlessDescriptorSet.");
    //         return false;
    //     }

    //     return updateBindlessDescriptorSet(bindlessSet, renderTargetIt->second, slot);
    // }

    bool RenderContext::waitForFence(FenceHandle fence, uint64_t timeout)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = fenceCache.find(fence);
            if (it == fenceCache.end())
            {
                spdlog::error("Fence handle not found in cache during waitForFence.");
                return false;
            }
            auto fenceVKHandle = it->second;
            return vulkanContext.waitForFence(fenceVKHandle, timeout);
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
            auto it = fenceCache.find(fence);
            if (it == fenceCache.end())
            {
                spdlog::error("Fence handle not found in cache during resetFence.");
                return false;
            }
            auto fenceVKHandle = it->second;
            return vulkanContext.resetFence(fenceVKHandle);
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

    bool RenderContext::acquireNextImage(SemaphoreHandle signalSemaphore, uint64_t timeout, uint32_t &imageIndex)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = semaphoreCache.find(signalSemaphore);
            if (it == semaphoreCache.end())
            {
                spdlog::error("Semaphore handle not found in cache during acquireNextImage.");
                return false;
            }
            auto semaphoreVKHandle = it->second;
            return vulkanContext.acquireNextImage(semaphoreVKHandle, timeout, imageIndex);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during image acquisition.");
            return false;
        }
    }

    bool RenderContext::beginCommandBuffer(const CommandBufferHandle &commandBuffer)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = commandBufferCache.find(commandBuffer);
            if (it == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during beginCommandBuffer.");
                return false;
            }
            return vulkanContext.beginCommandBuffer(it->second);
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
            auto it = commandBufferCache.find(commandBuffer);
            if (it == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during endCommandBuffer.");
                return false;
            }
            return vulkanContext.endCommandBuffer(it->second);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            auto textureIt = textureCache.find(texture);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during transitionImageLayout.");
                return false;
            }
            if (textureIt == textureCache.end())
            {
                spdlog::error("Texture handle not found in cache during transitionImageLayout.");
                return false;
            }

            return vulkanContext.transitionImageLayout(commandBufferIt->second, textureIt->second, oldUsage, newUsage);
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

    bool RenderContext::beginRendering(const CommandBufferHandle &commandBuffer, TextureHandle colorAttachment, TextureHandle depthAttachment)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto colorIt = textureCache.find(colorAttachment);
            auto depthIt = textureCache.find(depthAttachment);
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during beginRendering.");
                return false;
            }
            if (colorIt == textureCache.end())
            {
                spdlog::error("Color attachment texture handle not found in cache during beginRendering.");
                return false;
            }
            if (depthIt == textureCache.end())
            {
                spdlog::error("Depth attachment texture handle not found in cache during beginRendering.");
                return false;
            }

            return vulkanContext.beginRendering(commandBufferIt->second, colorIt->second, depthIt->second);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during endRendering.");
                return false;
            }
            return vulkanContext.endRendering(commandBufferIt->second);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during setViewport.");
                return false;
            }
            return vulkanContext.setViewport(commandBufferIt->second, x, y, width, height, minDepth, maxDepth);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during setScissor.");
                return false;
            }
            return vulkanContext.setScissor(commandBufferIt->second, x, y, width, height);
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
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during submit.");
                return false;
            }

            std::vector<gfx::SemaphoreVKHandle> waitSemaphoresVK;
            for (const auto &sem : waitSemaphores)
            {
                auto it = semaphoreCache.find(sem);
                if (it == semaphoreCache.end())
                {
                    spdlog::error("Wait semaphore handle not found in cache during submit.");
                    return false;
                }
                waitSemaphoresVK.push_back(it->second);
            }
            std::vector<gfx::SemaphoreVKHandle> signalSemaphoresVK;
            for (const auto &sem : signalSemaphores)
            {
                auto it = semaphoreCache.find(sem);
                if (it == semaphoreCache.end())
                {
                    spdlog::error("Signal semaphore handle not found in cache during submit.");
                    return false;
                }
                signalSemaphoresVK.push_back(it->second);
            }

            auto it = fenceCache.find(fence);
            if (it == fenceCache.end())
            {
                spdlog::error("Fence handle not found in cache during submit.");
                return false;
            }
            auto fenceVK = it->second;
            return vulkanContext.submit(commandBufferIt->second, waitSemaphoresVK, signalSemaphoresVK, fenceVK);
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

    bool RenderContext::present(uint32_t imageIndex, SemaphoreHandle waitSemaphore)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto semIt = semaphoreCache.find(waitSemaphore);
            if (semIt == semaphoreCache.end())
            {
                spdlog::error("Wait semaphore handle not found in cache during present.");
                return false;
            }
            return vulkanContext.present(imageIndex, semIt->second);
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during present.");
            return false;
        }
    }

    bool RenderContext::recreateSwapchain()
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto swapchain = vulkanContext.recreateSwapchain();
            if (!swapchain)
            {
                spdlog::error("Failed to recreate Vulkan swapchain.");
                return false;
            }
            return true;
        }
        case Backend::DX12:
            return false;
        case Backend::METAL:
            return false;
        default:
            spdlog::error("Unsupported backend during swapchain recreation.");
            return false;
        }
    }

    void RenderContext::bindPipeline(const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto pipelineIt = pipelineCache.find(pipeline);
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (pipelineIt == pipelineCache.end())
            {
                spdlog::error("Pipeline handle not found in cache during binding.");
                return;
            }
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during binding.");
                return;
            }

            vulkanContext.bindPipeline(commandBufferIt->second, pipelineIt->second);
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

    void RenderContext::bindVertexBuffer(const CommandBufferHandle &commandBuffer, const BufferHandle &buffer, uint64_t offset, uint32_t binding)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto bufferIt = bufferCache.find(buffer);
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (bufferIt == bufferCache.end())
            {
                spdlog::error("Buffer handle not found in cache during vertex buffer binding.");
                return;
            }
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during vertex buffer binding.");
                return;
            }

            vulkanContext.bindVertexBuffer(commandBufferIt->second, bufferIt->second, offset, binding);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during vertex buffer binding.");
            break;
        }
    }

    void RenderContext::bindIndexBuffer(const CommandBufferHandle &commandBuffer, const BufferHandle &buffer, uint64_t offset, Format indexType)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto bufferIt = bufferCache.find(buffer);
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (bufferIt == bufferCache.end())
            {
                spdlog::error("Buffer handle not found in cache during index buffer binding.");
                return;
            }
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during index buffer binding.");
                return;
            }

            vulkanContext.bindIndexBuffer(commandBufferIt->second, bufferIt->second, offset, indexType);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            auto pipelineIt = pipelineCache.find(pipeline);
            auto descriptorSetIt = descriptorSetCache.find(descriptorSet);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during descriptor set binding.");
                return;
            }
            if (pipelineIt == pipelineCache.end())
            {
                spdlog::error("Pipeline handle not found in cache during descriptor set binding.");
                return;
            }
            if (descriptorSetIt == descriptorSetCache.end())
            {
                spdlog::error("Descriptor set handle not found in cache during descriptor set binding.");
                return;
            }

            vulkanContext.bindDescriptorSet(commandBufferIt->second, pipelineIt->second, descriptorSetIt->second, setIndex);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            auto pipelineIt = pipelineCache.find(pipeline);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during push constants.");
                return;
            }
            if (pipelineIt == pipelineCache.end())
            {
                spdlog::error("Pipeline handle not found in cache during push constants.");
                return;
            }

            vulkanContext.pushConstants(commandBufferIt->second, pipelineIt->second, stage, data, size, offset);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during uniform setting.");
                return;
            }
            vulkanContext.setUniform(commandBufferIt->second, name, data, size);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during draw call.");
                return;
            }
            vulkanContext.draw(commandBufferIt->second, vertexCount, instanceCount);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during indexed draw call.");
                return;
            }
            vulkanContext.drawIndexed(commandBufferIt->second, indexCount, instanceCount, vertexOffset, firstIndex, firstInstance);
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
            auto commandBufferIt = commandBufferCache.find(commandBuffer);
            if (commandBufferIt == commandBufferCache.end())
            {
                spdlog::error("Command buffer handle not found in cache during indexed indirect draw call.");
                return;
            }
            auto drawInfoBufferIt = bufferCache.find(drawInfoBuffer);
            if (drawInfoBufferIt == bufferCache.end())
            {
                spdlog::error("Draw info buffer handle not found in cache during indexed indirect draw call.");
                return;
            }
            vulkanContext.drawIndexedIndirect(commandBufferIt->second, drawInfoBufferIt->second, offset, drawCount, stride);
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