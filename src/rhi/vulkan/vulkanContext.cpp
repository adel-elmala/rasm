#include <cassert>
#include <algorithm>

#include "stb_image_resize2.h"

#include "rasm/engine.h"
#include "platform/windowSurface.h"
#include "rhi/vulkan/vulkanContext.h"

#include "spdlog/spdlog.h"

#include "vk_mem_alloc.h"

namespace rasm::gfx
{
    namespace
    {
        // clang-format off

        // Private helper functions for Vulkan setup and management.
        vkb::Result<vkb::Instance>          initInstance(const char* appName, bool enableValidationLayers, bool& debugUtilsEnabled);
        vkb::Result<vkb::PhysicalDevice>    initPhysicalDevice(const vkb::Instance& vkbInstance, VkSurfaceKHR surface);
        vkb::Result<vkb::Device>            initLogicalDevice(const vkb::PhysicalDevice& physicalDevice);
        vkb::Result<VkQueue>                initQueue(const vkb::Device& device, vkb::QueueType type);
        vkb::Result<uint32_t>               getQueueIndex(const vkb::Device& device, vkb::QueueType type);
        vkb::Result<VkSurfaceKHR>           initSurface(const vkb::Instance& instance, Engine* engine);
        vkb::Result<vkb::Swapchain>         initSwapchain(const vkb::Device& device, VkExtent2D desiredExtent);
        vkb::Result<vkb::Swapchain>         rebuildSwapchain(const vkb::Device& device, SwapchainVKHandle& oldSwapchain, VkExtent2D desiredExtent);
        SwapchainStatus                     toSwapchainStatus(VkResult result);
        VkBufferUsageFlags                  toVkBufferUsageFlags(BufferUsage usage);
        VkImageUsageFlags                   toVkImageUsageFlags(TextureUsage usage);
        VkFormat                            toVkFormat(Format format);
        Format                              toFormat(VkFormat format);
        VkIndexType                         toVkIndexType(Format format);
        VkPrimitiveTopology                 toVkTopology(PrimitiveTopology topology);
        VkShaderStageFlags                  toVkShaderStageFlags(ShaderType stage);
        VkImageAspectFlags                  toVkAspectMask(TextureUsage usage);
        VkDescriptorType                    toVkDescriptorType(ResourceType type);
        const char* toVkResultString(VkResult result);

        // clang-format on

        VkPipelineRasterizationStateCreateInfo createRasterizer(VkPolygonMode polygonMode, VkCullModeFlags cullMode, VkFrontFace frontFace)
        {
            VkPipelineRasterizationStateCreateInfo rasterizer{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .depthClampEnable = VK_FALSE,
                .rasterizerDiscardEnable = VK_FALSE,
                .polygonMode = polygonMode,
                .cullMode = cullMode,
                .frontFace = frontFace,
                .depthBiasEnable = VK_FALSE,
                .lineWidth = 1.0f};

            return rasterizer;
        }

        vkb::Result<vkb::Instance> initInstance(const char *appName, bool enableValidationLayers, bool &debugUtilsEnabled)
        {
            debugUtilsEnabled = false;

            vkb::InstanceBuilder instanceBuilder;

            auto systemInfoRet = vkb::SystemInfo::get_system_info();
            if (!systemInfoRet)
            {
                spdlog::error("Failed to get system info. Error: {}", systemInfoRet.error().message());
                return vkb::Result<vkb::Instance>{systemInfoRet.error()};
            }

            auto systemInfo = systemInfoRet.value();

            // check for a layer
            // if (systemInfo.is_layer_available("VK_LAYER_LUNARG_api_dump"))
            // {
            //     instanceBuilder.enable_layer("VK_LAYER_LUNARG_api_dump");
            // }

            // Debug utils is cheap, so enable it whenever it is available: object names show up in
            // RenderDoc / Nsight even without validation (B3).
            if (systemInfo.debug_utils_available)
            {
                instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                debugUtilsEnabled = true;
            }

            if (enableValidationLayers && systemInfo.validation_layers_available)
            {
                instanceBuilder.enable_validation_layers();

                // Validation needs to send errors via a callback, have vk-bootstrap do it
                if (debugUtilsEnabled)
                    instanceBuilder.use_default_debug_messenger();
            }

            // instance level extension
            // if (systemInfo.is_extension_available("VK_KHR_get_physical_device_properties2"))
            // {
            //     instanceBuilder.enable_extension("VK_KHR_get_physical_device_properties2");
            // }

            auto instanceRet = instanceBuilder
                                   .set_app_name(appName ? appName : "rasm_app")
                                   .set_engine_name("rasm")
                                   .require_api_version(1, 4, 0)
                                   .set_minimum_instance_version(1, 3, 0)
                                   .build();

            if (!instanceRet)
            {
                spdlog::error("Failed to create Vulkan instance. Error: {}", instanceRet.error().message());
            }

            return instanceRet;
        }

        vkb::Result<vkb::PhysicalDevice> initPhysicalDevice(const vkb::Instance &vkbInstance, VkSurfaceKHR surface)
        {
            vkb::PhysicalDeviceSelector physDeviceSelector(vkbInstance);

            VkPhysicalDeviceVulkan11Features enabledVk11Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                .pNext = nullptr,
                .shaderDrawParameters = VK_TRUE,
            };

            VkPhysicalDeviceVulkan12Features enabledVk12Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                .pNext = &enabledVk11Features,
                .storagePushConstant8 = VK_TRUE,
                .shaderInt8 = VK_TRUE,
                .descriptorIndexing = VK_TRUE,
                .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
                .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
                .descriptorBindingPartiallyBound = VK_TRUE,
                .descriptorBindingVariableDescriptorCount = VK_TRUE,
                .runtimeDescriptorArray = VK_TRUE,
                .scalarBlockLayout = VK_TRUE,
                .bufferDeviceAddress = VK_TRUE,
            };

