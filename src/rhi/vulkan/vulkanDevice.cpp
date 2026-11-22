#include "rhi/vulkan/vulkanDevice.h"
#include "rhi/vulkan/vulkanCommandBuffer.h"
#include "rhi/vulkan/vulkanSwapchain.h"
#include "rhi/vulkan/vulkanUtils.h"
#include "rhi/mipChain.h"
#include "platform/windowSurface.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include <vector>

namespace rasm::rhi
{
    namespace
    {
        // The formats of a texture with an encoded view (TextureDesc::encodedView): the image may be viewed as either.
        constexpr VkFormat ENCODED_VIEW_FORMATS[2] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
        constexpr VkImageFormatListCreateInfo ENCODED_VIEW_FORMAT_LIST = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                                                          .viewFormatCount = 2,
                                                                          .pViewFormats = ENCODED_VIEW_FORMATS};

#if defined(_WIN32)
        constexpr const char *LOADER_NAME = "vulkan-1.dll";
#elif defined(__APPLE__)
        constexpr const char *LOADER_NAME = "libvulkan.dylib";
#else
        constexpr const char *LOADER_NAME = "libvulkan.so.1";
#endif

        bool hasEncodedView(const TextureDesc &desc)
        {
            return desc.encodedView && desc.format == Format::R8G8B8A8_SRGB && !desc.cube;
        }

        // The loader and volk's global functions, for the whole process (D24). With a window they come from SDL, which
        // loaded the loader for the window, so the surface and the instance come from one library; headless, volk finds
        // the loader. A 1.3 loader is enough: Vulkan 1.4 added only device-level commands, and those come from the
        // driver through each device's table.
        bool loadVulkan(bool window)
        {
            if (window)
            {
                const PFN_vkGetInstanceProcAddr getInstanceProcAddr = getWindowVulkanLoader();
                if (getInstanceProcAddr == nullptr)
                {
                    spdlog::error("The window has no Vulkan loader ({}).", LOADER_NAME);
                    return false;
                }
                volkInitializeCustom(getInstanceProcAddr);
            }
            else if (volkInitialize() != VK_SUCCESS)
            {
                spdlog::error("No Vulkan loader found ({}). Install a GPU driver with Vulkan 1.4.", LOADER_NAME);
                return false;
            }

            // A 1.0 loader has no vkEnumerateInstanceVersion, and volk returns 0 for it.
            const uint32_t version = std::max(volkGetInstanceVersion(), static_cast<uint32_t>(VK_API_VERSION_1_0));
            if (version < VK_API_VERSION_1_3)
            {
                spdlog::error("The Vulkan loader ({}) is version {}.{}.{}, and RASM needs 1.3 or newer. Update the GPU driver.", LOADER_NAME,
                              VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
                return false;
            }
            spdlog::info("Vulkan loader {}.{}.{}.", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
            return true;
        }

        // Names each GPU that is older than Vulkan 1.4, which vk-bootstrap only lists among its other reasons.
        void logOldDevices(VkInstance instance)
        {
            uint32_t count = 0;
            if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0)
            {
                spdlog::error("No GPU with Vulkan found. Install a GPU driver with Vulkan 1.4.");
                return;
            }
            std::vector<VkPhysicalDevice> devices(count);
            if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) < VK_SUCCESS)
                return;
            for (uint32_t i = 0; i < count; ++i)
            {
                VkPhysicalDeviceProperties properties = {};
                vkGetPhysicalDeviceProperties(devices[i], &properties);
                if (properties.apiVersion < VK_API_VERSION_1_4)
                    spdlog::error("GPU '{}' has Vulkan {}.{}.{}, and RASM needs 1.4. Update its driver.", properties.deviceName,
                                  VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
            }
        }

        vkb::Result<vkb::Instance> initInstance(const DeviceDesc &desc, bool &debugUtilsEnabled)
        {
            debugUtilsEnabled = false;

            // vk-bootstrap calls through volk's loader instead of loading its own.
            vkb::InstanceBuilder instanceBuilder(vkGetInstanceProcAddr);

            auto systemInfoRet = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
            if (!systemInfoRet)
            {
                spdlog::error("Failed to get system info. Error: {}", systemInfoRet.error().message());
                return vkb::Result<vkb::Instance>{systemInfoRet.error()};
            }

            const auto &systemInfo = systemInfoRet.value();

            // Debug utils is cheap, so enable it whenever it is available: object names show up in
            // RenderDoc / Nsight even without validation (B3).
            if (systemInfo.debug_utils_available)
            {
                instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                debugUtilsEnabled = true;
            }

            // Read by vkCreateInstance, which build() below calls.
            const VkBool32 settingOn = VK_TRUE;
            const bool validationOn = desc.enableValidation && systemInfo.validation_layers_available;
            if (desc.enableValidation && !systemInfo.validation_layers_available)
                spdlog::warn("Vulkan validation layers are not installed, validation is off");
            if (!validationOn && (desc.enableSyncValidation || desc.enableGpuValidation))
                spdlog::warn("enableSyncValidation and enableGpuValidation need enableValidation and the validation layers; ignored");

            if (validationOn)
            {
                instanceBuilder.enable_validation_layers();
                // The validation layer provides this extension; the settings below are chained through it.
                if ((desc.enableSyncValidation || desc.enableGpuValidation) && systemInfo.is_extension_available(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME))
                    instanceBuilder.enable_extension(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
                if (desc.enableSyncValidation)
                    instanceBuilder.add_layer_setting({"VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingOn});
                if (desc.enableGpuValidation)
                    instanceBuilder.add_layer_setting({"VK_LAYER_KHRONOS_validation", "gpuav_enable", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingOn});

                // Validation needs to send errors via a callback, have vk-bootstrap do it
                if (debugUtilsEnabled)
                    instanceBuilder.use_default_debug_messenger();
            }

            // Without a window the instance has no surface extensions, so a device without a presentation engine works.
            if (desc.window == nullptr)
                instanceBuilder.set_headless(true);

            auto instanceRet = instanceBuilder
                                   .set_app_name(desc.appName.empty() ? "rasm_app" : desc.appName.c_str())
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

        const char *toDeviceTypeString(VkPhysicalDeviceType type)
        {
            switch (type)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                return "Discrete";
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                return "Integrated";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                return "Virtual";
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                return "CPU";
            default:
                return "Other";
            }
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
                .drawIndirectCount = VK_TRUE,
                .storagePushConstant8 = VK_TRUE,
                .shaderInt8 = VK_TRUE,
                .descriptorIndexing = VK_TRUE,
                .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
                .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
                .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
                .descriptorBindingPartiallyBound = VK_TRUE,
                .descriptorBindingVariableDescriptorCount = VK_TRUE,
                .runtimeDescriptorArray = VK_TRUE,
                .scalarBlockLayout = VK_TRUE,
                .hostQueryReset = VK_TRUE,
                .timelineSemaphore = VK_TRUE,
                .bufferDeviceAddress = VK_TRUE,
            };

            VkPhysicalDeviceVulkan13Features enabledVk13Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                .pNext = &enabledVk12Features,
                .shaderDemoteToHelperInvocation = VK_TRUE,
                .synchronization2 = VK_TRUE,
                .dynamicRendering = VK_TRUE,
            };

            VkPhysicalDeviceFeatures enabledVk10Features{
                .multiDrawIndirect = VK_TRUE,
                .drawIndirectFirstInstance = VK_TRUE,
                .samplerAnisotropy = VK_TRUE,
                .shaderInt64 = VK_TRUE,
            };

            // A headless device presents nothing, so it needs no surface and no swapchain extension.
            physDeviceSelector.set_required_features_13(enabledVk13Features)
                .set_required_features_12(enabledVk12Features)
                .set_required_features_11(enabledVk11Features)
                .set_required_features(enabledVk10Features)
                .add_required_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            if (surface != VK_NULL_HANDLE)
                physDeviceSelector.set_surface(surface).add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

            // select() grabs a PhysicalDevice, By default, this will prefer a discrete GPU.
            auto physicalDeviceSelectorReturn = physDeviceSelector.select();

            if (!physicalDeviceSelectorReturn)
            {
                // If no suitable devices were found, detailed_failure_reasons() will contain a list of reasons why.
                logOldDevices(vkbInstance.instance);
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

            const auto &physicalDevice = physicalDeviceSelectorReturn.value();
            spdlog::info(
                "Selected GPU information:\n"
                "\t\t\t\t\tname: {} -- type: {} --  memory: {} MB\n"
                "\t\t\t\t\tdriver version: {}.{}.{} --  API version: {}.{}.{}",
                physicalDevice.properties.deviceName,
                toDeviceTypeString(physicalDevice.properties.deviceType),
                physicalDevice.memory_properties.memoryHeaps[0].size / (1024ull * 1024),
                VK_VERSION_MAJOR(physicalDevice.properties.driverVersion),
                VK_VERSION_MINOR(physicalDevice.properties.driverVersion),
                VK_VERSION_PATCH(physicalDevice.properties.driverVersion),
                VK_VERSION_MAJOR(physicalDevice.properties.apiVersion),
                VK_VERSION_MINOR(physicalDevice.properties.apiVersion),
                VK_VERSION_PATCH(physicalDevice.properties.apiVersion));

            return physicalDeviceSelectorReturn;
        }

        VkBufferUsageFlags toVkBufferUsageFlags(BufferUsage usage)
        {
            // A staging buffer is only copied from or to. VMA counts any other usage as GPU access and then prefers video
            // memory for a host-visible buffer, which would put a staging ring in the small BAR heap (P10).
            if (usage == BufferUsage::TRANSFER)
                return VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

            // Every other buffer is created with the device-address bit so that its GPU address can be
            // handed to shaders through push constants / other buffers regardless of its primary usage.
            // The transfer bits let any buffer be a staging source or a copy destination.
            VkBufferUsageFlags flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

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
            default:
                return flags;
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

        VkImageUsageFlags toVkImageUsageFlags(TextureUsage usage)
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
            case TextureUsage::SAMPLED_COLOR_ATTACHMENT:
                return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            case TextureUsage::SAMPLED_DEPTH_ATTACHMENT:
                return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            case TextureUsage::SAMPLED_STORAGE:
                return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            case TextureUsage::TRANSIENT_ATTACHMENT:
                return VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
            case TextureUsage::INPUT_ATTACHMENT:
                return VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            default:
                return 0;
            }
        }

        VkDescriptorType toVkDescriptorType(DescriptorType type)
        {
            switch (type)
            {
            case DescriptorType::UNIFORM_BUFFER:
                return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case DescriptorType::SAMPLER:
                return VK_DESCRIPTOR_TYPE_SAMPLER;
            case DescriptorType::COMBINED_IMAGE_SAMPLER:
                return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            case DescriptorType::STORAGE_IMAGE:
                return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            default:
                return VK_DESCRIPTOR_TYPE_MAX_ENUM;
            }
        }

        VkFilter toVkFilter(Filter filter)
        {
            return filter == Filter::NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        }

        VkSamplerAddressMode toVkAddressMode(AddressMode addressMode)
        {
            switch (addressMode)
            {
            case AddressMode::MIRRORED_REPEAT:
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case AddressMode::CLAMP_TO_EDGE:
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case AddressMode::REPEAT:
            default:
                return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            }
        }

        // The set and a PUSH_CONSTANTS_SIZE range that every stage of every pipeline sees, so graphics and compute
        // pipelines share one layout (P8).
        VkPipelineLayout createPipelineLayout(const VolkDeviceTable &vk, VkDevice device, VkDescriptorSetLayout setLayout)
        {
            VkPushConstantRange pushConstantRange{
                .stageFlags = PUSH_CONSTANT_STAGES,
                .size = PUSH_CONSTANTS_SIZE};

            VkDescriptorSetLayout setLayouts[] = {setLayout};

            VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = setLayouts,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &pushConstantRange};

            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            auto result = vk.vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
            if (result != VK_SUCCESS)
            {
                spdlog::error("Failed to create pipeline layout. Error: {}", toVkResultString(result));
                return VK_NULL_HANDLE;
            }
            return pipelineLayout;
        }

