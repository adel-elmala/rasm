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
            for (const auto &[handle, buffer] : bufferCache)
            {
                vulkanContext.removeBuffer(buffer);
            }

            for (const auto &[handle, texture] : textureCache)
            {
                vulkanContext.removeTexture(texture);
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
            BufferHandle handle = engine->getNextBufferHandle();
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
            TextureHandle handle = engine->getNextTextureHandle();
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
            ShaderHandle handle = engine->getNextShaderHandle();
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

    PipelineHandle RenderContext::createPipeline(ResourceDesc desc, const ShaderHandle &vertexShader, const ShaderHandle &fragmentShader)
    {
        assert(desc.type == ResourceDesc::Type::GRAPHICS_PIPELINE || desc.type == ResourceDesc::Type::COMPUTE_PIPELINE);

        auto cachedIt = pipelineDescCache.find(desc);
        if (cachedIt != pipelineDescCache.end())
        {
            return cachedIt->second;
        }

        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto vertexIt = shaderCache.find(vertexShader);
            auto fragmentIt = shaderCache.find(fragmentShader);
            if (vertexIt == shaderCache.end() || fragmentIt == shaderCache.end())
            {
                spdlog::error("Shader handle not found in cache during pipeline creation.");
                return {};
            }

            auto pipeline = vulkanContext.createGraphicsPipeline(desc, vertexIt->second, fragmentIt->second);
            if (!pipeline)
            {
                spdlog::error("Failed to create Vulkan pipeline.");
                return {};
            }
            PipelineHandle handle = engine->getNextPipelineHandle();
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

            CommandPoolHandle handle = engine->getNextCommandPoolHandle();
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
            auto commandBuffer = vulkanContext.createCommandBuffer(commandPoolCache[commandPool]);
            if (!commandBuffer)
            {
                spdlog::error("Failed to create Vulkan command buffer.");
                return {};
            }

            CommandBufferHandle handle = engine->getNextCommandBufferHandle();
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

            SemaphoreHandle handle = engine->getNextSemaphoreHandle();
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

            FenceHandle handle = engine->getNextFenceHandle();
            fence->handle = handle; // Fences might not need a handle in the same way as buffers/textures

            // Optionally store in a cache if you want to manage fences similarly
            // fenceCache[handle] = fence.value();

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
}