            VkPhysicalDeviceVulkan13Features enabledVk13Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                .pNext = &enabledVk12Features,
                .synchronization2 = VK_TRUE,
                .dynamicRendering = VK_TRUE,
            };

            VkPhysicalDeviceVulkan14Features enabledVk14Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                .pNext = &enabledVk13Features,
                .hostImageCopy = VK_TRUE};

            VkPhysicalDeviceFeatures enabledVk10Features{
                .multiDrawIndirect = VK_TRUE,
                .samplerAnisotropy = VK_TRUE,
                .shaderInt64 = VK_TRUE,
            };

            // select() grabs a PhysicalDevice, By default, this will prefer a discrete GPU.
            auto physicalDeviceSelectorReturn = physDeviceSelector
                                                    .set_surface(surface)
                                                    .set_required_features_14(enabledVk14Features)               // Enable the 1.4 core feature
                                                    .set_required_features_13(enabledVk13Features)               // Enable the 1.3 core feature
                                                    .set_required_features_12(enabledVk12Features)               // Enable the 1.2 core feature
                                                    .set_required_features_11(enabledVk11Features)               // Enable the 1.1 core feature
                                                    .set_required_features(enabledVk10Features)                  // Enable the 1.0 core feature
                                                    .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)     // Enable swapchain extension
                                                    .add_required_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) // Enable memory budget extension
                                                    .select();

            if (!physicalDeviceSelectorReturn)
            {
                // If no suitable devices were found, detailed_failure_reasons() will contain a list of reasons why.
                if (physicalDeviceSelectorReturn.error() == vkb::PhysicalDeviceError::no_suitable_device)
                {
                    const auto &detailedReasons = physicalDeviceSelectorReturn.detailed_failure_reasons();
                    if (!detailedReasons.empty())
                    {
                        spdlog::error("GPU Selection failure reasons:\n");
                        for (const std::string &reason : detailedReasons)
                        {
                            spdlog::error("{}", reason);
                        }
                    }
                }
                return physicalDeviceSelectorReturn;
            }

            auto physicalDevice = physicalDeviceSelectorReturn.value();
            spdlog::info(
                "Selected GPU information:\n"
                "\t\t\t\t\tname: {} -- type: {} --  memory: {} MB\n"
                "\t\t\t\t\tdriver version: {}.{}.{} --  API version: {}.{}.{}",
                physicalDevice.properties.deviceName,
                physicalDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "Discrete" : physicalDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "Integrated"
                                                                                                        : physicalDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU      ? "Virtual"
                                                                                                        : physicalDevice.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU              ? "CPU"
                                                                                                                                                                                           : "Other",
                physicalDevice.memory_properties.memoryHeaps[0].size / (1024 * 1024),
                VK_VERSION_MAJOR(physicalDevice.properties.driverVersion),
                VK_VERSION_MINOR(physicalDevice.properties.driverVersion),
                VK_VERSION_PATCH(physicalDevice.properties.driverVersion),
                VK_VERSION_MAJOR(physicalDevice.properties.apiVersion),
                VK_VERSION_MINOR(physicalDevice.properties.apiVersion),
                VK_VERSION_PATCH(physicalDevice.properties.apiVersion));

            return physicalDeviceSelectorReturn;
        }

        vkb::Result<vkb::Device> initLogicalDevice(const vkb::PhysicalDevice &physicalDevice)
        {
            vkb::DeviceBuilder deviceBuilder{physicalDevice};

            auto devRet = deviceBuilder.build();

            if (!devRet)
            {
                spdlog::error("Failed to create logical device. Error: {}", devRet.error().message());
            }

            return devRet;
        }

        vkb::Result<VkQueue> initQueue(const vkb::Device &device, vkb::QueueType type)
        {
            auto queueRet = device.get_queue(type);
            if (!queueRet)
            {
                spdlog::error("Failed to get queue. Error: {}", queueRet.error().message());
            }
            return queueRet;
        }

        vkb::Result<uint32_t> getQueueIndex(const vkb::Device &device, vkb::QueueType type)
        {
            auto queueIndexRet = device.get_queue_index(type);
            if (!queueIndexRet)
            {
                spdlog::error("Failed to get queue index. Error: {}", queueIndexRet.error().message());
            }
            return queueIndexRet;
        }

        vkb::Result<VkSurfaceKHR> initSurface(const vkb::Instance &instance, Engine *engine)
        {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!createWindowSurface(engine->getMainWindow(), instance.instance, &surface))
                return vkb::Result<VkSurfaceKHR>{vkb::Error{}};

            return vkb::Result<VkSurfaceKHR>{surface};
        }

        // FIFO, because with NVIDIA's overlay on, the other present modes crash vkDestroyDevice when the window fills the screen (B21).
        vkb::Result<vkb::Swapchain> initSwapchain(const vkb::Device &device, VkExtent2D desiredExtent)
        {
            vkb::SwapchainBuilder swapchainBuilder{device};
            auto swapRet = swapchainBuilder.set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                               .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                               .set_desired_extent(desiredExtent.width, desiredExtent.height)
                               .build();
            if (!swapRet)
            {
                spdlog::error("Failed to create swapchain. Error: {}", swapRet.error().message());
            }
            return swapRet;
        }

        // The caller must make sure the device is idle.
        vkb::Result<vkb::Swapchain> rebuildSwapchain(const vkb::Device &device, SwapchainVKHandle &oldSwapchain, VkExtent2D desiredExtent)
        {
            vkb::SwapchainBuilder swapchainBuilder{device};
            auto swapRet = swapchainBuilder.set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                               .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                               .set_desired_extent(desiredExtent.width, desiredExtent.height)
                               .set_old_swapchain(oldSwapchain.swapchain)
                               .build();
            if (!swapRet)
            {
                spdlog::error("Failed to recreate swapchain. Error: {}", swapRet.error().message());
            }

            // Passing it as oldSwapchain retired the old swapchain, even if creation failed, so it is
            // destroyed either way.
            for (const auto &image : oldSwapchain.images)
            {
                vkDestroyImageView(device.device, image.view, nullptr);
            }
            if (oldSwapchain.swapchain.swapchain != VK_NULL_HANDLE)
                vkb::destroy_swapchain(oldSwapchain.swapchain);

            return swapRet;
        }

        SwapchainStatus toSwapchainStatus(VkResult result)
        {
            switch (result)
            {
            case VK_SUCCESS:
                return SwapchainStatus::OK;
            case VK_SUBOPTIMAL_KHR:
                return SwapchainStatus::SUBOPTIMAL;
            case VK_ERROR_OUT_OF_DATE_KHR:
                return SwapchainStatus::OUT_OF_DATE;
            default:
                return SwapchainStatus::FAILURE;
            }
        }

        VkBufferUsageFlags toVkBufferUsageFlags(BufferUsage usage)
        {
            // Every buffer is created with the device-address bit so that its GPU address can be
            // handed to shaders through push constants / other buffers regardless of its primary usage.
            VkBufferUsageFlags flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

            switch (usage)
            {
            case BufferUsage::INDEX:
                return flags | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            case BufferUsage::INDIRECT:
                return flags | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
            case BufferUsage::UNIFORM:
                return flags | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            case BufferUsage::STORAGE:
                return flags | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            case BufferUsage::DEVICE_ADDRESS:
                return flags;
            default:
                return flags;
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
            default:
                return static_cast<VkShaderStageFlags>(0);
            }
        }

        VkPrimitiveTopology toVkTopology(PrimitiveTopology topology)
        {
            switch (topology)
            {
            case PrimitiveTopology::POINT_LIST:
                return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case PrimitiveTopology::LINE_LIST:
                return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case PrimitiveTopology::LINE_STRIP:
                return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case PrimitiveTopology::TRIANGLE_LIST:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case PrimitiveTopology::TRIANGLE_STRIP:
                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            default:
                return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
            }
        }

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

        VkIndexType toVkIndexType(Format format)
        {
            switch (format)
            {
            case Format::U16_UINT:
                return VK_INDEX_TYPE_UINT16;
            case Format::U32_UINT:
                return VK_INDEX_TYPE_UINT32;
            default:
                return VK_INDEX_TYPE_MAX_ENUM;
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
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return Format::D32_SFLOAT_S8_UINT;
            default:
                return Format::UNKNOWN;
            }
        }

        VkImageUsageFlags toVkImageUsageFlags(TextureUsage usage)
        {
            switch (usage)
            {
            case TextureUsage::TRANSFER_SRC:
                return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            case TextureUsage::TRANSFER_DST:
                return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            case TextureUsage::SAMPLED:
                // VK_EXT_host_image_copy must be enabled for VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT to be valid.
                return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT;
            case TextureUsage::STORAGE:
                return VK_IMAGE_USAGE_STORAGE_BIT;
            case TextureUsage::COLOR_ATTACHMENT:
                return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
                return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            case TextureUsage::SAMPLED_COLOR_ATTACHMENT:
                return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            case TextureUsage::TRANSIENT_ATTACHMENT:
                return VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
            case TextureUsage::INPUT_ATTACHMENT:
                return VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            default:
                return 0;
            }
        }

        VkImageAspectFlags toVkAspectMask(TextureUsage usage)
        {
            switch (usage)
            {
            case TextureUsage::COLOR_ATTACHMENT:
            case TextureUsage::SAMPLED:
            case TextureUsage::SAMPLED_COLOR_ATTACHMENT:
                return VK_IMAGE_ASPECT_COLOR_BIT;
            case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return 0;
            }
        }

        VkDescriptorType toVkDescriptorType(ResourceType type)
        {
            switch (type)
            {
            case ResourceType::BUFFER:
                return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case ResourceType::SAMPLER:
                return VK_DESCRIPTOR_TYPE_SAMPLER;
            case ResourceType::TEXTURE:
                return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            default:
                return VK_DESCRIPTOR_TYPE_MAX_ENUM;
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
    }
}

