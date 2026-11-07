#include "rhi/vulkan/vulkanDevice.h"
#include "rhi/vulkan/vulkanCommandBuffer.h"
#include "rhi/vulkan/vulkanSwapchain.h"
#include "rhi/vulkan/vulkanUtils.h"
#include "platform/windowSurface.h"

#include "spdlog/spdlog.h"
#include "stb_image_resize2.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include <vector>

namespace rasm::rhi
{
    namespace
    {
        vkb::Result<vkb::Instance> initInstance(const DeviceDesc &desc, bool &debugUtilsEnabled)
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

            // Debug utils is cheap, so enable it whenever it is available: object names show up in
            // RenderDoc / Nsight even without validation (B3).
            if (systemInfo.debug_utils_available)
            {
                instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                debugUtilsEnabled = true;
            }

            if (desc.enableValidation && systemInfo.validation_layers_available)
            {
                instanceBuilder.enable_validation_layers();

                // Validation needs to send errors via a callback, have vk-bootstrap do it
                if (debugUtilsEnabled)
                    instanceBuilder.use_default_debug_messenger();
            }

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
                .synchronization2 = VK_TRUE,
                .dynamicRendering = VK_TRUE,
            };

            VkPhysicalDeviceVulkan14Features enabledVk14Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                .pNext = &enabledVk13Features,
                .hostImageCopy = VK_TRUE};

            VkPhysicalDeviceFeatures enabledVk10Features{
                .multiDrawIndirect = VK_TRUE,
                .drawIndirectFirstInstance = VK_TRUE,
                .samplerAnisotropy = VK_TRUE,
                .shaderInt64 = VK_TRUE,
            };

            // select() grabs a PhysicalDevice, By default, this will prefer a discrete GPU.
            auto physicalDeviceSelectorReturn = physDeviceSelector
                                                    .set_surface(surface)
                                                    .set_required_features_14(enabledVk14Features)
                                                    .set_required_features_13(enabledVk13Features)
                                                    .set_required_features_12(enabledVk12Features)
                                                    .set_required_features_11(enabledVk11Features)
                                                    .set_required_features(enabledVk10Features)
                                                    .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                                                    .add_required_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)
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

            const auto &physicalDevice = physicalDeviceSelectorReturn.value();
            spdlog::info(
                "Selected GPU information:\n"
                "\t\t\t\t\tname: {} -- type: {} --  memory: {} MB\n"
                "\t\t\t\t\tdriver version: {}.{}.{} --  API version: {}.{}.{}",
                physicalDevice.properties.deviceName,
                toDeviceTypeString(physicalDevice.properties.deviceType),
                physicalDevice.memory_properties.memoryHeaps[0].size / (1024 * 1024),
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
            // Every buffer is created with the device-address bit so that its GPU address can be
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
                return flags;
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

        // The bindless set and a PUSH_CONSTANTS_SIZE range visible to the pipeline's stages.
        VkPipelineLayout createPipelineLayout(VkDevice device, VkShaderStageFlags stages, DescriptorSetLayout *layout)
        {
            VkPushConstantRange pushConstantRange{
                .stageFlags = stages,
                .size = PUSH_CONSTANTS_SIZE};

            VkDescriptorSetLayout setLayouts[] = {static_cast<VulkanDescriptorSetLayout *>(layout)->layout};

            VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = setLayouts,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &pushConstantRange};

            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            auto result = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
            if (result != VK_SUCCESS)
            {
                spdlog::error("Failed to create pipeline layout. Error: {}", toVkResultString(result));
                return VK_NULL_HANDLE;
            }
            return pipelineLayout;
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

        if (allocator != nullptr)
            vmaDestroyAllocator(allocator);
        graphicsQueue.reset();
        if (device.device != VK_NULL_HANDLE)
            vkb::destroy_device(device);
        if (surface != VK_NULL_HANDLE)
            vkb::destroy_surface(instance, surface);
        if (instance.instance != VK_NULL_HANDLE)
            vkb::destroy_instance(instance);
    }

    bool VulkanDevice::initialize(const DeviceDesc &desc)
    {
        if (desc.window == nullptr)
        {
            spdlog::error("The Vulkan device needs a window to present to.");
            return false;
        }

        auto vkbInstance = initInstance(desc, debugUtilsEnabled);
        if (!vkbInstance)
            return false;
        instance = vkbInstance.value();

        if (!createWindowSurface(*desc.window, instance.instance, &surface))
            return false;

        auto vkbPhysicalDevice = initPhysicalDevice(instance, surface);
        if (!vkbPhysicalDevice)
            return false;
        physicalDevice = vkbPhysicalDevice.value();

        auto vkbDevice = vkb::DeviceBuilder{physicalDevice}.build();
        if (!vkbDevice)
        {
            spdlog::error("Failed to create logical device. Error: {}", vkbDevice.error().message());
            return false;
        }
        device = vkbDevice.value();
        dispatchTable = device.make_table();

        auto queueRet = device.get_queue(vkb::QueueType::graphics);
        auto queueIndexRet = device.get_queue_index(vkb::QueueType::graphics);
        if (!queueRet || !queueIndexRet)
        {
            spdlog::error("Failed to get the graphics queue.");
            return false;
        }
        graphicsQueue = std::make_unique<VulkanQueue>(queueRet.value(), queueIndexRet.value());

        const uint32_t timestampBits = device.queue_families[queueIndexRet.value()].timestampValidBits;
        timestampMask = timestampBits >= 64 ? ~0ull : (1ull << timestampBits) - 1;
        timestampPeriod = physicalDevice.properties.limits.timestampPeriod;

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

        if (vmaCreateAllocator(&allocatorCreateInfo, &allocator) != VK_SUCCESS)
        {
            spdlog::error("Failed to create VMA allocator.");
            return false;
        }

        return true;
    }

    Buffer *VulkanDevice::createBuffer(const BufferDesc &desc)
    {
        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.size;
        bufferInfo.usage = toVkBufferUsageFlags(desc.usage);

        // P1: buffers that aren't hostVisible prefer device-local memory, and are mapped only when that memory is
        // also host-visible (ReBAR, integrated GPUs). Otherwise pMappedData stays null and they are filled by staging.
        VmaAllocationCreateInfo allocCI = {};
        allocCI.usage = VMA_MEMORY_USAGE_AUTO;
        allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (!desc.hostVisible)
            allocCI.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;

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
        buffer->deviceAddress = vkGetBufferDeviceAddress(device.device, &bufferAddressInfo);

        setObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer->buffer), desc.name);
        return buffer;
    }

    Texture *VulkanDevice::createTexture(const TextureDesc &desc)
    {
        // Drivers may accept an oversized image instead of failing, so this can't be left to vmaCreateImage.
        const uint32_t maxExtent = physicalDevice.properties.limits.maxImageDimension2D;
        if (desc.width > maxExtent || desc.height > maxExtent)
        {
            spdlog::error("Texture '{}' is {}x{}, the device limit is {}.", desc.name, desc.width, desc.height, maxExtent);
            return nullptr;
        }

        // Only sampled RGBA8 images use this upload/generation path. Render targets and
        // other formats retain a single level.
        uint32_t mipLevels = 1;
        if (desc.usage == TextureUsage::SAMPLED &&
            (desc.format == Format::R8G8B8A8_UNORM || desc.format == Format::R8G8B8A8_SRGB ||
             desc.format == Format::B8G8R8A8_SRGB))
        {
            for (uint32_t size = std::max(desc.width, desc.height); size > 1; size /= 2)
                ++mipLevels;
        }

        // TODO: check if the format is supported by the device, and if not, find a compatible one
        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = desc.width;
        imageInfo.extent.height = desc.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = toVkFormat(desc.format);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = toVkImageUsageFlags(desc.usage);
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        if (desc.usage == TextureUsage::SAMPLED)
        {
            allocInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
        }

        auto *texture = new VulkanTexture();
        texture->desc = desc;
        texture->mipLevels = mipLevels;

        auto result = vmaCreateImage(allocator, &imageInfo, &allocInfo, &texture->image, &texture->allocation, nullptr);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create image '{}'. Error: {}", desc.name, toVkResultString(result));
            delete texture;
            return nullptr;
        }

        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = texture->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange.aspectMask = toVkAspectMask(desc.format);
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.layerCount = 1;

        result = vkCreateImageView(device.device, &viewInfo, nullptr, &texture->view);
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

        auto result = vkCreateSampler(device.device, &samplerInfo, nullptr, &sampler->sampler);
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

        auto result = vkCreateShaderModule(device.device, &shaderModuleInfo, nullptr, &shader->module);
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
        VkPipelineLayout pipelineLayout = createPipelineLayout(device.device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, layout);
        if (pipelineLayout == VK_NULL_HANDLE)
            return nullptr;

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
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL, // reversed-Z: nearer is larger; pairs with the 0 clear in VulkanCommandBuffer::beginRendering
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE};

        // Dynamic rendering: these formats must match the attachments passed to beginRendering.
        VkFormat colorAttachmentFormat = toVkFormat(desc.colorAttachmentFormat);
        VkFormat depthAttachmentFormat = toVkFormat(desc.depthStencilAttachmentFormat);

        // Combined depth/stencil formats must declare the stencil aspect too (Metal validation asserts otherwise).
        VkPipelineRenderingCreateInfo pipelineRenderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorAttachmentFormat,
            .depthAttachmentFormat = depthAttachmentFormat,
            .stencilAttachmentFormat = hasStencil(desc.depthStencilAttachmentFormat) ? depthAttachmentFormat : VK_FORMAT_UNDEFINED,
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

        VkPipeline vkPipeline;
        auto result = vkCreateGraphicsPipelines(device.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &vkPipeline);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create graphics pipeline. Error: {}", toVkResultString(result));
            vkDestroyPipelineLayout(device.device, pipelineLayout, nullptr);
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
        VkPipelineLayout pipelineLayout = createPipelineLayout(device.device, VK_SHADER_STAGE_COMPUTE_BIT, layout);
        if (pipelineLayout == VK_NULL_HANDLE)
            return nullptr;

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
        auto result = vkCreateComputePipelines(device.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &vkPipeline);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create compute pipeline. Error: {}", toVkResultString(result));
            vkDestroyPipelineLayout(device.device, pipelineLayout, nullptr);
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
        samplerLayoutBinding.descriptorType = toVkDescriptorType(desc.type);
        samplerLayoutBinding.descriptorCount = desc.count;
        samplerLayoutBinding.stageFlags = toVkShaderStageFlags(desc.stage);

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &samplerLayoutBinding;
        layoutInfo.pNext = &extendedInfo;

        auto *layout = new VulkanDescriptorSetLayout();
        layout->desc = desc;

        auto result = vkCreateDescriptorSetLayout(device.device, &layoutInfo, nullptr, &layout->layout);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create bindless descriptor set layout. Error: {}", toVkResultString(result));
            delete layout;
            return nullptr;
        }

        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<uint64_t>(layout->layout), desc.name);
        return layout;
    }

    DescriptorPool *VulkanDevice::createDescriptorPool(const DescriptorPoolDesc &desc)
    {
        std::vector<VkDescriptorPoolSize> poolSizes;
        for (const auto &poolSize : desc.sizes)
        {
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

        auto result = vkCreateDescriptorPool(device.device, &poolInfo, nullptr, &pool->pool);
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

        auto result = vkAllocateDescriptorSets(device.device, &allocInfo, &descriptorSet->set);
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
        assert(queueType == QueueType::GRAPHICS);

        auto *commandBuffer = new VulkanCommandBuffer(*this);
        if (!commandBuffer->initialize(graphicsQueue->getFamilyIndex(), name))
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
        auto result = vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &fence->semaphore);
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
        auto result = vkCreateQueryPool(device.device, &poolInfo, nullptr, &pool->pool);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create query pool '{}'. Error: {}", name, toVkResultString(result));
            delete pool;
            return nullptr;
        }

        // A new pool's queries are undefined until reset.
        vkResetQueryPool(device.device, pool->pool, 0, count);
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
        vkDestroyImageView(device.device, vkTexture->view, nullptr);
        vmaDestroyImage(allocator, vkTexture->image, vkTexture->allocation);
        delete vkTexture;
    }

    void VulkanDevice::destroySampler(Sampler *sampler)
    {
        auto *vkSampler = static_cast<VulkanSampler *>(sampler);
        vkDestroySampler(device.device, vkSampler->sampler, nullptr);
        delete vkSampler;
    }

    void VulkanDevice::destroyShader(Shader *shader)
    {
        auto *vkShader = static_cast<VulkanShader *>(shader);
        vkDestroyShaderModule(device.device, vkShader->module, nullptr);
        delete vkShader;
    }

    void VulkanDevice::destroyPipeline(Pipeline *pipeline)
    {
        auto *vkPipeline = static_cast<VulkanPipeline *>(pipeline);
        vkDestroyPipeline(device.device, vkPipeline->pipeline, nullptr);
        vkDestroyPipelineLayout(device.device, vkPipeline->layout, nullptr);
        delete vkPipeline;
    }

    void VulkanDevice::destroyDescriptorSetLayout(DescriptorSetLayout *layout)
    {
        auto *vkLayout = static_cast<VulkanDescriptorSetLayout *>(layout);
        vkDestroyDescriptorSetLayout(device.device, vkLayout->layout, nullptr);
        delete vkLayout;
    }

    void VulkanDevice::destroyDescriptorPool(DescriptorPool *pool)
    {
        auto *vkPool = static_cast<VulkanDescriptorPool *>(pool);
        vkDestroyDescriptorPool(device.device, vkPool->pool, nullptr);
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
        vkDestroySemaphore(device.device, vkFence->semaphore, nullptr);
        delete vkFence;
    }

    void VulkanDevice::destroySwapchain(Swapchain *swapchain)
    {
        delete static_cast<VulkanSwapchain *>(swapchain);
    }

    void VulkanDevice::destroyQueryPool(QueryPool *pool)
    {
        auto *vkPool = static_cast<VulkanQueryPool *>(pool);
        vkDestroyQueryPool(device.device, vkPool->pool, nullptr);
        delete vkPool;
    }

    Queue *VulkanDevice::getQueue(QueueType queueType)
    {
        assert(queueType == QueueType::GRAPHICS);
        return graphicsQueue.get();
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

    // The GPU writes must be made available to the host first: a barrier to PipelineStage::HOST before the
    // fence that the caller waited for.
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

    bool VulkanDevice::fillTexture(Texture *texture, const void *data)
    {
        auto *vkTexture = static_cast<VulkanTexture *>(texture);
        assert(texture->desc.usage == TextureUsage::SAMPLED);

        // Transition from UNDEFINED to SHADER_READ directly on the CPU (VK_EXT_host_image_copy).
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

        auto result = vkTransitionImageLayout(device.device, 1, &transitionInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to transition image layout. Error: {}", toVkResultString(result));
            return false;
        }

        // Generate the complete RGBA8 mip chain on the host, matching this backend's
        // host-image-copy upload path. Color must be filtered in linear light; data maps
        // (normal/MR/AO) must retain their linear encoding. No GPU blit support is required.
        std::vector<std::vector<unsigned char>> mipData(vkTexture->mipLevels - 1);
        std::vector<VkMemoryToImageCopy> regions(vkTexture->mipLevels);
        const auto *pixels = static_cast<const unsigned char *>(data);
        uint32_t width = texture->desc.width;
        uint32_t height = texture->desc.height;
        for (uint32_t level = 0; level < vkTexture->mipLevels; ++level)
        {
            auto &region = regions[level];
            region.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY;
            region.pHostPointer = pixels;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            region.imageExtent = {width, height, 1};
            if (level + 1 < vkTexture->mipLevels)
            {
                auto nextWidth = std::max(1u, width / 2);
                auto nextHeight = std::max(1u, height / 2);
                auto &next = mipData[level];
                next.resize(static_cast<size_t>(nextWidth) * nextHeight * 4);
                unsigned char *resized = nullptr;
                if (texture->desc.format == Format::R8G8B8A8_SRGB ||
                    texture->desc.format == Format::B8G8R8A8_SRGB)
                    resized = stbir_resize_uint8_srgb(pixels, width, height, 0, next.data(), nextWidth, nextHeight, 0, STBIR_RGBA);
                else
                    resized = stbir_resize_uint8_linear(pixels, width, height, 0, next.data(), nextWidth, nextHeight, 0, STBIR_4CHANNEL);
                if (!resized)
                {
                    spdlog::error("Failed to generate mip level {} for '{}'", level + 1, texture->desc.name);
                    return false;
                }
                pixels = next.data();
                width = nextWidth;
                height = nextHeight;
            }
        }

        VkCopyMemoryToImageInfo copyInfo = {};
        copyInfo.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO;
        copyInfo.dstImage = vkTexture->image;
        copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        copyInfo.regionCount = static_cast<uint32_t>(regions.size());
        copyInfo.pRegions = regions.data();

        result = vkCopyMemoryToImage(device.device, &copyInfo);
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

        vkUpdateDescriptorSets(device.device, 1, &descriptorWrite, 0, nullptr);
    }

    bool VulkanDevice::waitForFence(Fence *fence, uint64_t value, uint64_t timeout)
    {
        VkSemaphoreWaitInfo waitInfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &static_cast<VulkanFence *>(fence)->semaphore,
            .pValues = &value,
        };

        auto result = vkWaitSemaphores(device.device, &waitInfo, timeout);
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
        auto result = vkGetSemaphoreCounterValue(device.device, static_cast<VulkanFence *>(fence)->semaphore, &value);
        if (result != VK_SUCCESS)
            spdlog::error("Failed to read fence value. Error: {}", toVkResultString(result));
        return value;
    }

    // The GPU must be done with the pool: today that means a fence wait covered its last submit.
    void VulkanDevice::resetQueries(QueryPool *pool)
    {
        vkResetQueryPool(device.device, static_cast<VulkanQueryPool *>(pool)->pool, 0, pool->count);
    }

    // Nanoseconds since the earliest of the queries. False when any of them has not been written since the last reset.
    bool VulkanDevice::getTimestamps(QueryPool *pool, uint32_t count, uint64_t *nanoseconds)
    {
        assert(count <= pool->count);
        if (count == 0)
            return true;

        auto result = vkGetQueryPoolResults(device.device, static_cast<VulkanQueryPool *>(pool)->pool, 0, count, count * sizeof(uint64_t),
                                            nanoseconds, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (result != VK_SUCCESS)
        {
            if (result != VK_NOT_READY)
                spdlog::error("Failed to read timestamps. Error: {}", toVkResultString(result));
            return false;
        }

        uint64_t earliest = UINT64_MAX;
        for (uint32_t i = 0; i < count; ++i)
            earliest = std::min(earliest, nanoseconds[i] & timestampMask);
        for (uint32_t i = 0; i < count; ++i)
            nanoseconds[i] = static_cast<uint64_t>(static_cast<double>((nanoseconds[i] & timestampMask) - earliest) * timestampPeriod);
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

        for (uint32_t i = 0; i < memoryProperties2.memoryProperties.memoryHeapCount; ++i)
        {
            const VkMemoryHeap &heap = memoryProperties2.memoryProperties.memoryHeaps[i];
            if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            {
                vramStats.vramBudget += budgetProperties.heapBudget[i];
                vramStats.vramUsage += budgetProperties.heapUsage[i];
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
            vkDeviceWaitIdle(device.device);
    }

    VkDevice VulkanDevice::getVkDevice() const
    {
        return device.device;
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
        if (!debugUtilsEnabled || !dispatchTable.fp_vkSetDebugUtilsObjectNameEXT || name.empty())
            return;

        VkDebugUtilsObjectNameInfoEXT nameInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        nameInfo.objectType = type;
        nameInfo.objectHandle = handle;
        nameInfo.pObjectName = name.c_str();

        dispatchTable.setDebugUtilsObjectNameEXT(&nameInfo);
    }
}
