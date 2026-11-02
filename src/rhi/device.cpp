#include "rhi/device.h"
#include "rhi/vulkan/vulkanDevice.h"

#include "spdlog/spdlog.h"

namespace rasm::rhi
{
    std::unique_ptr<Device> createDevice(Backend backend, const DeviceDesc &desc)
    {
        switch (backend)
        {
        case Backend::VULKAN:
            return createVulkanDevice(desc);
        case Backend::DX12:
            spdlog::error("The Direct3D 12 backend is not implemented yet.");
            return nullptr;
        case Backend::METAL:
            spdlog::error("The Metal backend is not implemented yet.");
            return nullptr;
        case Backend::WEBGPU:
            spdlog::error("The WebGPU backend is not implemented yet.");
            return nullptr;
        default:
            spdlog::error("Unknown backend.");
            return nullptr;
        }
    }
}