        // Drivers may accept an oversized image instead of failing, so this can't be left to image creation. A cube's
        // faces are square and have their own limit.
        bool fitsDevice(const TextureDesc &desc, const VkPhysicalDeviceLimits &limits)
        {
            if (desc.cube && desc.width != desc.height)
            {
                spdlog::error("Cube texture '{}' is {}x{}, and a cube's faces must be square.", desc.name, desc.width, desc.height);
                return false;
            }
            const uint32_t maxExtent = desc.cube ? limits.maxImageDimensionCube : limits.maxImageDimension2D;
            if (desc.width <= maxExtent && desc.height <= maxExtent)
                return true;
            spdlog::error("Texture '{}' is {}x{}, the device limit is {}.", desc.name, desc.width, desc.height, maxExtent);
            return false;
        }

        // Only sampled RGBA8 images get the full chain by default (RenderContext::fillTexture makes it). Render targets and
        // other formats keep one level, unless the desc asks for more; a count past the full chain is clamped to it.
        uint32_t mipLevelCount(const TextureDesc &desc)
        {
            const uint32_t fullChain = std::min(getFullMipCount(desc.width, desc.height), MAX_TEXTURE_MIPS);
            if (desc.mipLevels > 0)
                return std::min(desc.mipLevels, fullChain);
            if (desc.usage == TextureUsage::SAMPLED &&
                (desc.format == Format::R8G8B8A8_UNORM || desc.format == Format::R8G8B8A8_SRGB ||
                 desc.format == Format::B8G8R8A8_SRGB))
                return fullChain;
            return 1;
        }

        // An image used by several queue families is shared by them (VulkanDevice::getImageFamilies). Concurrent sharing
        // needs no ownership transfers between the queues; some GPUs compress such images less.
        // A sampled texture is filled either on the host (host image copy) or by a copy from the staging ring.
        VkImageCreateInfo imageCreateInfo(const TextureDesc &desc, uint32_t mipLevels, bool hostCopy, const uint32_t *sharedFamilies, uint32_t sharedFamilyCount)
        {
            VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = desc.width;
            imageInfo.extent.height = desc.height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = mipLevels;
            imageInfo.arrayLayers = desc.cube ? CUBE_FACES : 1;
            imageInfo.flags = desc.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
            if (hasEncodedView(desc))
            {
                imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
                imageInfo.pNext = &ENCODED_VIEW_FORMAT_LIST;
            }
            imageInfo.format = toVkFormat(desc.format);
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = toVkImageUsageFlags(desc.usage);
            if (desc.readable)
                imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (desc.usage == TextureUsage::SAMPLED)
                imageInfo.usage |= hostCopy ? VK_IMAGE_USAGE_HOST_TRANSFER_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imageInfo.samples = static_cast<VkSampleCountFlagBits>(std::max(desc.samples, 1u));
            imageInfo.sharingMode = sharedFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.queueFamilyIndexCount = sharedFamilyCount > 1 ? sharedFamilyCount : 0;
            imageInfo.pQueueFamilyIndices = sharedFamilyCount > 1 ? sharedFamilies : nullptr;
            return imageInfo;
        }

        void destroyImageViews(const VolkDeviceTable &vk, VkDevice device, VulkanTexture &texture)
        {
            vk.vkDestroyImageView(device, texture.view, nullptr);
            for (VkImageView &faceView : texture.faceViews)
            {
                vk.vkDestroyImageView(device, faceView, nullptr);
                faceView = VK_NULL_HANDLE;
            }
            for (VkImageView &mipView : texture.mipViews)
            {
                vk.vkDestroyImageView(device, mipView, nullptr);
                mipView = VK_NULL_HANDLE;
            }
            texture.view = VK_NULL_HANDLE;
            vk.vkDestroyImageView(device, texture.encodedView, nullptr);
            texture.encodedView = VK_NULL_HANDLE;
        }

