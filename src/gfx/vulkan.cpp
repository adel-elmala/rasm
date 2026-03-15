#include "rasm/core/engine.h"
#include "rasm/gfx/vulkan.h"

#include "spdlog/spdlog.h"

#define VMA_VULKAN_VERSION 1004000 // Vulkan 1.4
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

namespace rasm::gfx
{
    // Forward declarations of private helper functions for Vulkan setup and management.
    vkb::Result<vkb::Instance>          _init_instance(const char *app_name, bool enable_validation_layers);
    vkb::Result<vkb::PhysicalDevice>    _init_physical_device(const vkb::Instance &vkb_instance, VkSurfaceKHR surface);
    vkb::Result<vkb::Device>            _init_logical_device(const vkb::PhysicalDevice &physical_device);
    vkb::Result<VkSurfaceKHR>           _init_surface(const vkb::Instance &instance, const Window &window, WindowHandle handle);
    vkb::Result<vkb::Swapchain>         _init_swapchain(const vkb::Device &device);
    VkBufferUsageFlagBits               _to_vk_buffer_usage_flags(BufferUsage usage);
    VkImageUsageFlags                   _to_vk_image_usage_flags(TextureUsage usage);
    VkFormat                            _to_vk_format(TextureFormat format);
    const char *                        _to_vk_result_string(VkResult result);
}

namespace rasm::gfx
{
    VulkanContext::VulkanContext(Engine *owner) : engine(owner) {}

    VulkanContext::~VulkanContext() {}

    bool VulkanContext::initialize()
    {
        auto config = engine->getConfig();

        // create Vulkan instance
        auto vkb_instance = _init_instance(config.appName.c_str(), config.enableValidation);

        if (!vkb_instance)
            return false;

        this->instance = vkb_instance.value();

        // create surface
        auto vkb_surface = _init_surface(vkb_instance.value(), engine->getWindow(), engine->getMainWindow());
        if (!vkb_surface)
            return false;

        this->surface = vkb_surface.value();

        // select physical device
        auto vkb_physical_device = _init_physical_device(vkb_instance.value(), this->surface);

        if (!vkb_physical_device)
            return false;

        this->physical_device = vkb_physical_device.value();

        // create logical device
        auto vkb_device = _init_logical_device(vkb_physical_device.value());

        if (!vkb_device)
            return false;

        this->device = vkb_device.value();

        // create swapchain
        auto vkb_swapchain = _init_swapchain(vkb_device.value());

        if (!vkb_swapchain)
            return false;

        this->swapchain = vkb_swapchain.value();

        // create VMA allocator
        VmaAllocatorCreateInfo allocatorCreateInfo = {};
        allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_4;
        allocatorCreateInfo.physicalDevice = physical_device.physical_device;
        allocatorCreateInfo.device = device.device;
        allocatorCreateInfo.instance = instance.instance;

        if (vmaCreateAllocator(&allocatorCreateInfo, &this->allocator) != VK_SUCCESS)
        {
            spdlog::error("Failed to create VMA allocator.");
            return false;
        }

        return true;
    }

    void VulkanContext::cleanup()
    {
        vmaDestroyAllocator(allocator);
        vkb::destroy_swapchain(swapchain);
        vkb::destroy_device(device);
        vkb::destroy_surface(instance, surface);
        vkb::destroy_instance(instance);
    }

    std::optional<BufferVKHandle> VulkanContext::createBuffer(ResourceDesc desc)
    {
        assert(desc.type == ResourceDesc::Type::BUFFER);

        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.buffer.size;
        bufferInfo.usage = _to_vk_buffer_usage_flags(desc.buffer.usage);

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        VkBuffer buffer;
        VmaAllocation allocation;
        auto result = vmaCreateBuffer(this->allocator, &bufferInfo, &allocInfo, &buffer, &allocation, nullptr);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create buffer. Error: {}", _to_vk_result_string(result));
            return {};
        }

