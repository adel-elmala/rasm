#pragma once

#include "rasm/core/types.h"
#include "rasm/gfx/types.h"
#include "rasm/gfx/vulkan.h"

namespace rasm
{
    class Engine;

    class RenderContext
    {
    public:
                        RenderContext() = default;
                        RenderContext(Engine *owner);
                        ~RenderContext() = default;
        bool            initialize(Backend backend);
        void            cleanup();
        BufferHandle    createBuffer(ResourceDesc desc);
        TextureHandle   createTexture(ResourceDesc desc);
        ShaderHandle    createShader(ResourceDesc desc);
        PipelineHandle  createPipeline(const ShaderHandle& vertexShader, const ShaderHandle& fragmentShader);
        void            bindPipeline(const PipelineHandle& pipeline);
        void            bindTexture(const std::string& name, const TextureHandle& texture);
        void            setUniform(const std::string& name, const void* data, size_t size);
        void            draw(uint32_t vertexCount, uint32_t instanceCount = 1);

    protected:
        Engine*                                                              engine        = nullptr;
        gfx::VulkanContext                                                   vulkanContext = {};
        Backend                                                              backend       = Backend::VULKAN;
        std::unordered_map<BufferHandle, gfx::BufferVKHandle, HandleHash>    bufferCache;
        std::unordered_map<TextureHandle, gfx::TextureVKHandle, HandleHash>  textureCache;
        std::unordered_map<ShaderHandle, gfx::ShaderVKHandle, HandleHash>    shaderCache;

    };
}
