#include "rasm/gfx/context.h"
#include "rasm/gfx/vulkan.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    RenderContext::RenderContext(Engine *owner) : engine(owner) {}
   
    void RenderContext::initialize(Backend _backend)
    {
        this->backend = _backend;
        // In a real implementation, this is where we'd set up the graphics API context (e.g., create Vulkan instance, device, swapchain, etc.)

        switch (backend)
        {
        case Backend::Vulkan:
            // Initialize Vulkan context
            vulkanContext = new gfx::VulkanContext(engine);
            if (!vulkanContext->initialize())
            {
                spdlog::error("Failed to initialize Vulkan context.");
            }
            break;
        case Backend::DX12:
            // Initialize DirectX 12 context
            break;
        case Backend::Metal:
            // Initialize Metal context
            break;
        default:
            break;
        }
    }
}