        BufferVKHandle rawHandle = {desc, {}, buffer, allocation};
        return rawHandle;
    }

    std::optional<TextureVKHandle> VulkanContext::createTexture(ResourceDesc desc)
    {
        assert(desc.type == ResourceDesc::Type::TEXTURE);

        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = desc.texture.width;
        imageInfo.extent.height = desc.texture.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = _to_vk_format(desc.texture.format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = _to_vk_image_usage_flags(desc.texture.usage);
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        VkImage image;
        VmaAllocation allocation;
        auto result = vmaCreateImage(this->allocator, &imageInfo, &allocInfo, &image, &allocation, nullptr);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create image. Error: {}", _to_vk_result_string(result));
            return {};
        }

        TextureVKHandle rawHandle = {desc, {}, image, VK_NULL_HANDLE, allocation};
        return rawHandle;
    }

    std::optional<ShaderHandle> VulkanContext::createShader(ResourceDesc desc)
    {
        return {};
    }

    std::optional<PipelineHandle> VulkanContext::createPipeline(const ShaderHandle &vertexShader, const ShaderHandle &fragmentShader)
    {
        return {};
    }

    void VulkanContext::bindPipeline(const PipelineHandle &pipeline)
    {
    }

    void VulkanContext::bindTexture(const std::string &name, const TextureVKHandle &texture)
    {
    }

    void VulkanContext::setUniform(const std::string &name, const void *data, size_t size)
    {
    }

    void VulkanContext::draw(uint32_t vertexCount, uint32_t instanceCount)
    {
    }

    // ----------------------------------------------------------
    // private helper functions for Vulkan setup and management.
    // ----------------------------------------------------------

    vkb::Result<vkb::Instance> _init_instance(const char *app_name, bool enable_validation_layers)
    {
        vkb::InstanceBuilder instance_builder;

        auto system_info_ret = vkb::SystemInfo::get_system_info();
        if (!system_info_ret)
        {
            spdlog::error("Failed to get system info. Error: {}", system_info_ret.error().message());
            return vkb::Result<vkb::Instance>{system_info_ret.error()};
        }

        auto system_info = system_info_ret.value();

        // check for a layer
        // if (system_info.is_layer_available("VK_LAYER_LUNARG_api_dump"))
        // {
        //     instance_builder.enable_layer("VK_LAYER_LUNARG_api_dump");
        // }

        if (enable_validation_layers && system_info.validation_layers_available)
        {
            instance_builder.enable_validation_layers()
                // Validation needs to send errors via a callback, have vk-bootstrap do it
                .use_default_debug_messenger();
        }

        // instance level extension
        // if (system_info.is_extension_available("VK_KHR_get_physical_device_properties2"))
        // {
        //     instance_builder.enable_extension("VK_KHR_get_physical_device_properties2");
        // }

        auto instance_ret = instance_builder
                                .set_app_name(app_name ? app_name : "rasm_app")
                                .set_engine_name("rasm")
                                .require_api_version(1, 4, 0)
                                .set_minimum_instance_version(1, 3, 0)
                                .build();

        if (!instance_ret)
        {
            spdlog::error("Failed to create Vulkan instance. Error: {}", instance_ret.error().message());
        }

        return instance_ret;
    }

    vkb::Result<vkb::PhysicalDevice> _init_physical_device(const vkb::Instance &vkb_instance, VkSurfaceKHR surface)
    {
        vkb::PhysicalDeviceSelector phys_device_selector(vkb_instance);

        // select() grabs a PhysicalDevice, By default, this will prefer a discrete GPU.
        auto physical_device_selector_return = phys_device_selector.set_surface(surface).select();

        if (!physical_device_selector_return)
        {
            // If no suitable devices were found, detailed_failure_reasons() will contain a list of reasons why.
            if (physical_device_selector_return.error() == vkb::PhysicalDeviceError::no_suitable_device)
            {
                const auto &detailed_reasons = physical_device_selector_return.detailed_failure_reasons();
                if (!detailed_reasons.empty())
                {
                    spdlog::error("GPU Selection failure reasons:\n");
                    for (const std::string &reason : detailed_reasons)
                    {
                        spdlog::error("{}", reason);
                    }
                }
            }
        }

        auto physical_device = physical_device_selector_return.value();
        spdlog::info(
            "Selected GPU information:\n"
            "\t\t\t\t\tname: {} -- type: {} --  memory: {} MB\n"
            "\t\t\t\t\tdriver version: {}.{}.{} --  API version: {}.{}.{}",
            physical_device.properties.deviceName,
            physical_device.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "Discrete" : physical_device.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "Integrated"
                                                                                                     : physical_device.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU      ? "Virtual"
                                                                                                     : physical_device.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU              ? "CPU"
                                                                                                                                                                                         : "Other",
            physical_device.memory_properties.memoryHeaps[0].size / (1024 * 1024),
            VK_VERSION_MAJOR(physical_device.properties.driverVersion),
            VK_VERSION_MINOR(physical_device.properties.driverVersion),
            VK_VERSION_PATCH(physical_device.properties.driverVersion),
            VK_VERSION_MAJOR(physical_device.properties.apiVersion),
            VK_VERSION_MINOR(physical_device.properties.apiVersion),
            VK_VERSION_PATCH(physical_device.properties.apiVersion));

        return physical_device_selector_return;
    }

    vkb::Result<vkb::Device> _init_logical_device(const vkb::PhysicalDevice &physical_device)
    {
        vkb::DeviceBuilder device_builder{physical_device};

        auto dev_ret = device_builder.build();

        if (!dev_ret)
        {
            spdlog::error("Failed to create logical device. Error: {}", dev_ret.error().message());
        }

        return dev_ret;
    }

    vkb::Result<VkSurfaceKHR> _init_surface(const vkb::Instance &instance, const Window &window, WindowHandle handle)
    {
        auto surface_ret = window.createSurfaceVk(handle, instance);

        if (!surface_ret)
        {
            return vkb::Result<VkSurfaceKHR>{vkb::Error{}};
        }

        return vkb::Result<VkSurfaceKHR>{surface_ret};
    }

    vkb::Result<vkb::Swapchain> _init_swapchain(const vkb::Device &device)
    {
        vkb::SwapchainBuilder swapchain_builder{device};
        auto swap_ret = swapchain_builder.build();
        if (!swap_ret)
        {
            spdlog::error("Failed to create swapchain. Error: {}", swap_ret.error().message());
        }
        return swap_ret;
    }

    VkBufferUsageFlagBits _to_vk_buffer_usage_flags(BufferUsage usage)
    {
        switch (usage)
        {
        case BufferUsage::VERTEX:
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        case BufferUsage::INDEX:
            return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        case BufferUsage::UNIFORM:
            return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        case BufferUsage::STORAGE:
            return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        default:
            return static_cast<VkBufferUsageFlagBits>(0);
        }
    }

    VkFormat _to_vk_format(TextureFormat format)
    {
        switch (format)
        {
        case TextureFormat::RGBA8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RGBA16F:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureFormat::DEPTH24STENCIL8:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    VkImageUsageFlags _to_vk_image_usage_flags(TextureUsage usage)
    {
        switch (usage)
        {
        case TextureUsage::TRANSFER_SRC:
            return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        case TextureUsage::TRANSFER_DST:
            return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        case TextureUsage::SAMPLED:
            return VK_IMAGE_USAGE_SAMPLED_BIT;
        case TextureUsage::STORAGE:
            return VK_IMAGE_USAGE_STORAGE_BIT;
        case TextureUsage::COLOR_ATTACHMENT:
            return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
            return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        case TextureUsage::TRANSIENT_ATTACHMENT:
            return VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        case TextureUsage::INPUT_ATTACHMENT:
            return VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        default:
            return 0;
        }
    }

    const char* _to_vk_result_string(VkResult result)
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
        default:
            return "Unknown Error";
        }
    }
}
