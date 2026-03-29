// clang-format off
#pragma once

#include <unordered_map>

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
        bool                initialize(Backend backend);
        void                cleanup();
        BufferHandle        createBuffer(ResourceDesc desc);
        TextureHandle       createTexture(ResourceDesc desc);
        ShaderHandle        createShader(ResourceDesc desc);
        PipelineHandle      createPipeline(ResourceDesc desc, const ShaderHandle& vertexShader, const ShaderHandle& fragmentShader);
        CommandPoolHandle   createCommandPool(vkb::QueueType type);
        CommandBufferHandle createCommandBuffer(const CommandPoolHandle& commandPool);
        SemaphoreHandle     createSemaphore(bool timeline = false, uint64_t initialValue = 0);
        FenceHandle         createFence(bool signaled = false);
        bool                recreateSwapchain();
        void                bindPipeline(const CommandBufferHandle& commandBuffer, const PipelineHandle& pipeline);
        void                setUniform(const CommandBufferHandle& commandBuffer, const std::string& name, const void* data, size_t size);
        void                draw(const CommandBufferHandle& commandBuffer, const uint32_t vertexCount, uint32_t instanceCount = 1);

    protected:
        Engine*                                                                         engine        = nullptr;
        gfx::VulkanContext                                                              vulkanContext = {};
        Backend                                                                         backend       = Backend::VULKAN;
        std::unordered_map<BufferHandle, gfx::BufferVKHandle, HandleHash>               bufferCache;
        std::unordered_map<TextureHandle, gfx::TextureVKHandle, HandleHash>             textureCache;
        std::unordered_map<ShaderHandle, gfx::ShaderVKHandle, HandleHash>               shaderCache;
        std::unordered_map<PipelineHandle, gfx::PipelineVKHandle, HandleHash>           pipelineCache;
        std::unordered_map<CommandPoolHandle, gfx::CommandPoolVKHandle, HandleHash>     commandPoolCache;
        std::unordered_map<CommandBufferHandle, gfx::CommandBufferVKHandle, HandleHash> commandBufferCache;
        std::unordered_map<SemaphoreHandle, gfx::SemaphoreVKHandle, HandleHash>         semaphoreCache;
        std::unordered_map<FenceHandle, gfx::FenceVKHandle, HandleHash>                 fenceCache;

        // map resource handles to their desc.
        std::unordered_map<ResourceDesc, BufferHandle, ResourceDescHash> bufferDescCache;
        std::unordered_map<ResourceDesc, TextureHandle, ResourceDescHash> textureDescCache;
        std::unordered_map<ResourceDesc, ShaderHandle, ResourceDescHash> shaderDescCache;
        std::unordered_map<ResourceDesc, PipelineHandle, ResourceDescHash> pipelineDescCache;
    };
}
// clang-format on