        // A cube gets a cube view of all its faces, for sampling, and a 2D view of each face's first mip, for drawing into
        // it. A storage texture gets a 2D array view of each mip with every layer, for compute to write: a storage view
        // holds one mip, and one view type serves a 2D texture (1 layer) and a cube (6) alike.
        VkResult createImageViews(const VolkDeviceTable &vk, VkDevice device, VulkanTexture &texture)
        {
            VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = texture.image;
            viewInfo.viewType = texture.desc.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = toVkFormat(texture.desc.format);
            viewInfo.subresourceRange.aspectMask = toVkAspectMask(texture.desc.format);
            viewInfo.subresourceRange.levelCount = texture.mipLevels;
            viewInfo.subresourceRange.layerCount = texture.desc.cube ? CUBE_FACES : 1;
            VkResult result = vk.vkCreateImageView(device, &viewInfo, nullptr, &texture.view);
            for (uint32_t face = 0; texture.desc.cube && result == VK_SUCCESS && face < CUBE_FACES; ++face)
            {
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.subresourceRange.baseArrayLayer = face;
                viewInfo.subresourceRange.layerCount = 1;
                viewInfo.subresourceRange.levelCount = 1;
                result = vk.vkCreateImageView(device, &viewInfo, nullptr, &texture.faceViews[face]);
            }
            for (uint32_t mip = 0; texture.desc.usage == TextureUsage::SAMPLED_STORAGE && result == VK_SUCCESS && mip < texture.mipLevels; ++mip)
            {
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                viewInfo.subresourceRange.baseMipLevel = mip;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = texture.desc.cube ? CUBE_FACES : 1;
                result = vk.vkCreateImageView(device, &viewInfo, nullptr, &texture.mipViews[mip]);
            }
            // An attachment view: mip 0, layer 0, as beginRendering draws.
            if (hasEncodedView(texture.desc) && result == VK_SUCCESS)
            {
                const VkImageViewCreateInfo encodedInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                           .image = texture.image,
                                                           .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                                           .format = VK_FORMAT_R8G8B8A8_UNORM,
                                                           .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
                result = vk.vkCreateImageView(device, &encodedInfo, nullptr, &texture.encodedView);
            }
            if (result != VK_SUCCESS)
                destroyImageViews(vk, device, texture);
            return result;
        }
    }

    std::unique_ptr<Device> createVulkanDevice(const DeviceDesc &desc)
    {
        auto device = std::make_unique<VulkanDevice>();
        if (!device->initialize(desc))
            return nullptr;
        return device;
    }

    VulkanDevice::~VulkanDevice()
    {
        waitIdle();

        if (device.device != VK_NULL_HANDLE)
            pipelineCache.save(vk, device.device);
        if (allocator != nullptr)
            vmaDestroyAllocator(allocator);
        graphicsQueue.reset();
        computeQueue.reset();
        transferQueue.reset();
        if (device.device != VK_NULL_HANDLE)
            vkb::destroy_device(device);
        if (surface != VK_NULL_HANDLE)
            vkb::destroy_surface(instance, surface);
        if (instance.instance != VK_NULL_HANDLE)
            vkb::destroy_instance(instance);
    }

    bool VulkanDevice::initialize(const DeviceDesc &desc)
    {
        if (!loadVulkan(desc.window != nullptr))
            return false;

        auto vkbInstance = initInstance(desc, debugUtilsEnabled);
        if (!vkbInstance)
            return false;
        instance = vkbInstance.value();
        volkLoadInstanceOnly(instance.instance);

        // Headless: no window, so no surface; the frames go to an OffscreenSwapchain.
        if (desc.window != nullptr && !createWindowSurface(*desc.window, instance.instance, &surface))
            return false;

        auto vkbPhysicalDevice = initPhysicalDevice(instance, surface);
        if (!vkbPhysicalDevice)
            return false;
        physicalDevice = vkbPhysicalDevice.value();

        // Block-compressed textures, each family only where the GPU has it: BC on desktop and Apple GPUs, ASTC and ETC2
        // on mobile and Apple GPUs. vk-bootstrap enables a set only when all of it is present, so one at a time.
        const bool bc = physicalDevice.enable_features_if_present(VkPhysicalDeviceFeatures{.textureCompressionBC = VK_TRUE});
        const bool astc = physicalDevice.enable_features_if_present(VkPhysicalDeviceFeatures{.textureCompressionASTC_LDR = VK_TRUE});
        const bool etc2 = physicalDevice.enable_features_if_present(VkPhysicalDeviceFeatures{.textureCompressionETC2 = VK_TRUE});
        spdlog::info("Texture compression: BC {}, ASTC {}, ETC2 {}.", bc ? "yes" : "no", astc ? "yes" : "no", etc2 ? "yes" : "no");
        // Host image copy only saves the staging copy, so a GPU without it uses the staging ring for every texture.
        const bool hostImageCopy = desc.hostImageCopy && physicalDevice.enable_extension_features_if_present(VkPhysicalDeviceVulkan14Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .hostImageCopy = VK_TRUE});
        // A UNORM view of the sRGB swapchain lets the UI blend on the stored sRGB numbers, as Dear ImGui is designed for.
        swapchainMutable = surface != VK_NULL_HANDLE && physicalDevice.enable_extension_if_present(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);

        auto vkbDevice = vkb::DeviceBuilder{physicalDevice}.build();
        if (!vkbDevice)
        {
            spdlog::error("Failed to create logical device. Error: {}", vkbDevice.error().message());
            return false;
        }
        device = vkbDevice.value();
        // This device's functions, straight from the driver (or the first layer): no loader trampoline per call, and a
        // second device has its own table.
        volkLoadDeviceTable(&vk, device.device);
        pipelineCache.load(vk, device.device, physicalDevice.properties, desc.cacheDir);

        auto queueRet = device.get_queue(vkb::QueueType::graphics);
        auto queueIndexRet = device.get_queue_index(vkb::QueueType::graphics);
        if (!queueRet || !queueIndexRet)
        {
            spdlog::error("Failed to get the graphics queue.");
            return false;
        }
        graphicsQueue = std::make_unique<VulkanQueue>(vk, queueRet.value(), queueIndexRet.value());
        sharedFamilies[0] = uploadFamilies[0] = bufferFamilies[0] = queueIndexRet.value();

        const uint32_t timestampBits = device.queue_families[queueIndexRet.value()].timestampValidBits;
        timestampMask = timestampBits >= 64 ? ~0ull : (1ull << timestampBits) - 1;
        timestampPeriod = physicalDevice.properties.limits.timestampPeriod;
        VkPhysicalDeviceVulkan12Properties vulkan12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceMaintenance4Properties maintenance4 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES, &vulkan12};
        VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &maintenance4};
        vkGetPhysicalDeviceProperties2(physicalDevice.physical_device, &properties2);
        maxBufferSize = maintenance4.maxBufferSize;
        maxStorageImages = std::min(vulkan12.maxDescriptorSetUpdateAfterBindStorageImages, vulkan12.maxPerStageDescriptorUpdateAfterBindStorageImages);
        // A multisampled scene target is drawn into (color and depth), and its color and depth are sampled: the resolve
        // and the Hi-Z read them sample by sample.
        const VkPhysicalDeviceLimits &limits = physicalDevice.properties.limits;
        sampleCounts = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts & limits.sampledImageColorSampleCounts & limits.sampledImageDepthSampleCounts;
        // Resizable BAR, or memory shared with the CPU: a host-visible type lives in the largest device-local heap.
        const VkPhysicalDeviceMemoryProperties &memory = physicalDevice.memory_properties;
        VkDeviceSize largestVram = 0;
        for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap)
        {
            if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
                largestVram = std::max(largestVram, memory.memoryHeaps[heap].size);
        }
        for (uint32_t type = 0; type < memory.memoryTypeCount; ++type)
        {
            const VkMemoryPropertyFlags flags = memory.memoryTypes[type].propertyFlags;
            const VkMemoryPropertyFlags mapped = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            wholeVramMapped = wholeVramMapped || ((flags & mapped) == mapped && memory.memoryHeaps[memory.memoryTypes[type].heapIndex].size >= largestVram);
        }

        // A queue family with compute and no graphics runs beside the graphics queue. Its timestamps must match the
        // graphics queue's, since the GPU timers of both share one mask.
        if (desc.asyncCompute)
        {
            // vk-bootstrap's compute queue comes from a family without graphics, preferring one without transfer.
            auto computeRet = device.get_queue(vkb::QueueType::compute);
            auto computeIndexRet = device.get_queue_index(vkb::QueueType::compute);
            const bool separate = computeRet && computeIndexRet && (device.queue_families[computeIndexRet.value()].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0;
            if (separate && device.queue_families[computeIndexRet.value()].timestampValidBits == timestampBits)
            {
                computeQueue = std::make_unique<VulkanQueue>(vk, computeRet.value(), computeIndexRet.value());
                sharedFamilies[sharedFamilyCount++] = computeIndexRet.value();
                uploadFamilies[uploadFamilyCount++] = computeIndexRet.value();
                bufferFamilies[bufferFamilyCount++] = computeIndexRet.value();
                spdlog::info("Async compute on queue family {}.", computeIndexRet.value());
            }
            else
            {
                spdlog::info("No separate compute queue with matching timestamps; compute passes run on the graphics queue.");
            }
        }

        // A family that only copies is the GPU's copy engine (DMA), which runs next to the graphics queue: NVIDIA and AMD
        // GPUs have one, lavapipe, MoltenVK and most Intel GPUs do not. The uploads copy rows of a mip at any offset, so its
        // copies must be exact to the texel (granularity 1). vk-bootstrap made one queue of every family.
        if (desc.transferQueue)
        {
            constexpr VkQueueFlags COPY_ONLY = VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_PROTECTED_BIT;
            for (uint32_t family = 0; family < device.queue_families.size() && !transferQueue; ++family)
            {
                const VkQueueFamilyProperties &properties = device.queue_families[family];
                const VkExtent3D granularity = properties.minImageTransferGranularity;
                if ((properties.queueFlags & VK_QUEUE_TRANSFER_BIT) == 0 || (properties.queueFlags & ~COPY_ONLY) != 0 || properties.queueCount == 0 ||
                    granularity.width != 1 || granularity.height != 1 || granularity.depth != 1)
                    continue;
                VkQueue queue = VK_NULL_HANDLE;
                vk.vkGetDeviceQueue(device.device, family, 0, &queue);
                transferQueue = std::make_unique<VulkanQueue>(vk, queue, family);
                transferTimestamps = timestampMask != 0 && properties.timestampValidBits == timestampBits;
                // Buffers are shared: a pool is copied into in one range while frames read the others. Images filled from
                // the ring are too with a compute queue, which shares them already; otherwise they stay exclusive and move
                // to the graphics family after their copies (Texture::ownershipTransfer).
                bufferFamilies[bufferFamilyCount++] = family;
                if (uploadFamilyCount > 1)
                    uploadFamilies[uploadFamilyCount++] = family;
                spdlog::info("Load uploads on the transfer queue of family {}.", family);
            }
            if (!transferQueue)
                spdlog::info("No queue family that only copies (with exact copies); load uploads go on the graphics queue.");
        }

        // After the queues: an image's memory types are asked with the sharing it will have.
        queryFormatSupport(hostImageCopy);

        VmaAllocatorCreateInfo allocatorCreateInfo = {};
        allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT | VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_4;
        allocatorCreateInfo.physicalDevice = physicalDevice.physical_device;
        allocatorCreateInfo.device = device.device;
        allocatorCreateInfo.instance = instance.instance;
        VmaVulkanFunctions vkFunctions = {};
        if (vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vkFunctions) != VK_SUCCESS)
        {
            spdlog::error("Failed to get VMA's Vulkan functions from volk.");
            return false;
        }
        allocatorCreateInfo.pVulkanFunctions = &vkFunctions;

        if (vmaCreateAllocator(&allocatorCreateInfo, &allocator) != VK_SUCCESS)
        {
            spdlog::error("Failed to create VMA allocator.");
            return false;
        }

        return true;
    }

    Buffer *VulkanDevice::createBuffer(const BufferDesc &desc)
    {
        // A size past the limit is invalid usage, which a driver need not report.
        if (desc.size > maxBufferSize)
        {
            spdlog::error("Buffer '{}' is {} bytes, the device limit is {}.", desc.name, desc.size, maxBufferSize);
            return nullptr;
        }
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.size;
        bufferInfo.usage = toVkBufferUsageFlags(desc.usage);
        if (bufferFamilyCount > 1)
        {
            bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
            bufferInfo.queueFamilyIndexCount = bufferFamilyCount;
            bufferInfo.pQueueFamilyIndices = bufferFamilies;
        }

        // P1: buffers that aren't hostVisible prefer device-local memory, and are mapped only when that memory is
        // also host-visible (ReBAR, integrated GPUs). Otherwise pMappedData stays null and they are filled by staging.
        VmaAllocationCreateInfo allocCI = {};
        allocCI.usage = VMA_MEMORY_USAGE_AUTO;
        allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
        // Write-only memory may be uncached or across the bus, so a buffer the CPU reads asks for random access.
        if (desc.hostReadable)
            allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        else if (!desc.hostVisible)
            allocCI.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
        // A staging buffer goes to system memory, unless all of video memory is host-visible: then the GPU's copies
        // read video memory instead of crossing the bus, and there is no small BAR heap to use up (P10).
        if (desc.usage == BufferUsage::TRANSFER && desc.hostVisible && !desc.hostReadable && wholeVramMapped)
            allocCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        auto *buffer = new VulkanBuffer();
        buffer->desc = desc;

        VmaAllocationInfo allocInfo;
        auto result = vmaCreateBuffer(allocator, &bufferInfo, &allocCI, &buffer->buffer, &buffer->allocation, &allocInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create buffer '{}'. Error: {}", desc.name, toVkResultString(result));
            delete buffer;
            return nullptr;
        }
        buffer->mappedData = allocInfo.pMappedData;
        buffer->mapped = buffer->mappedData != nullptr;

        VkBufferDeviceAddressInfo bufferAddressInfo = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bufferAddressInfo.buffer = buffer->buffer;
        if (desc.usage != BufferUsage::TRANSFER)
            buffer->deviceAddress = vk.vkGetBufferDeviceAddress(device.device, &bufferAddressInfo);

        setObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer->buffer), desc.name);
        return buffer;
    }

    Texture *VulkanDevice::createTexture(const TextureDesc &desc)
    {
        if (!fitsDevice(desc, physicalDevice.properties.limits))
            return nullptr;

        auto *texture = new VulkanTexture();
        texture->desc = desc;
        texture->mipLevels = mipLevelCount(desc);

        // Sampled textures share VMA's blocks, so making or freeing one is rarely a vkAllocateMemory or vkFreeMemory
        // (P22); attachments and storage textures, few and often large, get their own. No image asks for host access: a
        // host-copy image may use only the video-memory types queryFormatSupport found, and when those are full it is
        // made for the staging ring instead.
        VkResult result = VK_ERROR_UNKNOWN;
        for (bool hostCopy = usesHostCopy(desc);; hostCopy = false)
        {
            uint32_t familyCount = 0;
            const uint32_t *families = getImageFamilies(desc, hostCopy, familyCount);
            const VkImageCreateInfo imageInfo = imageCreateInfo(desc, texture->mipLevels, hostCopy, families, familyCount);
            VmaAllocationCreateInfo allocInfo = {};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = desc.usage == TextureUsage::SAMPLED ? 0 : VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            allocInfo.memoryTypeBits = hostCopy ? formatSupport[static_cast<uint32_t>(desc.format)].hostCopyTypes : 0;
            texture->hostCopy = hostCopy;
            texture->ownershipTransfer = transferQueue && familyCount == 1 && desc.usage == TextureUsage::SAMPLED && !hostCopy;
            result = vmaCreateImage(allocator, &imageInfo, &allocInfo, &texture->image, &texture->allocation, nullptr);
            if (result == VK_SUCCESS || !hostCopy)
                break;
        }
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create image '{}'. Error: {}", desc.name, toVkResultString(result));
            delete texture;
            return nullptr;
        }

        result = createImageViews(vk, device.device, *texture);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create image view for '{}'. Error: {}", desc.name, toVkResultString(result));
            vmaDestroyImage(allocator, texture->image, texture->allocation);
            delete texture;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(texture->image), desc.name);
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(texture->view), desc.name);
        return texture;
    }

    MemoryRequirements VulkanDevice::getTextureMemoryRequirements(const TextureDesc &desc)
    {
        uint32_t familyCount = 0;
        const uint32_t *families = getImageFamilies(desc, usesHostCopy(desc), familyCount);
        const VkImageCreateInfo imageInfo = imageCreateInfo(desc, mipLevelCount(desc), usesHostCopy(desc), families, familyCount);
        const VkDeviceImageMemoryRequirements info = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &imageInfo};
        VkMemoryRequirements2 requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vk.vkGetDeviceImageMemoryRequirements(device.device, &info, &requirements);
        return {.size = requirements.memoryRequirements.size,
                .alignment = requirements.memoryRequirements.alignment,
                .memoryTypeBits = requirements.memoryRequirements.memoryTypeBits};
    }

    Heap *VulkanDevice::createHeap(uint64_t size, uint64_t alignment, uint32_t memoryTypeBits, const std::string &name)
    {
        const VkMemoryRequirements requirements = {.size = size, .alignment = alignment, .memoryTypeBits = memoryTypeBits};
        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        auto *heap = new VulkanHeap();
        heap->size = size;
        heap->memoryTypeBits = memoryTypeBits;
        auto result = vmaAllocateMemory(allocator, &requirements, &allocInfo, &heap->allocation, nullptr);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to allocate heap '{}' of {} bytes. Error: {}", name, size, toVkResultString(result));
            delete heap;
            return nullptr;
        }
        vmaSetAllocationName(allocator, heap->allocation, name.c_str());
        return heap;
    }

    // The image does not own its memory: destroyTexture frees only the image, and the heap outlives it.
    Texture *VulkanDevice::createPlacedTexture(const TextureDesc &desc, Heap *heap, uint64_t offset)
    {
        if (!fitsDevice(desc, physicalDevice.properties.limits))
            return nullptr;

        const uint32_t mipLevels = mipLevelCount(desc);
        const bool hostCopy = usesHostCopy(desc);
        uint32_t familyCount = 0;
        const uint32_t *families = getImageFamilies(desc, hostCopy, familyCount);
        const VkImageCreateInfo imageInfo = imageCreateInfo(desc, mipLevels, hostCopy, families, familyCount);

        auto *texture = new VulkanTexture();
        texture->desc = desc;
        texture->mipLevels = mipLevels;
        texture->hostCopy = hostCopy;
        texture->ownershipTransfer = transferQueue && familyCount == 1 && desc.usage == TextureUsage::SAMPLED && !hostCopy;

        auto result = vmaCreateAliasingImage2(allocator, static_cast<VulkanHeap *>(heap)->allocation, offset, &imageInfo, &texture->image);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to place image '{}' at offset {}. Error: {}", desc.name, offset, toVkResultString(result));
            delete texture;
            return nullptr;
        }

        result = createImageViews(vk, device.device, *texture);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create image view for '{}'. Error: {}", desc.name, toVkResultString(result));
            vk.vkDestroyImage(device.device, texture->image, nullptr);
            delete texture;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(texture->image), desc.name);
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(texture->view), desc.name);
        return texture;
    }

    Sampler *VulkanDevice::createSampler(const SamplerDesc &desc)
    {
        // Anisotropy is already required when selecting the physical device. VK_LOD_CLAMP_NONE lets one sampler serve
        // textures with any number of mips; sampling clamps to the levels of each view.
        VkSamplerCreateInfo samplerInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = toVkFilter(desc.magFilter),
            .minFilter = toVkFilter(desc.minFilter),
            .mipmapMode = desc.mipFilter == Filter::NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = toVkAddressMode(desc.addressMode),
            .addressModeV = toVkAddressMode(desc.addressMode),
            .addressModeW = toVkAddressMode(desc.addressMode),
            .anisotropyEnable = desc.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = std::min(desc.maxAnisotropy, physicalDevice.properties.limits.maxSamplerAnisotropy),
            .maxLod = VK_LOD_CLAMP_NONE,
        };

        auto *sampler = new VulkanSampler();
        sampler->desc = desc;

        auto result = vk.vkCreateSampler(device.device, &samplerInfo, nullptr, &sampler->sampler);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create sampler '{}'. Error: {}", desc.name, toVkResultString(result));
            delete sampler;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(sampler->sampler), desc.name);
        return sampler;
    }

    Shader *VulkanDevice::createShader(const ShaderDesc &desc)
    {
        VkShaderModuleCreateInfo shaderModuleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderModuleInfo.codeSize = desc.code.size();
        shaderModuleInfo.pCode = reinterpret_cast<const uint32_t *>(desc.code.data());

        auto *shader = new VulkanShader();
        shader->desc = desc;

        auto result = vk.vkCreateShaderModule(device.device, &shaderModuleInfo, nullptr, &shader->module);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create shader module. Error: {}", toVkResultString(result));
            delete shader;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<uint64_t>(shader->module), desc.name);
        return shader;
    }

    Pipeline *VulkanDevice::createGraphicsPipeline(const PipelineDesc &desc, Shader *vertexShader, Shader *fragmentShader, DescriptorSetLayout *layout)
    {
        const VkPipelineLayout pipelineLayout = static_cast<VulkanDescriptorSetLayout *>(layout)->pipelineLayout;

        // With vertex pulling the shader fetches vertices itself, so use an empty vertex input state.
        // This is core Vulkan and avoids VK_EXT_vertex_input_dynamic_state, which MoltenVK doesn't support.
        VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = toVkTopology(desc.topology),
        };

        VkPipelineShaderStageCreateInfo shaderStages[] = {
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = static_cast<VulkanShader *>(vertexShader)->module,
                .pName = vertexShader->desc.entryPoint.c_str(),
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = static_cast<VulkanShader *>(fragmentShader)->module,
                .pName = fragmentShader->desc.entryPoint.c_str(),
            }};

        VkPipelineViewportStateCreateInfo viewportStateInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };

        // Cull mode is core dynamic state since Vulkan 1.3, so one pipeline serves every cull mode (D8).
        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};

        VkPipelineDynamicStateCreateInfo dynamicStateInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates)),
            .pDynamicStates = dynamicStates,
        };

        VkPipelineDepthStencilStateCreateInfo depthStencilInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            // A pipeline without depth (a swapchain pass) has no depth to test.
            .depthTestEnable = desc.depthStencilAttachmentFormat != Format::UNKNOWN && desc.depthTest ? VK_TRUE : VK_FALSE,
            .depthWriteEnable = desc.depthStencilAttachmentFormat != Format::UNKNOWN && desc.depthWrite ? VK_TRUE : VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL, // reversed-Z: nearer is larger; pairs with the 0 clear in VulkanCommandBuffer::beginRendering
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE};

        // Dynamic rendering: these formats must match the attachments passed to beginRendering.
        const uint32_t colorCount = colorAttachmentCount(desc.colorAttachmentFormats);
        VkFormat colorAttachmentFormats[MAX_COLOR_ATTACHMENTS] = {};
        for (uint32_t i = 0; i < colorCount; ++i)
            colorAttachmentFormats[i] = toVkFormat(desc.colorAttachmentFormats[i]);
        VkFormat depthAttachmentFormat = toVkFormat(desc.depthStencilAttachmentFormat);

        // Combined depth/stencil formats must declare the stencil aspect too (Metal validation asserts otherwise).
        VkPipelineRenderingCreateInfo pipelineRenderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = colorCount,
            .pColorAttachmentFormats = colorAttachmentFormats,
            .depthAttachmentFormat = depthAttachmentFormat,
            .stencilAttachmentFormat = hasStencil(desc.depthStencilAttachmentFormat) ? depthAttachmentFormat : VK_FORMAT_UNDEFINED,
        };

        // Premultiplied alpha: the shader already multiplied its color by alpha, so the source counts once and the
        // target keeps (1 - alpha) of its own, for color and alpha alike. Additive keeps all of the target's.
        const bool blend = desc.blendMode != BlendMode::NONE;
        const VkBlendFactor destination = desc.blendMode == BlendMode::ADDITIVE ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkPipelineColorBlendAttachmentState colorBlendAttachment = {
            .blendEnable = blend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = destination,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = blend ? destination : VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };

        VkPipelineColorBlendAttachmentState colorBlendAttachments[MAX_COLOR_ATTACHMENTS];
        std::fill(std::begin(colorBlendAttachments), std::end(colorBlendAttachments), colorBlendAttachment);

        VkPipelineColorBlendStateCreateInfo colorBlendInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = colorCount,
            .pAttachments = colorBlendAttachments};

        VkPipelineRasterizationStateCreateInfo rasterizerInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            // Counter-clockwise in the Y-up clip space; the negative-height viewport keeps it so in framebuffer space.
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f};

        VkPipelineMultisampleStateCreateInfo multisampleInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = static_cast<VkSampleCountFlagBits>(std::max(desc.samples, 1u)),
            .sampleShadingEnable = VK_FALSE,
            .alphaToCoverageEnable = desc.alphaToCoverage ? VK_TRUE : VK_FALSE};

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

        VkPipeline vkPipeline;
        auto result = vk.vkCreateGraphicsPipelines(device.device, pipelineCache.get(), 1, &pipelineInfo, nullptr, &vkPipeline);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create graphics pipeline. Error: {}", toVkResultString(result));
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(vkPipeline), desc.name);

        auto *pipeline = new VulkanPipeline();
        pipeline->desc = desc;
        pipeline->pipeline = vkPipeline;
        pipeline->layout = pipelineLayout;
        pipeline->bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        return pipeline;
    }

    Pipeline *VulkanDevice::createComputePipeline(const PipelineDesc &desc, Shader *computeShader, DescriptorSetLayout *layout)
    {
        const VkPipelineLayout pipelineLayout = static_cast<VulkanDescriptorSetLayout *>(layout)->pipelineLayout;

        VkComputePipelineCreateInfo pipelineInfo = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = static_cast<VulkanShader *>(computeShader)->module,
                .pName = computeShader->desc.entryPoint.c_str(),
            },
            .layout = pipelineLayout};

        VkPipeline vkPipeline;
        auto result = vk.vkCreateComputePipelines(device.device, pipelineCache.get(), 1, &pipelineInfo, nullptr, &vkPipeline);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create compute pipeline. Error: {}", toVkResultString(result));
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(vkPipeline), desc.name);

        auto *pipeline = new VulkanPipeline();
        pipeline->desc = desc;
        pipeline->pipeline = vkPipeline;
        pipeline->layout = pipelineLayout;
        pipeline->bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
        return pipeline;
    }

    DescriptorSetLayout *VulkanDevice::createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc &desc)
    {
        // Allow updating after binding, and allow empty slots in our giant arrays. Binding 1 holds the storage images,
        // when the desc asks for them.
        const VkDescriptorBindingFlags bindlessFlags[2] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
        };
        const uint32_t bindingCount = desc.storageImageCount > 0 ? 2 : 1;
        if (desc.storageImageCount > maxStorageImages)
        {
            spdlog::error("The bindless set needs {} storage images, and this device allows {} (maxDescriptorSetUpdateAfterBindStorageImages, "
                          "maxPerStageDescriptorUpdateAfterBindStorageImages). Lower MAX_BINDLESS_STORAGE_IMAGES.",
                          desc.storageImageCount, maxStorageImages);
            return nullptr;
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfo extendedInfo = {};
        extendedInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        extendedInfo.bindingCount = bindingCount;
        extendedInfo.pBindingFlags = bindlessFlags;

        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = toVkDescriptorType(desc.type);
        bindings[0].descriptorCount = desc.count;
        bindings[0].stageFlags = toVkShaderStageFlags(desc.stage);
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = desc.storageImageCount;
        bindings[1].stageFlags = toVkShaderStageFlags(desc.stage);

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = bindingCount;
        layoutInfo.pBindings = bindings;
        layoutInfo.pNext = &extendedInfo;

        auto *layout = new VulkanDescriptorSetLayout();
        layout->desc = desc;

        auto result = vk.vkCreateDescriptorSetLayout(device.device, &layoutInfo, nullptr, &layout->layout);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create bindless descriptor set layout. Error: {}", toVkResultString(result));
            delete layout;
            return nullptr;
        }
        layout->pipelineLayout = createPipelineLayout(vk, device.device, layout->layout);
        if (layout->pipelineLayout == VK_NULL_HANDLE)
        {
            vk.vkDestroyDescriptorSetLayout(device.device, layout->layout, nullptr);
            delete layout;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<uint64_t>(layout->layout), desc.name);
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<uint64_t>(layout->pipelineLayout), desc.name);
        return layout;
    }

    DescriptorPool *VulkanDevice::createDescriptorPool(const DescriptorPoolDesc &desc)
    {
        std::vector<VkDescriptorPoolSize> poolSizes;
        for (const auto &poolSize : desc.sizes)
        {
            if (poolSize.count == 0)
                continue;
            VkDescriptorPoolSize vkPoolSize = {};
            vkPoolSize.type = toVkDescriptorType(poolSize.type);
            vkPoolSize.descriptorCount = poolSize.count;
            poolSizes.push_back(vkPoolSize);
        }

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = desc.maxSets;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

        auto *pool = new VulkanDescriptorPool();
        pool->desc = desc;

        auto result = vk.vkCreateDescriptorPool(device.device, &poolInfo, nullptr, &pool->pool);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create descriptor pool. Error: {}", toVkResultString(result));
            delete pool;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, reinterpret_cast<uint64_t>(pool->pool), desc.name);
        return pool;
    }

    DescriptorSet *VulkanDevice::allocateDescriptorSet(const std::string &name, DescriptorPool *pool, DescriptorSetLayout *layout)
    {
        auto *vkLayout = static_cast<VulkanDescriptorSetLayout *>(layout);
        uint32_t variableDescCount = vkLayout->desc.count;

        VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
            .descriptorSetCount = 1,
            .pDescriptorCounts = &variableDescCount};

        VkDescriptorSetAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = &variableDescCountAI,
            .descriptorPool = static_cast<VulkanDescriptorPool *>(pool)->pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &vkLayout->layout};

        auto *descriptorSet = new VulkanDescriptorSet();
        descriptorSet->name = name;
        descriptorSet->storageImageCount = vkLayout->desc.storageImageCount;

        auto result = vk.vkAllocateDescriptorSets(device.device, &allocInfo, &descriptorSet->set);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to allocate descriptor set. Error: {}", toVkResultString(result));
            delete descriptorSet;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64_t>(descriptorSet->set), name);
        return descriptorSet;
    }

    CommandBuffer *VulkanDevice::createCommandBuffer(QueueType queueType, const std::string &name)
    {
        auto *queue = static_cast<VulkanQueue *>(getQueue(queueType));
        const QueueType onQueue = queue == computeQueue.get() ? QueueType::COMPUTE : queue == transferQueue.get() ? QueueType::TRANSFER
                                                                                                                  : QueueType::GRAPHICS;

        auto *commandBuffer = new VulkanCommandBuffer(*this);
        if (!commandBuffer->initialize(queue->getFamilyIndex(), onQueue, name))
        {
            delete commandBuffer;
            return nullptr;
        }
        return commandBuffer;
    }

    Fence *VulkanDevice::createFence(uint64_t initialValue, const std::string &name)
    {
        VkSemaphoreTypeCreateInfo typeInfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = initialValue,
        };

        VkSemaphoreCreateInfo semaphoreInfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &typeInfo};

        auto *fence = new VulkanFence();
        auto result = vk.vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &fence->semaphore);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create timeline semaphore. Error: {}", toVkResultString(result));
            delete fence;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(fence->semaphore), name);
        return fence;
    }

    Swapchain *VulkanDevice::createSwapchain(const SwapchainDesc &desc)
    {
        if (surface == VK_NULL_HANDLE)
        {
            spdlog::error("A headless device has no surface to make a swapchain for.");
            return nullptr;
        }
        auto *swapchain = new VulkanSwapchain(*this);
        if (!swapchain->initialize(desc))
        {
            delete swapchain;
            return nullptr;
        }
        return swapchain;
    }

    QueryPool *VulkanDevice::createTimestampQueryPool(uint32_t count, const std::string &name)
    {
        if (timestampMask == 0)
        {
            spdlog::warn("The graphics queue does not support timestamps.");
            return nullptr;
        }

        VkQueryPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = count};
        auto *pool = new VulkanQueryPool();
        pool->count = count;
        auto result = vk.vkCreateQueryPool(device.device, &poolInfo, nullptr, &pool->pool);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create query pool '{}'. Error: {}", name, toVkResultString(result));
            delete pool;
            return nullptr;
        }

        // A new pool's queries are undefined until reset.
        vk.vkResetQueryPool(device.device, pool->pool, 0, count);
        setObjectName(VK_OBJECT_TYPE_QUERY_POOL, reinterpret_cast<uint64_t>(pool->pool), name);
        return pool;
    }

    void VulkanDevice::destroyBuffer(Buffer *buffer)
    {
        auto *vkBuffer = static_cast<VulkanBuffer *>(buffer);
        vmaDestroyBuffer(allocator, vkBuffer->buffer, vkBuffer->allocation);
        delete vkBuffer;
    }

    void VulkanDevice::destroyTexture(Texture *texture)
    {
        auto *vkTexture = static_cast<VulkanTexture *>(texture);
        destroyImageViews(vk, device.device, *vkTexture);
        vmaDestroyImage(allocator, vkTexture->image, vkTexture->allocation);
        delete vkTexture;
    }

    void VulkanDevice::destroyHeap(Heap *heap)
    {
        auto *vkHeap = static_cast<VulkanHeap *>(heap);
        vmaFreeMemory(allocator, vkHeap->allocation);
        delete vkHeap;
    }

    void VulkanDevice::destroySampler(Sampler *sampler)
    {
        auto *vkSampler = static_cast<VulkanSampler *>(sampler);
        vk.vkDestroySampler(device.device, vkSampler->sampler, nullptr);
        delete vkSampler;
    }

    void VulkanDevice::destroyShader(Shader *shader)
    {
        auto *vkShader = static_cast<VulkanShader *>(shader);
        vk.vkDestroyShaderModule(device.device, vkShader->module, nullptr);
        delete vkShader;
    }

    void VulkanDevice::destroyPipeline(Pipeline *pipeline)
    {
        auto *vkPipeline = static_cast<VulkanPipeline *>(pipeline);
        vk.vkDestroyPipeline(device.device, vkPipeline->pipeline, nullptr);
        delete vkPipeline;
    }

    void VulkanDevice::destroyDescriptorSetLayout(DescriptorSetLayout *layout)
    {
        auto *vkLayout = static_cast<VulkanDescriptorSetLayout *>(layout);
        vk.vkDestroyPipelineLayout(device.device, vkLayout->pipelineLayout, nullptr);
        vk.vkDestroyDescriptorSetLayout(device.device, vkLayout->layout, nullptr);
        delete vkLayout;
    }

    void VulkanDevice::destroyDescriptorPool(DescriptorPool *pool)
    {
        auto *vkPool = static_cast<VulkanDescriptorPool *>(pool);
        vk.vkDestroyDescriptorPool(device.device, vkPool->pool, nullptr);
        delete vkPool;
    }

    void VulkanDevice::destroyDescriptorSet(DescriptorSet *descriptorSet)
    {
        // The pool has no FREE_DESCRIPTOR_SET flag, so the set itself is freed with its pool.
        delete static_cast<VulkanDescriptorSet *>(descriptorSet);
    }

    void VulkanDevice::destroyCommandBuffer(CommandBuffer *commandBuffer)
    {
        delete static_cast<VulkanCommandBuffer *>(commandBuffer);
    }

    void VulkanDevice::destroyFence(Fence *fence)
    {
        auto *vkFence = static_cast<VulkanFence *>(fence);
        vk.vkDestroySemaphore(device.device, vkFence->semaphore, nullptr);
        delete vkFence;
    }

    void VulkanDevice::destroySwapchain(Swapchain *swapchain)
    {
        delete static_cast<VulkanSwapchain *>(swapchain);
    }

    void VulkanDevice::destroyQueryPool(QueryPool *pool)
    {
        auto *vkPool = static_cast<VulkanQueryPool *>(pool);
        vk.vkDestroyQueryPool(device.device, vkPool->pool, nullptr);
        delete vkPool;
    }

    Queue *VulkanDevice::getQueue(QueueType queueType)
    {
        if (queueType == QueueType::COMPUTE && computeQueue != nullptr)
            return computeQueue.get();
        if (queueType == QueueType::TRANSFER && transferQueue != nullptr)
            return transferQueue.get();
        return graphicsQueue.get();
    }

    bool VulkanDevice::hasAsyncCompute() const
    {
        return computeQueue != nullptr;
    }

    bool VulkanDevice::hasTransferQueue() const
    {
        return transferQueue != nullptr;
    }

    // The compute queue is taken only with the graphics queue's valid bits.
    bool VulkanDevice::hasTimestamps(QueueType queueType) const
    {
        if (queueType == QueueType::TRANSFER && transferQueue != nullptr)
            return transferTimestamps;
        return timestampMask != 0;
    }

    uint32_t VulkanDevice::getTransferFamily() const
    {
        return transferQueue ? transferQueue->getFamilyIndex() : graphicsQueue->getFamilyIndex();
    }

    bool VulkanDevice::hasMutableSwapchainFormat() const
    {
        return swapchainMutable;
    }

    bool VulkanDevice::fillBuffer(Buffer *buffer, const void *data, size_t size, size_t offset)
    {
        auto *vkBuffer = static_cast<VulkanBuffer *>(buffer);
        assert(offset + size <= vkBuffer->desc.size);
        assert(vkBuffer->mappedData != nullptr);

        std::memcpy(static_cast<uint8_t *>(vkBuffer->mappedData) + offset, data, size);
        // VMA may choose non-coherent host-visible memory; coherent allocations make this a no-op.
        auto result = vmaFlushAllocation(allocator, vkBuffer->allocation, offset, size);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to flush buffer memory: {}", toVkResultString(result));
            return false;
        }
        return true;
    }

    // The buffer was created with hostReadable. The GPU writes must be made available to the host first: a
    // barrier to PipelineStage::HOST before the fence that the caller waited for.
    bool VulkanDevice::readBuffer(Buffer *buffer, void *data, size_t size, size_t offset)
    {
        auto *vkBuffer = static_cast<VulkanBuffer *>(buffer);
        assert(offset + size <= vkBuffer->desc.size);
        assert(vkBuffer->mappedData != nullptr);

        auto result = vmaInvalidateAllocation(allocator, vkBuffer->allocation, offset, size);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to invalidate buffer memory: {}", toVkResultString(result));
            return false;
        }
        std::memcpy(data, static_cast<const uint8_t *>(vkBuffer->mappedData) + offset, size);
        return true;
    }

    // The format table is read once: the features of a format never change while the device lives. A block format also
    // needs its compression feature, which initialize enabled where the GPU has it. A format is copied on the host only
    // where that costs the GPU nothing when it samples the image (P2): the driver says the image keeps its optimal
    // layout, and it can live in video memory (getHostCopyMemoryTypes). Otherwise it is filled from the staging ring.
    void VulkanDevice::queryFormatSupport(bool hostImageCopy)
    {
        // The copy writes into SHADER_READ_ONLY_OPTIMAL, which the device must list.
        if (hostImageCopy)
        {
            VkPhysicalDeviceHostImageCopyProperties copyProperties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES};
            VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &copyProperties};
            vkGetPhysicalDeviceProperties2(physicalDevice.physical_device, &properties2);
            std::vector<VkImageLayout> layouts(copyProperties.copyDstLayoutCount);
            copyProperties.pCopyDstLayouts = layouts.data();
            vkGetPhysicalDeviceProperties2(physicalDevice.physical_device, &properties2);
            hostImageCopy = std::find(layouts.begin(), layouts.end(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) != layouts.end();
        }
        VkPhysicalDeviceMemoryProperties memory = {};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice.physical_device, &memory);

        uint32_t hostCopyFormats = 0;
        const VkPhysicalDeviceFeatures &enabled = physicalDevice.features;
        for (uint32_t value = 0; value < FORMAT_COUNT; ++value)
        {
            const auto format = static_cast<Format>(value);
            const VkFormat vkFormat = toVkFormat(format);
            if (vkFormat == VK_FORMAT_UNDEFINED)
                continue;
            switch (format)
            {
            case Format::BC4_UNORM:
            case Format::BC5_UNORM:
            case Format::BC7_UNORM:
            case Format::BC7_SRGB:
                if (!enabled.textureCompressionBC)
                    continue;
                break;
            case Format::ASTC_4x4_UNORM:
            case Format::ASTC_4x4_SRGB:
                if (!enabled.textureCompressionASTC_LDR)
                    continue;
                break;
            case Format::ETC2_R8G8B8A8_UNORM:
            case Format::ETC2_R8G8B8A8_SRGB:
            case Format::EAC_R11_UNORM:
            case Format::EAC_R11G11_UNORM:
                if (!enabled.textureCompressionETC2)
                    continue;
                break;
            default:
                break;
            }

            VkFormatProperties3 properties3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
            VkFormatProperties2 properties = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &properties3};
            vkGetPhysicalDeviceFormatProperties2(physicalDevice.physical_device, vkFormat, &properties);
            const VkFormatFeatureFlags2 features = properties3.optimalTilingFeatures;
            const bool sampled = (features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) != 0 && (features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0 &&
                                 (features & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT) != 0;
            uint32_t hostCopyTypes = 0;
            if (sampled && hostImageCopy && (features & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT) != 0)
            {
                VkHostImageCopyDevicePerformanceQuery performance = {.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY};
                VkImageFormatProperties2 imageProperties = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &performance};
                const VkPhysicalDeviceImageFormatInfo2 imageInfo = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                                                    .format = vkFormat,
                                                                    .type = VK_IMAGE_TYPE_2D,
                                                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT};
                if (vkGetPhysicalDeviceImageFormatProperties2(physicalDevice.physical_device, &imageInfo, &imageProperties) == VK_SUCCESS && performance.optimalDeviceAccess)
                {
                    const TextureDesc desc = {.width = 256, .height = 256, .format = format, .usage = TextureUsage::SAMPLED};
                    hostCopyTypes = getHostCopyMemoryTypes(memory, getImageMemoryTypes(desc, true), getImageMemoryTypes(desc, false));
                }
            }
            formatSupport[value] = {.sampled = sampled, .hostCopyTypes = hostCopyTypes};
            hostCopyFormats += hostCopyTypes != 0 ? 1 : 0;
        }
        spdlog::info("Host image copy: {}.", hostCopyFormats > 0 ? std::to_string(hostCopyFormats) + " formats, in video memory" : "off, textures come from the staging ring");
    }

    uint32_t VulkanDevice::getImageMemoryTypes(const TextureDesc &desc, bool hostCopy)
    {
        uint32_t familyCount = 0;
        const uint32_t *families = getImageFamilies(desc, hostCopy, familyCount);
        const VkImageCreateInfo imageInfo = imageCreateInfo(desc, 1, hostCopy, families, familyCount);
        const VkDeviceImageMemoryRequirements info = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &imageInfo};
        VkMemoryRequirements2 requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vk.vkGetDeviceImageMemoryRequirements(device.device, &info, &requirements);
        return requirements.memoryRequirements.memoryTypeBits;
    }

    // A sampled texture filled from the staging ring may be copied on the transfer queue; every other image is used by
    // the frames' queues only.
    const uint32_t *VulkanDevice::getImageFamilies(const TextureDesc &desc, bool hostCopy, uint32_t &count) const
    {
        if (desc.usage == TextureUsage::SAMPLED && !hostCopy)
        {
            count = uploadFamilyCount;
            return uploadFamilies;
        }
        count = sharedFamilyCount;
        return sharedFamilies;
    }

    bool VulkanDevice::usesHostCopy(const TextureDesc &desc) const
    {
        const auto value = static_cast<uint32_t>(desc.format);
        return desc.usage == TextureUsage::SAMPLED && value < FORMAT_COUNT && formatSupport[value].hostCopyTypes != 0;
    }

    bool VulkanDevice::supportsTextureFormat(Format format) const
    {
        const auto value = static_cast<uint32_t>(format);
        return value < FORMAT_COUNT && formatSupport[value].sampled;
    }

    // VkSampleCountFlagBits are the counts themselves (VK_SAMPLE_COUNT_4_BIT is 4), so the mask is already 2^n per bit n.
    uint32_t VulkanDevice::supportedSampleCounts() const
    {
        return sampleCounts;
    }

    // Copies every mip on the host (VK_EXT_host_image_copy, core in 1.4), with no command buffer: the image goes from
    // UNDEFINED to SHADER_READ_ONLY on the CPU first. Only for a texture created with hostCopy; the others go through
    // the staging ring (RenderContext::fillTexture).
    bool VulkanDevice::fillTexture(Texture *texture, const TextureLevel *levels, uint32_t levelCount)
    {
        auto *vkTexture = static_cast<VulkanTexture *>(texture);
        assert(texture->desc.usage == TextureUsage::SAMPLED && texture->hostCopy && levelCount == texture->mipLevels);

        VkHostImageLayoutTransitionInfo transitionInfo = {};
        transitionInfo.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO;
        transitionInfo.image = vkTexture->image;
        transitionInfo.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        transitionInfo.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transitionInfo.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                           .baseMipLevel = 0,
                                           .levelCount = vkTexture->mipLevels,
                                           .baseArrayLayer = 0,
                                           .layerCount = 1};

        auto result = vk.vkTransitionImageLayout(device.device, 1, &transitionInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to transition image layout. Error: {}", toVkResultString(result));
            return false;
        }

        // Each region's extent is the mip's size in texels; a block format's rows of blocks are read tightly packed.
        std::vector<VkMemoryToImageCopy> regions(levelCount);
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            auto &region = regions[level];
            region.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY;
            region.pHostPointer = levels[level].data;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            region.imageExtent = {std::max(1u, texture->desc.width >> level), std::max(1u, texture->desc.height >> level), 1};
        }

        VkCopyMemoryToImageInfo copyInfo = {};
        copyInfo.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO;
        copyInfo.dstImage = vkTexture->image;
        copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        copyInfo.regionCount = levelCount;
        copyInfo.pRegions = regions.data();

        result = vk.vkCopyMemoryToImage(device.device, &copyInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to copy memory to image. Error: {}", toVkResultString(result));
            return false;
        }
        return true;
    }

    void VulkanDevice::updateBindlessTexture(DescriptorSet *descriptorSet, uint32_t slot, Texture *texture, Sampler *sampler)
    {
        VkDescriptorImageInfo imageInfo = {
            .sampler = static_cast<VulkanSampler *>(sampler)->sampler,
            .imageView = static_cast<VulkanTexture *>(texture)->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        VkWriteDescriptorSet descriptorWrite = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = static_cast<VulkanDescriptorSet *>(descriptorSet)->set,
            .dstBinding = 0,
            .dstArrayElement = slot,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfo,
        };

        vk.vkUpdateDescriptorSets(device.device, 1, &descriptorWrite, 0, nullptr);
    }

    // Storage images are written in GENERAL, the layout the STORAGE state transitions to.
    void VulkanDevice::updateBindlessStorageImage(DescriptorSet *descriptorSet, uint32_t slot, Texture *texture, uint32_t mip)
    {
        auto *vkTexture = static_cast<VulkanTexture *>(texture);
        assert(texture->desc.usage == TextureUsage::SAMPLED_STORAGE && mip < texture->mipLevels);
        VkDescriptorImageInfo imageInfo = {
            .imageView = vkTexture->mipViews[mip],
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };

        VkWriteDescriptorSet descriptorWrite = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = static_cast<VulkanDescriptorSet *>(descriptorSet)->set,
            .dstBinding = 1,
            .dstArrayElement = slot,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &imageInfo,
        };

        vk.vkUpdateDescriptorSets(device.device, 1, &descriptorWrite, 0, nullptr);
    }

    bool VulkanDevice::waitForFence(Fence *fence, uint64_t value, uint64_t timeout)
    {
        VkSemaphoreWaitInfo waitInfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &static_cast<VulkanFence *>(fence)->semaphore,
            .pValues = &value,
        };

        auto result = vk.vkWaitSemaphores(device.device, &waitInfo, timeout);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to wait for fence. Error: {}", toVkResultString(result));
            return false;
        }
        return true;
    }

    uint64_t VulkanDevice::getFenceValue(Fence *fence)
    {
        uint64_t value = 0;
        auto result = vk.vkGetSemaphoreCounterValue(device.device, static_cast<VulkanFence *>(fence)->semaphore, &value);
        if (result != VK_SUCCESS)
            spdlog::error("Failed to read fence value. Error: {}", toVkResultString(result));
        return value;
    }

    // The GPU must be done with the pool: today that means a fence wait covered its last submit.
    void VulkanDevice::resetQueries(QueryPool *pool)
    {
        vk.vkResetQueryPool(device.device, static_cast<VulkanQueryPool *>(pool)->pool, 0, pool->count);
    }

    // Nanoseconds since the first of the queries; one before it reads 0. False when any of them has not been
    // written since the last reset.
    bool VulkanDevice::getTimestamps(QueryPool *pool, uint32_t firstQuery, uint32_t count, uint64_t *nanoseconds)
    {
        assert(firstQuery + count <= pool->count);
        if (count == 0)
            return true;

        auto result = vk.vkGetQueryPoolResults(device.device, static_cast<VulkanQueryPool *>(pool)->pool, firstQuery, count, count * sizeof(uint64_t),
                                               nanoseconds, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (result != VK_SUCCESS)
        {
            if (result != VK_NOT_READY)
                spdlog::error("Failed to read timestamps. Error: {}", toVkResultString(result));
            return false;
        }

        // The counter wraps after timestampValidBits, so each value is turned into its distance from the first one,
        // modulo the valid bits, and one in the upper half of that range counts as before the first. That holds while
        // the queries span less than half the counter's range. Queries written on another queue may count from
        // another origin, so their values can be far off; they are clamped, never wrapped.
        constexpr double MAX_NANOSECONDS = 9.0e18;
        const uint64_t first = nanoseconds[0];
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint64_t ticks = (nanoseconds[i] - first) & timestampMask;
            const bool before = ticks > timestampMask >> 1;
            nanoseconds[i] = before ? 0 : static_cast<uint64_t>(std::min(static_cast<double>(ticks) * timestampPeriod, MAX_NANOSECONDS));
        }
        return true;
    }

    VRAMStats VulkanDevice::queryVRAMStats()
    {
        VRAMStats vramStats{};

        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProperties{};
        budgetProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

        VkPhysicalDeviceMemoryProperties2 memoryProperties2{};
        memoryProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        memoryProperties2.pNext = &budgetProperties;

        vkGetPhysicalDeviceMemoryProperties2(physicalDevice.physical_device, &memoryProperties2);
        // The usage counts whole memory blocks, which VMA fills with many resources; the allocated bytes are theirs.
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(allocator, budgets);

        for (uint32_t i = 0; i < memoryProperties2.memoryProperties.memoryHeapCount; ++i)
        {
            const VkMemoryHeap &heap = memoryProperties2.memoryProperties.memoryHeaps[i];
            if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            {
                vramStats.vramBudget += budgetProperties.heapBudget[i];
                vramStats.vramUsage += budgetProperties.heapUsage[i];
                vramStats.vramAllocated += budgets[i].statistics.allocationBytes;
            }
            else
            {
                vramStats.hostBudget += budgetProperties.heapBudget[i];
                vramStats.hostUsage += budgetProperties.heapUsage[i];
            }
        }

        return vramStats;
    }

    void VulkanDevice::waitIdle()
    {
        if (device.device != VK_NULL_HANDLE)
            vk.vkDeviceWaitIdle(device.device);
    }

    VkDevice VulkanDevice::getVkDevice() const
    {
        return device.device;
    }

    const VolkDeviceTable &VulkanDevice::getVkTable() const
    {
        return vk;
    }

    const vkb::Device &VulkanDevice::getVkbDevice() const
    {
        return device;
    }

    VkPhysicalDevice VulkanDevice::getVkPhysicalDevice() const
    {
        return physicalDevice.physical_device;
    }

    VkSurfaceKHR VulkanDevice::getSurface() const
    {
        return surface;
    }

    VulkanQueue *VulkanDevice::getGraphicsQueue() const
    {
        return graphicsQueue.get();
    }

    void VulkanDevice::setObjectName(VkObjectType type, uint64_t handle, const std::string &name)
    {
        // Skip naming when the extension is not enabled; the function pointer is null or invalid then (B3)
        if (!debugUtilsEnabled || !vkSetDebugUtilsObjectNameEXT || name.empty())
            return;

        VkDebugUtilsObjectNameInfoEXT nameInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        nameInfo.objectType = type;
        nameInfo.objectHandle = handle;
        nameInfo.pObjectName = name.c_str();

        vkSetDebugUtilsObjectNameEXT(device.device, &nameInfo);
    }
}
