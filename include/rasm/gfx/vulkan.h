// clang-format off
#pragma once

#include "VkBootstrap.h"
#include "vk_mem_alloc.h"

#include "rasm/core/types.h"

#include <optional>

namespace rasm
{
    class RenderContext;
    class Engine;

    namespace gfx
    {
        class VulkanContext
        {
        public:
                                            VulkanContext(Engine *owner);
                                            VulkanContext() = default;
                                            ~VulkanContext();
            bool                            initialize();
            void                            cleanup();
            std::optional<BufferVKHandle>   createBuffer(ResourceDesc desc);
            std::optional<TextureVKHandle>  createTexture(ResourceDesc desc);
            std::optional<ShaderHandle>     createShader(ResourceDesc desc);
            std::optional<PipelineHandle>   createPipeline(const ShaderHandle& vertexShader, const ShaderHandle& fragmentShader);
            void                            bindPipeline(const PipelineHandle& pipeline);
            void                            bindTexture(const std::string& name, const TextureVKHandle& texture);
            void                            setUniform(const std::string& name, const void* data, size_t size);
            void                            draw(uint32_t vertexCount, uint32_t instanceCount = 1);

        private:
            Engine*             engine          = nullptr;
            VmaAllocator        allocator       = {};
            vkb::Instance       instance        = {};
            vkb::PhysicalDevice physical_device = {};
            vkb::Device         device          = {};
            vkb::Swapchain      swapchain       = {};
            VkSurfaceKHR        surface         = VK_NULL_HANDLE;

        };
    }
}
// clang-format on