namespace rasm::gfx
{
    VulkanContext::VulkanContext(Engine *owner) : engine(owner) {}

    VulkanContext::~VulkanContext() {}

    bool VulkanContext::initialize()
    {
        auto config = engine->getConfig();

        // create Vulkan instance
        auto vkbInstance = initInstance(config.appName.c_str(), config.enableValidation, this->debugUtilsEnabled);

        if (!vkbInstance)
            return false;

        this->instance = vkbInstance.value();

        // create surface
        auto vkbSurface = initSurface(vkbInstance.value(), engine);
        if (!vkbSurface)
            return false;

        this->surface = vkbSurface.value();

        // select physical device
        auto vkbPhysicalDevice = initPhysicalDevice(vkbInstance.value(), this->surface);

        if (!vkbPhysicalDevice)
            return false;

        this->physicalDevice = vkbPhysicalDevice.value();

        // create logical device
        auto vkbDevice = initLogicalDevice(vkbPhysicalDevice.value());

        if (!vkbDevice)
            return false;

        this->device = vkbDevice.value();

        // Create the dispatch table
        this->dispatchTable = this->device.make_table();

        // get graphics queue
        auto vkbQueue = initQueue(vkbDevice.value(), vkb::QueueType::graphics);

        if (!vkbQueue)
            return false;
        this->graphicsQueue = vkbQueue.value();

        // get graphics queue family index
        auto graphicsQueueIndexResult = getQueueIndex(vkbDevice.value(), vkb::QueueType::graphics);

        if (!graphicsQueueIndexResult)
            return false;

        this->graphicsQueueFamilyIndex = graphicsQueueIndexResult.value();

        // create swapchain
        auto vkbSwapchain = initSwapchain(vkbDevice.value(), getDesiredSwapchainExtent());

        if (!vkbSwapchain)
            return false;

        this->swapchain.swapchain = vkbSwapchain.value();
        if (!loadSwapchainImages())
            return false;

        // create VMA allocator
        VmaVulkanFunctions vkFunctions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
            .vkCreateImage = vkCreateImage,
        };

        VmaAllocatorCreateInfo allocatorCreateInfo = {};
        allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT | VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_4;
        allocatorCreateInfo.physicalDevice = physicalDevice.physical_device;
        allocatorCreateInfo.device = device.device;
        allocatorCreateInfo.instance = instance.instance;
        allocatorCreateInfo.pVulkanFunctions = &vkFunctions;

        if (vmaCreateAllocator(&allocatorCreateInfo, &this->allocator) != VK_SUCCESS)
        {
            spdlog::error("Failed to create VMA allocator.");
            return false;
        }

