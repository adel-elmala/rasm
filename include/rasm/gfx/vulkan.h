#pragma once

#include "VkBootstrap.h"

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
                 ~VulkanContext();
            bool initialize();
            void cleanup();

        private:
            Engine*             engine          = nullptr;
            vkb::Instance       instance        = {};
            vkb::PhysicalDevice physical_device = {};
            vkb::Device         device          = {};
            vkb::Swapchain      swapchain       = {};
            VkSurfaceKHR        surface         = VK_NULL_HANDLE;
        };
    }
}