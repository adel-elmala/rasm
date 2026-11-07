#include "rhi/vulkan/vulkanUtils.h"

namespace rasm::rhi
{
    VkFormat toVkFormat(Format format)
    {
        switch (format)
        {
        case Format::R32G32B32_SFLOAT:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::R32G32_SFLOAT:
            return VK_FORMAT_R32G32_SFLOAT;
        case Format::R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::R16G16B16A16_SFLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::D24_UNORM_S8_UINT:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_SFLOAT:
            return VK_FORMAT_D32_SFLOAT;
        case Format::D32_SFLOAT_S8_UINT:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case Format::R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R32G32B32A32_SFLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::R8G8B8_UNORM:
            return VK_FORMAT_R8G8B8_UNORM;
        case Format::UNKNOWN:
            return VK_FORMAT_UNDEFINED;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    Format toFormat(VkFormat vkFormat)
    {
        switch (vkFormat)
        {
        case VK_FORMAT_R32G32B32_SFLOAT:
            return Format::R32G32B32_SFLOAT;
        case VK_FORMAT_R32G32_SFLOAT:
            return Format::R32G32_SFLOAT;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return Format::R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return Format::R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return Format::B8G8R8A8_SRGB;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return Format::R16G16B16A16_SFLOAT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return Format::D24_UNORM_S8_UINT;
        case VK_FORMAT_R8G8B8_UNORM:
            return Format::R8G8B8_UNORM;
        case VK_FORMAT_D32_SFLOAT:
            return Format::D32_SFLOAT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return Format::D32_SFLOAT_S8_UINT;
        default:
            return Format::UNKNOWN;
        }
    }

    VkShaderStageFlags toVkShaderStageFlags(ShaderType stage)
    {
        switch (stage)
        {
        case ShaderType::VERTEX:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderType::FRAGMENT:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderType::COMPUTE:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        case ShaderType::VERTEX_FRAGMENT:
            return VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderType::ALL:
            return VK_SHADER_STAGE_ALL;
        default:
            return static_cast<VkShaderStageFlags>(0);
        }
    }

    VkImageAspectFlags toVkAspectMask(Format format)
    {
        switch (format)
        {
        case Format::D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case Format::D24_UNORM_S8_UINT:
        case Format::D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    const char *toVkResultString(VkResult result)
    {
        switch (result)
        {
        case VK_SUCCESS:
            return "Success";
        case VK_NOT_READY:
            return "Not Ready";
        case VK_TIMEOUT:
            return "Timeout";
        case VK_EVENT_SET:
            return "Event Set";
        case VK_EVENT_RESET:
            return "Event Reset";
        case VK_INCOMPLETE:
            return "Incomplete";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "Out of Host Memory";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "Out of Device Memory";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "Initialization Failed";
        case VK_ERROR_DEVICE_LOST:
            return "Device Lost";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "Memory Map Failed";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "Layer Not Present";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "Extension Not Present";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "Feature Not Present";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "Incompatible Driver";
        case VK_ERROR_TOO_MANY_OBJECTS:
            return "Too Many Objects";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return "Format Not Supported";
        case VK_ERROR_FRAGMENTED_POOL:
            return "Fragmented Pool";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "Out of Date KHR";
        case VK_SUBOPTIMAL_KHR:
            return "Suboptimal KHR";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "Surface Lost KHR";
        default:
            return "Unknown Error";
        }
    }

    bool hasStencil(Format format)
    {
        return format == Format::D24_UNORM_S8_UINT || format == Format::D32_SFLOAT_S8_UINT;
    }
}