        return true;
    }

    void VulkanContext::cleanup()
    {
        waitIdle();

        for (auto &imageHandle : swapchain.images)
        {
            vkDestroyImageView(device.device, imageHandle.view, nullptr);
        }
        swapchain.images.clear();
        if (swapchain.swapchain.swapchain != VK_NULL_HANDLE)
            vkb::destroy_swapchain(swapchain.swapchain);
        swapchain = {};
        if (allocator != nullptr)
            vmaDestroyAllocator(allocator);
        allocator = nullptr;
        if (device.device != VK_NULL_HANDLE)
            vkb::destroy_device(device);
        device = {};
        if (surface != VK_NULL_HANDLE)
            vkb::destroy_surface(instance, surface);
        surface = VK_NULL_HANDLE;
        if (instance.instance != VK_NULL_HANDLE)
            vkb::destroy_instance(instance);
        instance = {};
        physicalDevice = {};
        dispatchTable = {};
        graphicsQueue = VK_NULL_HANDLE;
    }

    void VulkanContext::waitIdle()
    {
        if (device.device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(this->device.device);
    }

    std::optional<BufferVKHandle> VulkanContext::createBuffer(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::BUFFER);

        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.buffer.size;
        bufferInfo.usage = toVkBufferUsageFlags(desc.buffer.usage);

        VmaAllocationCreateInfo allocCI = {};
        allocCI.usage = VMA_MEMORY_USAGE_AUTO;
        allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer buffer;
        VmaAllocation allocation;
        VmaAllocationInfo allocInfo;
        auto result = vmaCreateBuffer(this->allocator, &bufferInfo, &allocCI, &buffer, &allocation, &allocInfo);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create buffer. Error: {}", toVkResultString(result));
            return {};
        }

        VkBufferDeviceAddressInfo bufferAddressInfo = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bufferAddressInfo.buffer = buffer;
        desc.buffer.deviceAddress = vkGetBufferDeviceAddress(this->device.device, &bufferAddressInfo);

        setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)buffer, desc.name);

        BufferVKHandle rawHandle = {desc, {}, buffer, allocation, allocInfo};
        return rawHandle;
    }

    void VulkanContext::fillTexture(const TextureVKHandle &texture, const void *data)
    {
        assert(texture.desc.type == ResourceType::TEXTURE && texture.desc.texture.usage == TextureUsage::SAMPLED);

        // 1. Transition layout from UNDEFINED to SHADER_READ directly on the CPU
        VkHostImageLayoutTransitionInfo transitionDstInfo = {};
        transitionDstInfo.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO;
        transitionDstInfo.image = texture.image;
        transitionDstInfo.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        transitionDstInfo.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transitionDstInfo.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                              .baseMipLevel = 0,
                                              .levelCount = texture.mipLevels,
                                              .baseArrayLayer = 0,
                                              .layerCount = 1};

        auto result = vkTransitionImageLayout(device, 1, &transitionDstInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to transition image layout. Error: {}", toVkResultString(result));
            return;
        }

        // Generate the complete RGBA8 mip chain on the host, matching this backend's
        // host-image-copy upload path. Color must be filtered in linear light; data maps
        // (normal/MR/AO) must retain their linear encoding. No GPU blit support is required.
        std::vector<std::vector<unsigned char>> mipData(texture.mipLevels - 1);
        std::vector<VkMemoryToImageCopy> regions(texture.mipLevels);
        const auto *pixels = static_cast<const unsigned char *>(data);
        uint32_t width = texture.desc.texture.width;
        uint32_t height = texture.desc.texture.height;
        for (uint32_t level = 0; level < texture.mipLevels; ++level)
        {
            auto &region = regions[level];
            region.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY;
            region.pHostPointer = pixels;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            region.imageExtent = {width, height, 1};
            if (level + 1 < texture.mipLevels)
            {
                auto nextWidth = std::max(1u, width / 2);
                auto nextHeight = std::max(1u, height / 2);
                auto &next = mipData[level];
                next.resize(static_cast<size_t>(nextWidth) * nextHeight * 4);
                unsigned char *resized = nullptr;
                if (texture.desc.texture.format == Format::R8G8B8A8_SRGB ||
                    texture.desc.texture.format == Format::B8G8R8A8_SRGB)
                    resized = stbir_resize_uint8_srgb(pixels, width, height, 0, next.data(), nextWidth, nextHeight, 0, STBIR_RGBA);
                else
                    resized = stbir_resize_uint8_linear(pixels, width, height, 0, next.data(), nextWidth, nextHeight, 0, STBIR_4CHANNEL);
                if (!resized)
                {
                    spdlog::error("Failed to generate mip level {} for '{}'", level + 1, texture.desc.name);
                    return;
                }
                pixels = next.data();
                width = nextWidth;
                height = nextHeight;
            }
        }

        VkCopyMemoryToImageInfo copyInfo = {};
        copyInfo.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO;
        copyInfo.dstImage = texture.image;
        copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        copyInfo.regionCount = static_cast<uint32_t>(regions.size());
        copyInfo.pRegions = regions.data();

        result = vkCopyMemoryToImage(device, &copyInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to copy memory to image. Error: {}", toVkResultString(result));
            return;
        }
    }

    std::optional<TextureVKHandle> VulkanContext::createTexture(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::TEXTURE);

        // Drivers may accept an oversized image instead of failing, so this can't be left to vmaCreateImage.
        const uint32_t maxExtent = physicalDevice.properties.limits.maxImageDimension2D;
        if (desc.texture.width > maxExtent || desc.texture.height > maxExtent)
        {
            spdlog::error("Texture '{}' is {}x{}, the device limit is {}.", desc.name, desc.texture.width, desc.texture.height, maxExtent);
            return {};
        }

        // TODO: check if the format is supported by the device, and if not, find a compatible one
        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = desc.texture.width;
        imageInfo.extent.height = desc.texture.height;
        imageInfo.extent.depth = 1;
        // Only sampled RGBA8 images use this upload/generation path. Render targets and
        // other formats retain a single level.
        uint32_t mipLevels = 1;
        if (desc.texture.usage == TextureUsage::SAMPLED &&
            (desc.texture.format == Format::R8G8B8A8_UNORM || desc.texture.format == Format::R8G8B8A8_SRGB ||
             desc.texture.format == Format::B8G8R8A8_SRGB))
        {
            for (uint32_t size = std::max(desc.texture.width, desc.texture.height); size > 1; size /= 2)
                ++mipLevels;
        }
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = toVkFormat(desc.texture.format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = toVkImageUsageFlags(desc.texture.usage);
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        if (desc.texture.usage == TextureUsage::SAMPLED)
        {
            allocInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
        }

        VkImage image;
        VmaAllocation allocation;
        auto result = vmaCreateImage(this->allocator, &imageInfo, &allocInfo, &image, &allocation, nullptr);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create image. Error: {}", toVkResultString(result));
            return {};
        }

        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = toVkFormat(desc.texture.format);
        viewInfo.subresourceRange.aspectMask = toVkAspectMask(desc.texture.usage);
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView imageView;
        result = vkCreateImageView(this->device.device, &viewInfo, nullptr, &imageView);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create image view. Error: {}", toVkResultString(result));
            vmaDestroyImage(this->allocator, image, allocation);
            return {};
        }

        // create a sampler for the texture
        VkSampler sampler = VK_NULL_HANDLE;
        if (desc.texture.usage == TextureUsage::SAMPLED ||
            desc.texture.usage == TextureUsage::SAMPLED_COLOR_ATTACHMENT)
        {

            VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.maxLod = static_cast<float>(mipLevels - 1);
            // Anisotropy is already required when selecting the physical device.
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physicalDevice, &properties);
            samplerInfo.anisotropyEnable = VK_TRUE;
            samplerInfo.maxAnisotropy = std::min(8.0f, properties.limits.maxSamplerAnisotropy);
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

            result = vkCreateSampler(this->device.device, &samplerInfo, nullptr, &sampler);
            if (result != VK_SUCCESS)
            {
                spdlog::error("Failed to create sampler. Error: {}", toVkResultString(result));
                vkDestroyImageView(this->device.device, imageView, nullptr);
                vmaDestroyImage(this->allocator, image, allocation);
                return {};
            }
        }

        setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)image, desc.name);
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)imageView, desc.name);

        TextureVKHandle rawHandle = {desc, {}, image, imageView, sampler, allocation, mipLevels};
        return rawHandle;
    }

    std::optional<ShaderVKHandle> VulkanContext::createShader(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::SHADER);

        VkShaderModuleCreateInfo shaderModuleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderModuleInfo.codeSize = desc.shader.sourceSize;
        shaderModuleInfo.pCode = reinterpret_cast<const uint32_t *>(desc.shader.source);

        VkShaderModule shaderModule;
        auto result = vkCreateShaderModule(this->device.device, &shaderModuleInfo, nullptr, &shaderModule);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create shader module. Error: {}", toVkResultString(result));
            return {};
        }

        setObjectName(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)shaderModule, desc.name);

        ShaderVKHandle rawHandle = {desc, {}, shaderModule};
        return rawHandle;
    }

    std::optional<PipelineVKHandle> VulkanContext::createGraphicsPipeline(ResourceDesc desc, const ShaderVKHandle &vertexShader, const ShaderVKHandle &fragmentShader, const DescriptorSetLayoutVKHandle &bindlessDescriptorSetLayout)
    {
        VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .size = sizeof(VkDeviceAddress)};

        VkDescriptorSetLayout setLayouts[] = {bindlessDescriptorSetLayout.layout};

        VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = setLayouts,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange};

        VkPipelineLayout pipelineLayout;
        auto result = vkCreatePipelineLayout(this->device.device, &pipelineLayoutInfo, nullptr, &pipelineLayout);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create pipeline layout. Error: {}", toVkResultString(result));
            return {};
        }

        // With vertex pulling the shader fetches vertices itself, so use an empty vertex input state.
        // This is core Vulkan and avoids VK_EXT_vertex_input_dynamic_state, which MoltenVK doesn't support.
        VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = toVkTopology(desc.pipeline.topology),
        };

        VkPipelineShaderStageCreateInfo shaderStages[] = {
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertexShader.module,
                .pName = "vertMain",
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragmentShader.module,
                .pName = "fragMain",
            }};

        VkPipelineViewportStateCreateInfo viewportStateInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };

        std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        VkPipelineDynamicStateCreateInfo dynamicStateInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
            .pDynamicStates = dynamicStates.data(),
        };

        VkPipelineDepthStencilStateCreateInfo depthStencilInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE};

        // Dynamic rendering: describe the formats used by this pipeline.
        // NOTE: These formats must match the actual color and depth attachments
        // used when beginning dynamic rendering.
        VkFormat colorAttachmentFormat = toVkFormat(desc.pipeline.colorAttachmentFormat);
        VkFormat depthAttachmentFormat = toVkFormat(desc.pipeline.depthStencilAttachmentFormat);
        // combined depth/stencil formats must declare the stencil aspect too (Metal validation asserts otherwise).
        bool hasStencil = depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
                          depthAttachmentFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;

        VkPipelineRenderingCreateInfo pipelineRenderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorAttachmentFormat,
            .depthAttachmentFormat = depthAttachmentFormat,
            .stencilAttachmentFormat = hasStencil ? depthAttachmentFormat : VK_FORMAT_UNDEFINED,
        };

        VkPipelineColorBlendAttachmentState colorBlendAttachment = {
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };

        VkPipelineColorBlendStateCreateInfo colorBlendInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &colorBlendAttachment};

        VkPipelineRasterizationStateCreateInfo rasterizerInfo = createRasterizer(VK_POLYGON_MODE_FILL,
                                                                                 VK_CULL_MODE_NONE,
                                                                                 VK_FRONT_FACE_CLOCKWISE);

        VkPipelineMultisampleStateCreateInfo multisampleInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE};

        VkGraphicsPipelineCreateInfo pipelineInfo = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &pipelineRenderingInfo,
            .stageCount = 2,
            .pStages = shaderStages,
            .pVertexInputState = &vertexInputInfo,
            .pInputAssemblyState = &inputAssemblyInfo,
            .pViewportState = &viewportStateInfo,
            .pRasterizationState = &rasterizerInfo,
            .pMultisampleState = &multisampleInfo,
            .pDepthStencilState = &depthStencilInfo,
            .pColorBlendState = &colorBlendInfo,
            .pDynamicState = &dynamicStateInfo,
            .layout = pipelineLayout};

        VkPipeline pipeline;
        result = vkCreateGraphicsPipelines(this->device.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create graphics pipeline. Error: {}", toVkResultString(result));
            vkDestroyPipelineLayout(this->device.device, pipelineLayout, nullptr);
            return {};
        }

        setObjectName(VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline, desc.name);

        PipelineVKHandle rawHandle = {desc, {}, pipeline, pipelineLayout};
        return rawHandle;
    }

    std::optional<CommandPoolVKHandle> VulkanContext::createCommandPool()
    {
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = this->graphicsQueueFamilyIndex;

        VkCommandPool commandPool;
        auto result = vkCreateCommandPool(this->device.device, &poolInfo, nullptr, &commandPool);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create command pool. Error: {}", toVkResultString(result));
            return {};
        }

        CommandPoolVKHandle rawHandle = {{}, commandPool};
        return rawHandle;
    }

    std::optional<CommandBufferVKHandle> VulkanContext::createCommandBuffer(const CommandPoolVKHandle &commandPool)
    {
        VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = commandPool.commandPool;
        allocInfo.commandBufferCount = 1;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

        VkCommandBuffer commandBuffer;
        auto result = vkAllocateCommandBuffers(this->device.device, &allocInfo, &commandBuffer);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to allocate command buffer. Error: {}", toVkResultString(result));
            return {};
        }

        CommandBufferVKHandle rawHandle = {{}, commandBuffer};
        return rawHandle;
    }

    std::optional<SemaphoreVKHandle> VulkanContext::createSemaphore(bool timeline, uint64_t initialValue)
    {
        VkSemaphoreTypeCreateInfo typeInfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY,
            .initialValue = initialValue, // Initial value is relevant for timeline semaphore type
        };

        VkSemaphoreCreateInfo semaphoreInfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &typeInfo};

        VkSemaphore semaphore;
        auto result = vkCreateSemaphore(this->device.device, &semaphoreInfo, nullptr, &semaphore);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create semaphore. Error: {}", toVkResultString(result));
            return {};
        }

        SemaphoreVKHandle rawHandle = {timeline ? SemaphoreVKHandle::Type::TIMELINE : SemaphoreVKHandle::Type::BINARY, {}, semaphore};
        return rawHandle;
    }

    std::optional<FenceVKHandle> VulkanContext::createFence(bool signaled)
    {
        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;

        VkFence fence;
        auto result = vkCreateFence(this->device.device, &fenceInfo, nullptr, &fence);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create fence. Error: {}", toVkResultString(result));
            return {};
        }

        FenceVKHandle rawHandle = {{}, fence};
        return rawHandle;
    }

    std::optional<DescriptorSetLayoutVKHandle> VulkanContext::createBindlessDescriptorSetLayout(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT);

        // Allow updating after binding, and allow empty slots in our giant array
        VkDescriptorBindingFlags bindlessFlags =
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

        VkDescriptorSetLayoutBindingFlagsCreateInfo extendedInfo = {};
        extendedInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        extendedInfo.bindingCount = 1;
        extendedInfo.pBindingFlags = &bindlessFlags;

        VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
        samplerLayoutBinding.binding = 0;
        samplerLayoutBinding.descriptorType = toVkDescriptorType(desc.bindlessDescriptorSetLayout.type);
        samplerLayoutBinding.descriptorCount = desc.bindlessDescriptorSetLayout.count;
        samplerLayoutBinding.stageFlags = toVkShaderStageFlags(desc.bindlessDescriptorSetLayout.stage);

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        // Crucial flag for the layout itself
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &samplerLayoutBinding;
        layoutInfo.pNext = &extendedInfo;

        VkDescriptorSetLayout bindlessLayout;
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &bindlessLayout) != VK_SUCCESS)
        {
            spdlog::error("Failed to create bindless textures descriptor set layout.");
            return {};
        }

        DescriptorSetLayoutVKHandle rawHandle = {desc, {}, bindlessLayout};
        return rawHandle;
    }

    std::optional<DescriptorPoolVKHandle> VulkanContext::createDescriptorPool(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::DESCRIPTOR_POOL);

        std::vector<VkDescriptorPoolSize> poolSizes;
        for (const auto &poolSize : desc.descriptorPool.descriptorType)
        {
            VkDescriptorPoolSize vkPoolSize = {};
            vkPoolSize.type = toVkDescriptorType(poolSize.descriptorType);
            vkPoolSize.descriptorCount = poolSize.descriptorCount;
            poolSizes.push_back(vkPoolSize);
        }

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = desc.descriptorPool.maxSets;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

        VkDescriptorPool descriptorPool;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
        {
            spdlog::error("Failed to create descriptor pool.");
            return {};
        }

        DescriptorPoolVKHandle rawHandle = {desc, {}, descriptorPool};
        return rawHandle;
    }

    std::optional<DescriptorSetVKHandle> VulkanContext::allocateDescriptorSet(ResourceDesc desc, const DescriptorPoolVKHandle &pool, const DescriptorSetLayoutVKHandle &layout)
    {
        assert(desc.type == ResourceType::DESCRIPTOR_SET);

        uint32_t variableDescCount{static_cast<uint32_t>(layout.desc.bindlessDescriptorSetLayout.count)};

        VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT,
            .descriptorSetCount = 1,
            .pDescriptorCounts = &variableDescCount};

        VkDescriptorSetAllocateInfo texDescSetAlloc{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = &variableDescCountAI,
            .descriptorPool = pool.pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout.layout};

        VkDescriptorSet descriptorSet;
        if (vkAllocateDescriptorSets(device, &texDescSetAlloc, &descriptorSet) != VK_SUCCESS)
        {
            spdlog::error("Failed to allocate descriptor set.");
            return {};
        }

        DescriptorSetVKHandle rawHandle = {desc, {}, pool, layout, descriptorSet};
        return rawHandle;
    }

    std::vector<TextureVKHandle> VulkanContext::getSwapchainImages()
    {
        return swapchain.images;
    }

    Format VulkanContext::getSwapchainImageFormat()
    {
        return toFormat(swapchain.swapchain.image_format);
    }

    glm::uvec2 VulkanContext::getSwapchainExtent()
    {
        return {swapchain.swapchain.extent.width, swapchain.swapchain.extent.height};
    }

    SwapchainStatus VulkanContext::recreateSwapchain()
    {
        waitIdle();

        // A minimized window has a 0x0 surface, and no swapchain can be created for it. Keep the
        // old swapchain until the surface has pixels again (B5).
        VkSurfaceCapabilitiesKHR capabilities{};
        auto capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice.physical_device, surface, &capabilities);
        if (capsResult != VK_SUCCESS)
        {
            spdlog::error("Failed to query surface capabilities. Error: {}", toVkResultString(capsResult));
            return SwapchainStatus::FAILURE;
        }
        if (capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0)
            return SwapchainStatus::OUT_OF_DATE;

        auto swapchainRet = rebuildSwapchain(this->device, this->swapchain, getDesiredSwapchainExtent());

        // The old swapchain is retired and destroyed even when creation fails. Without a swapchain,
        // acquire reports OUT_OF_DATE so the caller keeps trying to recreate it.
        this->swapchain = {};
        if (!swapchainRet)
            return SwapchainStatus::FAILURE;

        this->swapchain.swapchain = swapchainRet.value();
        if (!loadSwapchainImages())
        {
            for (const auto &image : swapchain.images)
                vkDestroyImageView(device.device, image.view, nullptr);
            vkb::destroy_swapchain(swapchain.swapchain);
            this->swapchain = {};
            return SwapchainStatus::FAILURE;
        }

        return SwapchainStatus::OK;
    }

    bool VulkanContext::loadSwapchainImages()
    {
        this->swapchain.imageFormat = toFormat(swapchain.swapchain.image_format);
        this->swapchain.imageCount = swapchain.swapchain.image_count;
        this->swapchain.images.clear();

        auto imagesResult = swapchain.swapchain.get_images();
        if (!imagesResult)
        {
            spdlog::error("Failed to get swapchain images: {}", imagesResult.error().message());
            return false;
        }
        auto viewsResult = swapchain.swapchain.get_image_views();
        if (!viewsResult)
        {
            spdlog::error("Failed to get swapchain image views: {}", viewsResult.error().message());
            return false;
        }
        const auto &images = imagesResult.value();
        const auto &views = viewsResult.value();

        for (uint32_t i = 0; i < swapchain.swapchain.image_count; ++i)
        {
            TextureVKHandle imageHandle;
            imageHandle.handle = {};
            imageHandle.image = images[i];
            imageHandle.view = views[i];
            imageHandle.desc.type = ResourceType::TEXTURE;
            imageHandle.desc.texture.width = swapchain.swapchain.extent.width;
            imageHandle.desc.texture.height = swapchain.swapchain.extent.height;
            imageHandle.desc.texture.format = swapchain.imageFormat;
            imageHandle.desc.texture.usage = TextureUsage::COLOR_ATTACHMENT;

            this->swapchain.images.push_back(imageHandle);
        }
        return true;
    }

    // Only used when the surface leaves the extent to the swapchain (currentExtent == 0xFFFFFFFF, e.g. Wayland).
    VkExtent2D VulkanContext::getDesiredSwapchainExtent() const
    {
        const auto &window = engine->getMainWindow();
        return {window.pixelWidth, window.pixelHeight};
    }

    void VulkanContext::fillBuffer(const BufferVKHandle &buffer, const void *data, size_t size, size_t offset)
    {
        assert(offset + size <= buffer.desc.buffer.size);
        assert(buffer.allocationInfo.pMappedData != nullptr);
        std::memcpy(static_cast<uint8_t *>(buffer.allocationInfo.pMappedData) + offset, data, size);
        // VMA may choose non-coherent host-visible memory; coherent allocations make this a no-op.
        auto result = vmaFlushAllocation(allocator, buffer.allocation, offset, size);
        if (result != VK_SUCCESS)
            spdlog::error("Failed to flush buffer memory: {}", toVkResultString(result));
    }

    void VulkanContext::updateBindlessDescriptorSet(const DescriptorSetVKHandle &bindlessSet, const TextureVKHandle &texture, uint32_t slot)
    {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texture.view;
        imageInfo.sampler = texture.sampler;

        VkWriteDescriptorSet descriptorWrite = {};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = bindlessSet.set;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = slot;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1; // We are only writing one texture
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    bool VulkanContext::transitionImageLayout(const CommandBufferVKHandle &commandBuffer, const TextureVKHandle &image, const TextureUsage &oldUsage, const TextureUsage &newUsage)
    {
        VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkAccessFlags2 srcAccessMask = 0;
        VkAccessFlags2 dstAccessMask = 0;
        VkPipelineStageFlags2 srcStageMask = 0;
        VkPipelineStageFlags2 dstStageMask = 0;

        // Determine the old and new layouts based on the usage flags
        auto usageToLayout = [](TextureUsage usage) -> VkImageLayout
        {
            switch (usage)
            {
            case TextureUsage::UNKNOWN:
                return VK_IMAGE_LAYOUT_UNDEFINED;
            case TextureUsage::COLOR_ATTACHMENT:
                return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case TextureUsage::PRESENT_SRC:
                return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            case TextureUsage::SAMPLED:
                return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            default:
                spdlog::error("Unsupported texture usage for layout transition.");
                return VK_IMAGE_LAYOUT_UNDEFINED;
            }
        };

        // What the barrier waits for depends only on how the image was used before.
        auto getSrcScope = [](TextureUsage oldUsage, VkAccessFlags2 &srcAccessMask, VkPipelineStageFlags2 &srcStageMask)
        {
            switch (oldUsage)
            {
            case TextureUsage::UNKNOWN:
                // The contents are discarded, but the last use still has to finish, and it could be any of these.
                // The color output stage also chains the barrier to the acquire semaphore of a swapchain image.
                srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT_KHR |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::COLOR_ATTACHMENT:
                srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
                srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT_KHR | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR;
                srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::SAMPLED:
                // Reads make nothing available, so only their stage is waited on.
                srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                srcAccessMask = VK_ACCESS_2_NONE;
                return true;
            default:
                spdlog::error("Unsupported old texture usage for layout transition.");
                return false;
            }
        };

        auto getDstScope = [](TextureUsage newUsage, VkAccessFlags2 &dstAccessMask, VkPipelineStageFlags2 &dstStageMask)
        {
            switch (newUsage)
            {
            case TextureUsage::COLOR_ATTACHMENT:
                dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::PRESENT_SRC:
                dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                dstAccessMask = VK_ACCESS_2_NONE;
                return true;
            case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
                dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT_KHR | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR;
                dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::SAMPLED:
                dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                return true;
            default:
                spdlog::error("Unsupported new texture usage for layout transition.");
                return false;
            }
        };

        if (!getSrcScope(oldUsage, srcAccessMask, srcStageMask) || !getDstScope(newUsage, dstAccessMask, dstStageMask))
            return false;

        oldLayout = usageToLayout(oldUsage);
        newLayout = usageToLayout(newUsage);

        VkImageAspectFlags aspectMask = image.desc.texture.usage == TextureUsage::DEPTH_STENCIL_ATTACHMENT
                                            ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                            : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                         .srcStageMask = srcStageMask,
                                         .srcAccessMask = srcAccessMask,
                                         .dstStageMask = dstStageMask,
                                         .dstAccessMask = dstAccessMask,
                                         .oldLayout = oldLayout,
                                         .newLayout = newLayout,
                                         .image = image.image,
                                         .subresourceRange = {
                                             .aspectMask = aspectMask,
                                             .baseMipLevel = 0,
                                             .levelCount = image.mipLevels,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1,
                                         }};

        VkDependencyInfo dependencyInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                           .imageMemoryBarrierCount = 1,
                                           .pImageMemoryBarriers = &barrier};

        vkCmdPipelineBarrier2(commandBuffer.commandBuffer, &dependencyInfo);
        return true;
    }

    bool VulkanContext::beginCommandBuffer(const CommandBufferVKHandle &commandBuffer)
    {
        auto resetResult = vkResetCommandBuffer(commandBuffer.commandBuffer, 0);
        if (resetResult != VK_SUCCESS)
        {
            spdlog::error("Failed to reset command buffer. Error: {}", toVkResultString(resetResult));
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        auto beginResult = vkBeginCommandBuffer(commandBuffer.commandBuffer, &beginInfo);
        if (beginResult != VK_SUCCESS)
        {
            spdlog::error("Failed to begin command buffer. Error: {}", toVkResultString(beginResult));
            return false;
        }
        return true;
    }

    bool VulkanContext::endCommandBuffer(const CommandBufferVKHandle &commandBuffer)
    {
        auto result = vkEndCommandBuffer(commandBuffer.commandBuffer);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to end command buffer. Error: {}", toVkResultString(result));
            return false;
        }
        return true;
    }

    bool VulkanContext::beginRendering(const CommandBufferVKHandle &commandBuffer, const TextureVKHandle &colorAttachment, const TextureVKHandle &depthAttachment, const glm::vec4 &clearColor)
    {
        // begin dynamic rendering
        VkRenderingAttachmentInfo colorAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = colorAttachment.view,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{.color{clearColor.r, clearColor.g, clearColor.b, clearColor.a}}};

        VkRenderingAttachmentInfo depthAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depthAttachment.view,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = {.depthStencil = {1.0f, 0}}};

        // must match the pipeline's stencilAttachmentFormat (see createGraphicsPipeline).
        Format depthFormat = depthAttachment.desc.texture.format;
        bool hasStencil = depthFormat == Format::D24_UNORM_S8_UINT || depthFormat == Format::D32_SFLOAT_S8_UINT;

        VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{
                .extent{
                    .width = colorAttachment.desc.texture.width,
                    .height = colorAttachment.desc.texture.height}},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachmentInfo,
            .pDepthAttachment = &depthAttachmentInfo,
            .pStencilAttachment = hasStencil ? &depthAttachmentInfo : nullptr};

        vkCmdBeginRendering(commandBuffer.commandBuffer, &renderingInfo);
        return true;
    }

    bool VulkanContext::endRendering(const CommandBufferVKHandle &commandBuffer)
    {
        vkCmdEndRendering(commandBuffer.commandBuffer);
        return true;
    }

    bool VulkanContext::setViewport(const CommandBufferVKHandle &commandBuffer, float x, float y, float width, float height, float minDepth, float maxDepth)
    {
        VkViewport viewport = {
            .x = x,
            .y = y,
            .width = width,
            .height = height,
            .minDepth = minDepth,
            .maxDepth = maxDepth};

        vkCmdSetViewport(commandBuffer.commandBuffer, 0, 1, &viewport);
        return true;
    }

    bool VulkanContext::setScissor(const CommandBufferVKHandle &commandBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        VkRect2D scissor = {
            .offset = {x, y},
            .extent = {width, height}};

        vkCmdSetScissor(commandBuffer.commandBuffer, 0, 1, &scissor);
        return true;
    }

    bool VulkanContext::submit(const CommandBufferVKHandle &commandBuffer, const std::vector<SemaphoreVKHandle> &waitSemaphores, const std::vector<SemaphoreVKHandle> &signalSemaphores, const FenceVKHandle &fence)
    {
        // pWaitDstStageMask holds one entry per wait semaphore.
        std::vector<VkPipelineStageFlags> waitStages(waitSemaphores.size(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        std::vector<VkSemaphore> waitVkSemaphores;
        for (const auto &waitSemaphore : waitSemaphores)
        {
            waitVkSemaphores.push_back(waitSemaphore.semaphore);
        }

        std::vector<VkSemaphore> signalVkSemaphores;
        for (const auto &signalSemaphore : signalSemaphores)
        {
            signalVkSemaphores.push_back(signalSemaphore.semaphore);
        }

        VkSubmitInfo submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = static_cast<uint32_t>(waitVkSemaphores.size()),
            .pWaitSemaphores = waitVkSemaphores.data(),
            .pWaitDstStageMask = waitStages.data(),
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer.commandBuffer,
            .signalSemaphoreCount = static_cast<uint32_t>(signalVkSemaphores.size()),
            .pSignalSemaphores = signalVkSemaphores.data()};

        auto result = vkQueueSubmit(this->graphicsQueue, 1, &submitInfo, fence.fence);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to submit command buffer. Error: {}", toVkResultString(result));
            return false;
        }
        return true;
    }
    SwapchainStatus VulkanContext::present(uint32_t imageIndex, const SemaphoreVKHandle &waitSemaphore)
    {
        if (swapchain.swapchain.swapchain == VK_NULL_HANDLE)
            return SwapchainStatus::OUT_OF_DATE;

        VkPresentInfoKHR presentInfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &waitSemaphore.semaphore,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain.swapchain.swapchain,
            .pImageIndices = &imageIndex};

        auto result = vkQueuePresentKHR(this->graphicsQueue, &presentInfo);
        auto status = toSwapchainStatus(result);
        if (status == SwapchainStatus::FAILURE)
            spdlog::error("Failed to present swapchain image. Error: {}", toVkResultString(result));
        return status;
    }

    void VulkanContext::removeBuffer(const BufferVKHandle &buffer)
    {
        vmaDestroyBuffer(this->allocator, buffer.buffer, buffer.allocation);
    }

    void VulkanContext::removeTexture(const TextureVKHandle &texture)
    {
        if (texture.sampler != VK_NULL_HANDLE)
            vkDestroySampler(this->device.device, texture.sampler, nullptr);
        vkDestroyImageView(this->device.device, texture.view, nullptr);
        vmaDestroyImage(this->allocator, texture.image, texture.allocation);
    }

    void VulkanContext::removeShader(const ShaderVKHandle &shader)
    {
        vkDestroyShaderModule(this->device.device, shader.module, nullptr);
    }

    void VulkanContext::removePipeline(const PipelineVKHandle &pipeline)
    {
        vkDestroyPipeline(this->device.device, pipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(this->device.device, pipeline.layout, nullptr);
    }

    void VulkanContext::removeCommandPool(const CommandPoolVKHandle &commandPool)
    {
        vkDestroyCommandPool(this->device.device, commandPool.commandPool, nullptr);
    }

    void VulkanContext::removeDescriptorPool(const DescriptorPoolVKHandle &descriptorPool)
    {
        vkDestroyDescriptorPool(this->device.device, descriptorPool.pool, nullptr);
    }

    void VulkanContext::removeDescriptorSetLayout(const DescriptorSetLayoutVKHandle &descriptorSetLayout)
    {
        vkDestroyDescriptorSetLayout(this->device.device, descriptorSetLayout.layout, nullptr);
    }

    void VulkanContext::removeSemaphore(const SemaphoreVKHandle &semaphore)
    {
        vkDestroySemaphore(this->device.device, semaphore.semaphore, nullptr);
    }

    void VulkanContext::removeFence(const FenceVKHandle &fence)
    {
        vkDestroyFence(this->device.device, fence.fence, nullptr);
    }

    void VulkanContext::bindPipeline(const CommandBufferVKHandle &commandBuffer, const PipelineVKHandle &pipeline)
    {
        vkCmdBindPipeline(commandBuffer.commandBuffer, pipeline.desc.type == ResourceType::GRAPHICS_PIPELINE ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    }

    void VulkanContext::bindIndexBuffer(const CommandBufferVKHandle &commandBuffer, const BufferVKHandle &buffer, uint64_t offset, Format indexType)
    {
        auto vkIndexType = toVkIndexType(indexType);
        vkCmdBindIndexBuffer(commandBuffer.commandBuffer, buffer.buffer, offset, vkIndexType);
    }

    void VulkanContext::bindDescriptorSet(const CommandBufferVKHandle &commandBuffer, const PipelineVKHandle &pipeline, const DescriptorSetVKHandle &descriptorSet, uint32_t setIndex)
    {
        vkCmdBindDescriptorSets(commandBuffer.commandBuffer,
                                pipeline.desc.type == ResourceType::GRAPHICS_PIPELINE ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout,
                                setIndex,
                                1,
                                &descriptorSet.set,
                                0,
                                nullptr);
    }

    void VulkanContext::pushConstants(const CommandBufferVKHandle &commandBuffer, const PipelineVKHandle &pipeline, ShaderType stage, const void *data, uint32_t size, uint32_t offset)
    {
        auto stageFlags = toVkShaderStageFlags(stage);
        vkCmdPushConstants(commandBuffer.commandBuffer, pipeline.layout, stageFlags, offset, size, data);
    }

    void VulkanContext::setUniform(const CommandBufferVKHandle &commandBuffer, const std::string &name, const void *data, size_t size)
    {
        (void)commandBuffer;
        (void)name;
        (void)data;
        (void)size;
    }

    void VulkanContext::draw(const CommandBufferVKHandle &commandBuffer, uint32_t vertexCount, uint32_t instanceCount)
    {
        vkCmdDraw(commandBuffer.commandBuffer, vertexCount, instanceCount, 0, 0);
    }

    void VulkanContext::drawIndexed(const CommandBufferVKHandle &commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t vertexOffset, uint32_t firstIndex, uint32_t firstInstance)
    {
        vkCmdDrawIndexed(commandBuffer.commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void VulkanContext::drawIndexedIndirect(const CommandBufferVKHandle &commandBuffer, const BufferVKHandle &buffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        vkCmdDrawIndexedIndirect(commandBuffer.commandBuffer, buffer.buffer, offset, drawCount, stride);
    }

    bool VulkanContext::waitForFence(const FenceVKHandle &fence, uint64_t timeout)
    {
        auto result = vkWaitForFences(this->device.device, 1, &fence.fence, VK_TRUE, timeout);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to wait for fence. Error: {}", toVkResultString(result));
            return false;
        }
        return true;
    }

    bool VulkanContext::resetFence(const FenceVKHandle &fence)
    {
        auto result = vkResetFences(this->device.device, 1, &fence.fence);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to reset fence. Error: {}", toVkResultString(result));
            return false;
        }
        return true;
    }

    SwapchainStatus VulkanContext::acquireNextImage(const SemaphoreVKHandle &semaphore, uint64_t timeout, uint32_t &imageIndex)
    {
        if (swapchain.swapchain.swapchain == VK_NULL_HANDLE)
            return SwapchainStatus::OUT_OF_DATE;

        auto result = vkAcquireNextImageKHR(this->device.device, this->swapchain.swapchain, timeout, semaphore.semaphore, VK_NULL_HANDLE, &imageIndex);
        auto status = toSwapchainStatus(result);
        if (status == SwapchainStatus::FAILURE)
            spdlog::error("Failed to acquire next image. Error: {}", toVkResultString(result));
        return status;
    }

    VRAMStats VulkanContext::queryVRAMStats()
    {
        VRAMStats vramStats{};

        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProperties{};
        budgetProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        budgetProperties.pNext = nullptr;

        VkPhysicalDeviceMemoryProperties2 memoryProperties2{};
        memoryProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        memoryProperties2.pNext = &budgetProperties;

        vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &memoryProperties2);

        for (uint32_t i = 0; i < memoryProperties2.memoryProperties.memoryHeapCount; ++i)
        {
            const VkMemoryHeap &heap = memoryProperties2.memoryProperties.memoryHeaps[i];

            VkDeviceSize budget = budgetProperties.heapBudget[i];
            VkDeviceSize usage = budgetProperties.heapUsage[i];

            bool isDeviceLocal = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;

            if (isDeviceLocal)
            {
                vramStats.vramBudget += budget;
                vramStats.vramUsage += usage;
            }
            else
            {
                vramStats.hostBudget += budget;
                vramStats.hostUsage += usage;
            }
        }

        return vramStats;
    }

    void VulkanContext::setObjectName(VkObjectType type, uint64_t handle, const std::string &name)
    {
        // Skip naming when the extension is not enabled; the function pointer is null or invalid then (B3)
        if (!this->debugUtilsEnabled || !this->dispatchTable.fp_vkSetDebugUtilsObjectNameEXT || name.empty())
            return;

        VkDebugUtilsObjectNameInfoEXT nameInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        nameInfo.objectType = type;
        nameInfo.objectHandle = handle;
        nameInfo.pObjectName = name.c_str();

        this->dispatchTable.setDebugUtilsObjectNameEXT(&nameInfo);
    }

}
