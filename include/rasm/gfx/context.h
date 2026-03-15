#pragma once

#include "rasm/core/types.h"
#include "rasm/gfx/types.h"

namespace rasm
{
    class Engine;
    namespace gfx { class VulkanContext; }

    class RenderContext
    {
    public:
                        RenderContext() = default;
                        RenderContext(Engine *owner);
                        ~RenderContext() = default;
        void            initialize(Backend backend);
        BufferHandle    createBuffer(size_t size, BufferUsage usage);
        TextureHandle   createTexture(uint32_t width, uint32_t height, TextureFormat format);
        ShaderHandle    createShader(const std::string& source, ShaderType type);
        PipelineHandle  createPipeline(const ShaderHandle& vertexShader, const ShaderHandle& fragmentShader);
        void            bindPipeline(const PipelineHandle& pipeline);
        void            bindTexture(const std::string& name, const TextureHandle& texture);
        void            setUniform(const std::string& name, const void* data, size_t size);
        void            draw(uint32_t vertexCount, uint32_t instanceCount = 1);

    protected:
        Engine*                 engine        = nullptr;
        gfx::VulkanContext*     vulkanContext = nullptr;
        Backend                 backend       = Backend::Vulkan;

    };
}
