#include "rhi/device.h"
#include "rhi/vulkan/vulkanDevice.h"

#include "spdlog/spdlog.h"

namespace rasm::rhi
{
    FormatBlock getFormatBlock(Format format)
    {
        switch (format)
        {
        case Format::BC4_UNORM:
        case Format::EAC_R11_UNORM:
            return {.width = 4, .height = 4, .bytes = 8};
        case Format::BC5_UNORM:
        case Format::BC7_UNORM:
        case Format::BC7_SRGB:
        case Format::ASTC_4x4_UNORM:
        case Format::ASTC_4x4_SRGB:
        case Format::ETC2_R8G8B8A8_UNORM:
        case Format::ETC2_R8G8B8A8_SRGB:
        case Format::EAC_R11G11_UNORM:
            return {.width = 4, .height = 4, .bytes = 16};
        case Format::R8G8B8A8_UNORM:
        case Format::R8G8B8A8_SRGB:
        case Format::B8G8R8A8_SRGB:
        case Format::D24_UNORM_S8_UINT:
        case Format::D32_SFLOAT:
        case Format::U32_UINT:
        case Format::R16G16_SFLOAT:
            return {.bytes = 4};
        case Format::R8G8B8_UNORM:
            return {.bytes = 3};
        case Format::U16_UINT:
            return {.bytes = 2};
        case Format::R16G16B16A16_SFLOAT:
        case Format::R32G32_SFLOAT:
        case Format::D32_SFLOAT_S8_UINT:
            return {.bytes = 8};
        case Format::R32G32B32_SFLOAT:
            return {.bytes = 12};
        case Format::R32G32B32A32_SFLOAT:
            return {.bytes = 16};
        case Format::UNKNOWN:
        default:
            return {.bytes = 0};
        }
    }

    // A partial block at the right or bottom edge is stored whole.
    uint64_t getTextureLevelSize(Format format, uint32_t width, uint32_t height)
    {
        const FormatBlock block = getFormatBlock(format);
        const uint64_t blocksX = (width + block.width - 1) / block.width;
        const uint64_t blocksY = (height + block.height - 1) / block.height;
        return blocksX * blocksY * block.bytes;
    }

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
