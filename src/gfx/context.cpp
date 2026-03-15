#include "rasm/gfx/context.h"
#include "rasm/gfx/vulkan.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    RenderContext::RenderContext(Engine *owner) : engine(owner) {}

    bool RenderContext::initialize(Backend _backend)
    {
        this->backend = _backend;

        switch (backend)
        {
        case Backend::Vulkan:
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
        case Backend::Metal:
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
        case Backend::Vulkan:
            vulkanContext.cleanup();
            break;
        case Backend::DX12:
            break;
        case Backend::Metal:
            break;
        default:
            spdlog::error("Unsupported backend during cleanup.");
            break;
        }
    }
}