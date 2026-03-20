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
            auto texture = vulkanContext.createTexture(desc);
            if (!texture)
            {
                spdlog::error("Failed to create Vulkan texture.");
                return {};
            }
            TextureHandle handle = engine->getNextTextureHandle();
            texture->handle = handle;

            textureCache[handle] = texture.value();

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

    void RenderContext::bindPipeline(const PipelineHandle &pipeline)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = pipelineCache.find(pipeline);
            if (it == pipelineCache.end())
            {
                spdlog::error("Pipeline handle not found in cache during binding.");
                return;
            }
            vulkanContext.bindPipeline(it->second);
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

    void RenderContext::bindTexture(const std::string &name, const TextureHandle &texture)
    {
        switch (backend)
        {
        case Backend::VULKAN:
        {
            auto it = textureCache.find(texture);
            if (it == textureCache.end())
            {
                spdlog::error("Texture handle not found in cache during binding.");
                return;
            }
            vulkanContext.bindTexture(name, it->second);
            break;
        }
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during texture binding.");
            break;
        }
    }

    void RenderContext::setUniform(const std::string &name, const void *data, size_t size)
    {
        switch (backend)
        {
        case Backend::VULKAN:
            vulkanContext.setUniform(name, data, size);
            break;
        case Backend::DX12:
            break;
        case Backend::METAL:
            break;
        default:
            spdlog::error("Unsupported backend during uniform setting.");
            break;
        }
    }

    void RenderContext::draw(uint32_t vertexCount, uint32_t instanceCount)
    {
        switch (backend)
        {
        case Backend::VULKAN:
            vulkanContext.draw(vertexCount, instanceCount);
            break;
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