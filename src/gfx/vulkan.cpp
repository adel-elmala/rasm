#include "rasm/core/engine.h"
#include "rasm/gfx/vulkan.h"

#include "VkBootstrap.h"
#include "spdlog/spdlog.h"

namespace rasm::gfx
{
    // Forward declarations of private helper functions for Vulkan setup and management.
    vkb::Result<vkb::Instance> _init_instance(const char *app_name, bool enable_validation_layers);
    vkb::Result<vkb::PhysicalDevice> _init_physical_device(const vkb::Instance &vkb_instance, VkSurfaceKHR surface);
    vkb::Result<vkb::Device> _init_logical_device(const vkb::PhysicalDevice &physical_device);
    vkb::Result<VkSurfaceKHR> _init_surface(const vkb::Instance &instance, const Window &window, WindowHandle handle);
    vkb::Result<vkb::Swapchain> _init_swapchain(const vkb::Device &device);
}

namespace rasm::gfx
{
    VulkanContext::VulkanContext(Engine *owner) : engine(owner) {}

    VulkanContext::~VulkanContext() { cleanup(); }

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

        return true;
    }

    void VulkanContext::cleanup()
    {
        vkb::destroy_swapchain(swapchain);
        vkb::destroy_device(device);
        vkb::destroy_surface(instance, surface);
        vkb::destroy_instance(instance);
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
                                .require_api_version(1, 3, 0)
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

}