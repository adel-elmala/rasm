#include <cassert>

#include "rasm/core/engine.h"
#include "rasm/gfx/vulkan.h"

#include "spdlog/spdlog.h"

#include "vk_mem_alloc.h"

#define VMA_VULKAN_VERSION 1004000 // Vulkan 1.4
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

namespace rasm::gfx
{
    // clang-format off

    // Forward declarations of private helper functions for Vulkan setup and management.
    vkb::Result<vkb::Instance>          _init_instance(const char* app_name, bool enable_validation_layers);
    vkb::Result<vkb::PhysicalDevice>    _init_physical_device(const vkb::Instance& vkb_instance, VkSurfaceKHR surface);
    vkb::Result<vkb::Device>            _init_logical_device(const vkb::PhysicalDevice& physical_device);
    vkb::Result<VkQueue>                _init_queue(const vkb::Device& device, vkb::QueueType type);
    vkb::Result<uint32_t>               _get_queue_index(const vkb::Device& device, vkb::QueueType type);
    vkb::Result<VkSurfaceKHR>           _init_surface(const vkb::Instance& instance, Engine* engine);
    vkb::Result<vkb::Swapchain>         _init_swapchain(const vkb::Device& device);
    vkb::Result<vkb::Swapchain>         _recreate_swapchain(const vkb::Device& device, SwapchainVKHandle& old_swapchain);
    VkBufferUsageFlags                  _to_vk_buffer_usage_flags(BufferUsage usage);
    VkImageUsageFlags                   _to_vk_image_usage_flags(TextureUsage usage);
    VkFormat                            _to_vk_format(Format format);
    Format                              _to_format(VkFormat format);
    VkIndexType                         _to_vk_index_type(Format format);
    VkPrimitiveTopology                 _to_vk_topology(PrimitiveTopology topology);
    VkShaderStageFlags                  _to_vk_shader_stage_flags(ShaderType stage);
    VkImageAspectFlags                  _to_vk_aspect_mask(TextureUsage usage);
    VkDescriptorType                    _to_vk_descriptor_type(ResourceType type);
    const char* _to_vk_result_string(VkResult result);

    // clang-format on
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
        auto vkb_surface = _init_surface(vkb_instance.value(), engine);
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

        // Create the dispatch table
        this->dispatch_table = this->device.make_table();

        // get graphics queue
        auto vkb_queue = _init_queue(vkb_device.value(), vkb::QueueType::graphics);

        if (!vkb_queue)
            return false;
        this->graphics_queue = vkb_queue.value();

        // get graphics queue family index
        auto graphics_queue_index_result = _get_queue_index(vkb_device.value(), vkb::QueueType::graphics);

        if (!graphics_queue_index_result)
            return false;

        this->graphics_queue_family_index = graphics_queue_index_result.value();

        // create swapchain
        auto vkb_swapchain = _init_swapchain(vkb_device.value());

        if (!vkb_swapchain)
            return false;

        this->swapchain.swapchain = vkb_swapchain.value();
        this->swapchain.imageFormat = _to_format(swapchain.swapchain.image_format);
        this->swapchain.imageCount = swapchain.swapchain.image_count;
        auto images = swapchain.swapchain.get_images().value();
        auto views = swapchain.swapchain.get_image_views().value();

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

        // create VMA allocator
        VmaVulkanFunctions vkFunctions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
            .vkCreateImage = vkCreateImage,
        };

        VmaAllocatorCreateInfo allocatorCreateInfo = {};
        allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT | VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_4;
        allocatorCreateInfo.physicalDevice = physical_device.physical_device;
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

        vkb::destroy_swapchain(swapchain.swapchain);
        vmaDestroyAllocator(allocator);
        vkb::destroy_device(device);
        vkb::destroy_surface(instance, surface);
        vkb::destroy_instance(instance);
    }

    void VulkanContext::waitIdle()
    {
        vkDeviceWaitIdle(this->device.device);
    }

    std::optional<BufferVKHandle> VulkanContext::createBuffer(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::BUFFER);

        VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.buffer.size;
        bufferInfo.usage = _to_vk_buffer_usage_flags(desc.buffer.usage);

        VmaAllocationCreateInfo allocCI = {};
        allocCI.usage = VMA_MEMORY_USAGE_AUTO;
        allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer buffer;
        VmaAllocation allocation;
        VmaAllocationInfo allocInfo;
        auto result = vmaCreateBuffer(this->allocator, &bufferInfo, &allocCI, &buffer, &allocation, &allocInfo);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create buffer. Error: {}", _to_vk_result_string(result));
            return {};
        }

        if (desc.buffer.usage == BufferUsage::DEVICE_ADDRESS || desc.buffer.usage == BufferUsage::STORAGE)
        {
            VkBufferDeviceAddressInfo bufferAddressInfo = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            bufferAddressInfo.buffer = buffer;
            desc.buffer.deviceAddressBuffer.address = vkGetBufferDeviceAddress(this->device.device, &bufferAddressInfo);
        }

        // Set a debug name for the buffer
        VkDebugUtilsObjectNameInfoEXT bufferNameInfo = {};
        bufferNameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        bufferNameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
        bufferNameInfo.objectHandle = (uint64_t)buffer;
        bufferNameInfo.pObjectName = desc.name.c_str();

        this->dispatch_table.setDebugUtilsObjectNameEXT(&bufferNameInfo);

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
                                              .levelCount = 1,
                                              .baseArrayLayer = 0,
                                              .layerCount = 1};

        auto result = vkTransitionImageLayout(device, 1, &transitionDstInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to transition image layout. Error: {}", _to_vk_result_string(result));
            return;
        }

        // 2. Copy raw pixels directly into the OPTIMAL image
        VkMemoryToImageCopy region = {};
        region.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY;
        region.pHostPointer = data;
        region.memoryRowLength = texture.desc.texture.width;
        region.memoryImageHeight = texture.desc.texture.height;
        region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .mipLevel = 0,
                                   .baseArrayLayer = 0,
                                   .layerCount = 1};
        region.imageExtent = {texture.desc.texture.width, texture.desc.texture.height, 1};

        VkCopyMemoryToImageInfo copyInfo = {};
        copyInfo.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO;
        copyInfo.flags = 0;
        copyInfo.dstImage = texture.image;
        copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        copyInfo.regionCount = 1;
        copyInfo.pRegions = &region;

        result = vkCopyMemoryToImage(device, &copyInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to copy memory to image. Error: {}", _to_vk_result_string(result));
            return;
        }
    }

    std::optional<TextureVKHandle> VulkanContext::createTexture(ResourceDesc desc)
    {
        assert(desc.type == ResourceType::TEXTURE);

        // TODO: check if the format is supported by the device, and if not, find a compatible one
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
            spdlog::error("Failed to create image. Error: {}", _to_vk_result_string(result));
            return {};
        }

        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = _to_vk_format(desc.texture.format);
        viewInfo.subresourceRange.aspectMask = _to_vk_aspect_mask(desc.texture.usage);
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView imageView;
        result = vkCreateImageView(this->device.device, &viewInfo, nullptr, &imageView);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create image view. Error: {}", _to_vk_result_string(result));
            vmaDestroyImage(this->allocator, image, allocation);
            return {};
        }

        // create a sampler for the texture
        VkSampler sampler = VK_NULL_HANDLE;
        if (desc.texture.usage == TextureUsage::SAMPLED ||
            desc.texture.usage == TextureUsage::SAMPLED_COLOR_ATTACHMENT ||
            desc.texture.usage == TextureUsage::SAMPLED_DEPTH_STENCIL_ATTACHMENT)
        {

            VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

            result = vkCreateSampler(this->device.device, &samplerInfo, nullptr, &sampler);
            if (result != VK_SUCCESS)
            {
                spdlog::error("Failed to create sampler. Error: {}", _to_vk_result_string(result));
                vkDestroyImageView(this->device.device, imageView, nullptr);
                vmaDestroyImage(this->allocator, image, allocation);
                return {};
            }
        }

        // Set a debug name for the image
        VkDebugUtilsObjectNameInfoEXT imageNameInfo = {};
        imageNameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        imageNameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
        imageNameInfo.objectHandle = (uint64_t)image;
        imageNameInfo.pObjectName = desc.name.c_str();

        // Set a debug name for the image view
        VkDebugUtilsObjectNameInfoEXT imageViewNameInfo = {};
        imageViewNameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        imageViewNameInfo.objectType = VK_OBJECT_TYPE_IMAGE_VIEW;
        imageViewNameInfo.objectHandle = (uint64_t)imageView;
        imageViewNameInfo.pObjectName = desc.name.c_str();

        this->dispatch_table.setDebugUtilsObjectNameEXT(&imageNameInfo);
        this->dispatch_table.setDebugUtilsObjectNameEXT(&imageViewNameInfo);

        TextureVKHandle rawHandle = {desc, {}, image, imageView, sampler, allocation};
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
            spdlog::error("Failed to create shader module. Error: {}", _to_vk_result_string(result));
            return {};
        }

        // Set a debug name for the shader module
        VkDebugUtilsObjectNameInfoEXT shaderNameInfo = {};
        shaderNameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        shaderNameInfo.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
        shaderNameInfo.objectHandle = (uint64_t)shaderModule;
        shaderNameInfo.pObjectName = desc.name.c_str();

        this->dispatch_table.setDebugUtilsObjectNameEXT(&shaderNameInfo);

        ShaderVKHandle rawHandle = {desc, {}, shaderModule};
        return rawHandle;
    }

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

    std::optional<PipelineVKHandle> VulkanContext::createGraphicsPipeline(ResourceDesc desc, const ShaderVKHandle &vertexShader, const ShaderVKHandle &fragmentShader, const DescriptorSetLayoutVKHandle &bindlessDescriptorSetLayout)
    {
        VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
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
            spdlog::error("Failed to create pipeline layout. Error: {}", _to_vk_result_string(result));
            return {};
        }

        auto vertexPulling = desc.pipeline.vertexInputLayout.vertexPulling;

        VkVertexInputBindingDescription vertexBinding{
            .binding = desc.pipeline.vertexInputLayout.binding,
            .stride = desc.pipeline.vertexInputLayout.stride,
            .inputRate = desc.pipeline.vertexInputLayout.perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};

        std::vector<VkVertexInputAttributeDescription> vertexAttributes;
        for (const auto &attr : desc.pipeline.vertexInputLayout.attributes)
        {
            if (!attr.used)
                continue;

            vertexAttributes.push_back({.location = attr.location,
                                        .binding = attr.binding,
                                        .format = _to_vk_format(attr.format),
                                        .offset = attr.offset});
        }

        VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertexBinding,
            .vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size()),
            .pVertexAttributeDescriptions = vertexAttributes.data()};

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = _to_vk_topology(desc.pipeline.topology),
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

        std::vector<VkDynamicState> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        if (vertexPulling)
            dynamic_states.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);

        VkPipelineDynamicStateCreateInfo dynamicStateInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
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
        VkFormat colorAttachmentFormat = _to_vk_format(desc.pipeline.colorAttachmentFormat);
        VkFormat depthAttachmentFormat = _to_vk_format(desc.pipeline.depthStencilAttachmentFormat);

        VkPipelineRenderingCreateInfo pipelineRenderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorAttachmentFormat,
            .depthAttachmentFormat = depthAttachmentFormat,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
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
            .pVertexInputState = vertexPulling ? nullptr : &vertexInputInfo,
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
            spdlog::error("Failed to create graphics pipeline. Error: {}", _to_vk_result_string(result));
            vkDestroyPipelineLayout(this->device.device, pipelineLayout, nullptr);
            return {};
        }

        // Set a debug name for the pipeline
        VkDebugUtilsObjectNameInfoEXT pipelineNameInfo = {};
        pipelineNameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        pipelineNameInfo.objectType = VK_OBJECT_TYPE_PIPELINE;
        pipelineNameInfo.objectHandle = (uint64_t)pipeline;
        pipelineNameInfo.pObjectName = desc.name.c_str();

        this->dispatch_table.setDebugUtilsObjectNameEXT(&pipelineNameInfo);

        PipelineVKHandle rawHandle = {desc, {}, pipeline, pipelineLayout};
        return rawHandle;
    }

    std::optional<CommandPoolVKHandle> VulkanContext::createCommandPool(vkb::QueueType type)
    {
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        auto queue_index_result = _get_queue_index(this->device, type);
        if (!queue_index_result)
        {
            spdlog::error("Failed to get queue family index for command pool.");
            return {};
        }
        poolInfo.queueFamilyIndex = queue_index_result.value();

        VkCommandPool commandPool;
        auto result = vkCreateCommandPool(this->device.device, &poolInfo, nullptr, &commandPool);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create command pool. Error: {}", _to_vk_result_string(result));
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
            spdlog::error("Failed to allocate command buffer. Error: {}", _to_vk_result_string(result));
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
            spdlog::error("Failed to create semaphore. Error: {}", _to_vk_result_string(result));
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
            spdlog::error("Failed to create fence. Error: {}", _to_vk_result_string(result));
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
        samplerLayoutBinding.descriptorType = _to_vk_descriptor_type(desc.bindlessDescriptorSetLayout.type);
        samplerLayoutBinding.descriptorCount = desc.bindlessDescriptorSetLayout.count;
        samplerLayoutBinding.stageFlags = _to_vk_shader_stage_flags(desc.bindlessDescriptorSetLayout.stage);

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
            vkPoolSize.type = _to_vk_descriptor_type(poolSize.descriptorType);
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
        return _to_format(swapchain.swapchain.image_format);
    }

    std::optional<SwapchainVKHandle> VulkanContext::recreateSwapchain()
    {
        auto swapchain_ret = _recreate_swapchain(this->device, this->swapchain);

        if (!swapchain_ret)
        {
            spdlog::error("Failed to recreate swapchain.");
            return {};
        }

        this->swapchain.swapchain = swapchain_ret.value();
        this->swapchain.imageFormat = _to_format(swapchain.swapchain.image_format);
        this->swapchain.imageCount = swapchain.swapchain.image_count;

        auto images = swapchain.swapchain.get_images().value();
        auto views = swapchain.swapchain.get_image_views().value();

        this->swapchain.images.clear();
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

        return this->swapchain;
    }

    void VulkanContext::fillBuffer(const BufferVKHandle &buffer, const void *data, size_t size, size_t offset)
    {
        assert(offset + size <= buffer.desc.buffer.size);
        std::memcpy(static_cast<uint8_t *>(buffer.allocationInfo.pMappedData) + offset, data, size);
    }

    bool VulkanContext::updateBindlessDescriptorSet(const DescriptorSetVKHandle &bindlessSet, const TextureVKHandle &texture, uint32_t slot)
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
        return true;
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

        oldLayout = usageToLayout(oldUsage);
        newLayout = usageToLayout(newUsage);

        auto getAccessMaskAndStage = [](TextureUsage newUsage, VkAccessFlags2 &srcAccessMask, VkAccessFlags2 &dstAccessMask, VkPipelineStageFlags2 &srcStageMask, VkPipelineStageFlags2 &dstStageMask)
        {
            switch (newUsage)
            {
            case TextureUsage::COLOR_ATTACHMENT:
                srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

                srcAccessMask = VK_ACCESS_2_NONE;
                dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                break;
            case TextureUsage::PRESENT_SRC:
                srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                dstStageMask = VK_PIPELINE_STAGE_2_NONE;

                srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                dstAccessMask = VK_ACCESS_2_NONE;
                break;
            case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
                srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT_KHR | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR;

                srcAccessMask = VK_ACCESS_2_NONE;
                dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                break;
            case TextureUsage::SAMPLED:
                srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

                srcAccessMask = VK_ACCESS_2_NONE;
                dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                break;
            default:
                spdlog::error("Unsupported new texture usage for layout transition.");
                break;
            }
        };

        getAccessMaskAndStage(newUsage, srcAccessMask, dstAccessMask, srcStageMask, dstStageMask);

        VkImageAspectFlags aspectMask = (image.desc.texture.usage == TextureUsage::DEPTH_STENCIL_ATTACHMENT ||
                                         image.desc.texture.usage == TextureUsage::SAMPLED_DEPTH_STENCIL_ATTACHMENT)
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
                                             .levelCount = 1,
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
        auto reset_result = vkResetCommandBuffer(commandBuffer.commandBuffer, 0);
        if (reset_result != VK_SUCCESS)
        {
            spdlog::error("Failed to reset command buffer. Error: {}", _to_vk_result_string(reset_result));
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        auto begin_result = vkBeginCommandBuffer(commandBuffer.commandBuffer, &beginInfo);
        if (begin_result != VK_SUCCESS)
        {
            spdlog::error("Failed to begin command buffer. Error: {}", _to_vk_result_string(begin_result));
            return false;
        }
        return true;
    }

    bool VulkanContext::endCommandBuffer(const CommandBufferVKHandle &commandBuffer)
    {
        auto result = vkEndCommandBuffer(commandBuffer.commandBuffer);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to end command buffer. Error: {}", _to_vk_result_string(result));
            return false;
        }
        return true;
    }

    bool VulkanContext::beginRendering(const CommandBufferVKHandle &commandBuffer, const TextureVKHandle &colorAttachment, const TextureVKHandle &depthAttachment)
    {
        // begin dynamic rendering
        VkRenderingAttachmentInfo colorAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = colorAttachment.view,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{.color{1.0f, 0.0f, 0.0f, 1.0f}}};

        VkRenderingAttachmentInfo depthAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depthAttachment.view,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = {.depthStencil = {1.0f, 0}}};

        VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{
                .extent{
                    .width = colorAttachment.desc.texture.width,
                    .height = colorAttachment.desc.texture.height}},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachmentInfo,
            .pDepthAttachment = &depthAttachmentInfo};

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
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

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
            .pWaitDstStageMask = waitStages,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer.commandBuffer,
            .signalSemaphoreCount = static_cast<uint32_t>(signalVkSemaphores.size()),
            .pSignalSemaphores = signalVkSemaphores.data()};

        auto result = vkQueueSubmit(this->graphics_queue, 1, &submitInfo, fence.fence);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to submit command buffer. Error: {}", _to_vk_result_string(result));
            return false;
        }
        return true;
    }
    bool VulkanContext::present(uint32_t imageIndex, const SemaphoreVKHandle &waitSemaphore)
    {
        VkPresentInfoKHR presentInfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &waitSemaphore.semaphore,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain.swapchain.swapchain,
            .pImageIndices = &imageIndex};

        auto result = vkQueuePresentKHR(this->graphics_queue, &presentInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to present swapchain image. Error: {}", _to_vk_result_string(result));
            return false;
        }
        return true;
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

    void VulkanContext::bindVertexBuffer(const CommandBufferVKHandle &commandBuffer, const BufferVKHandle &buffer, uint64_t offset, uint32_t binding)
    {
        VkBuffer vertexBuffers[] = {buffer.buffer};
        VkDeviceSize offsets[] = {offset};
        vkCmdBindVertexBuffers(commandBuffer.commandBuffer, binding, 1, vertexBuffers, offsets);
    }

    void VulkanContext::bindIndexBuffer(const CommandBufferVKHandle &commandBuffer, const BufferVKHandle &buffer, uint64_t offset, Format indexType)
    {
        auto vkIndexType = _to_vk_index_type(indexType);
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
        auto stageFlags = _to_vk_shader_stage_flags(stage);
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
        this->dispatch_table.cmdSetVertexInputEXT(commandBuffer.commandBuffer, 0, nullptr, 0 , nullptr);
        vkCmdDrawIndexedIndirect(commandBuffer.commandBuffer, buffer.buffer, offset, drawCount, stride);
    }

    bool VulkanContext::waitForFence(const FenceVKHandle &fence, uint64_t timeout)
    {
        auto result = vkWaitForFences(this->device.device, 1, &fence.fence, VK_TRUE, timeout);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to wait for fence. Error: {}", _to_vk_result_string(result));
            return false;
        }
        return true;
    }

    bool VulkanContext::resetFence(const FenceVKHandle &fence)
    {
        auto result = vkResetFences(this->device.device, 1, &fence.fence);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to reset fence. Error: {}", _to_vk_result_string(result));
            return false;
        }
        return true;
    }

    bool VulkanContext::acquireNextImage(const SemaphoreVKHandle &semaphore, uint64_t timeout, uint32_t &imageIndex)
    {
        auto result = vkAcquireNextImageKHR(this->device.device, this->swapchain.swapchain, timeout, semaphore.semaphore, VK_NULL_HANDLE, &imageIndex);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to acquire next image. Error: {}", _to_vk_result_string(result));
            return false;
        }
        return true;
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

        vkGetPhysicalDeviceMemoryProperties2(physical_device, &memoryProperties2);

        for (uint32_t i = 0; i < memoryProperties2.memoryProperties.memoryHeapCount; ++i)
        {
            const VkMemoryHeap &heap = memoryProperties2.memoryProperties.memoryHeaps[i];

            VkDeviceSize budget = budgetProperties.heapBudget[i];
            VkDeviceSize usage = budgetProperties.heapUsage[i];

            bool isDeviceLocal = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;

            if (isDeviceLocal)
            {
                vramStats.vram_budget += budget;
                vramStats.vram_usage += usage;
            }
            else
            {
                vramStats.host_budget += budget;
                vramStats.host_usage += usage;
            }
        }

        return vramStats;
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
                .use_default_debug_messenger()
                .enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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

        VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex_input_features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
            .vertexInputDynamicState = VK_TRUE,
        };

        // select() grabs a PhysicalDevice, By default, this will prefer a discrete GPU.
        auto physical_device_selector_return = phys_device_selector
                                                   .set_surface(surface)
                                                   .set_required_features_14(enabledVk14Features)               // Enable the 1.4 core feature
                                                   .set_required_features_13(enabledVk13Features)               // Enable the 1.3 core feature
                                                   .set_required_features_12(enabledVk12Features)               // Enable the 1.2 core feature
                                                   .set_required_features_11(enabledVk11Features)               // Enable the 1.1 core feature
                                                   .set_required_features(enabledVk10Features)                  // Enable the 1.0 core feature
                                                   .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)     // Enable swapchain extension
                                                   .add_required_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) // Enable memory budget extension
                                                   .add_required_extension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) // Enable vertex input dynamic state extension
                                                   .add_required_extension_features(vertex_input_features) // Enable vertex input dynamic state extension
                                                   .select();

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
            return physical_device_selector_return;
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

    vkb::Result<VkQueue> _init_queue(const vkb::Device &device, vkb::QueueType type)
    {
        auto queue_ret = device.get_queue(type);
        if (!queue_ret)
        {
            spdlog::error("Failed to get queue. Error: {}", queue_ret.error().message());
        }
        return queue_ret;
    }

    vkb::Result<uint32_t> _get_queue_index(const vkb::Device &device, vkb::QueueType type)
    {
        auto queue_index_ret = device.get_queue_index(type);
        if (!queue_index_ret)
        {
            spdlog::error("Failed to get queue index. Error: {}", queue_index_ret.error().message());
        }
        return queue_index_ret;
    }

    vkb::Result<VkSurfaceKHR> _init_surface(const vkb::Instance &instance, Engine *engine)
    {
        auto handle = engine->getMainWindow();
        auto surface_ret = engine->createSurfaceVk(handle, instance.instance);

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

    vkb::Result<vkb::Swapchain> _recreate_swapchain(const vkb::Device &device, SwapchainVKHandle &old_swapchain)
    {
        vkDeviceWaitIdle(device.device);

        vkb::SwapchainBuilder swapchain_builder{device};
        auto swap_ret = swapchain_builder.set_old_swapchain(old_swapchain.swapchain).build();
        if (!swap_ret)
        {
            spdlog::error("Failed to recreate swapchain. Error: {}", swap_ret.error().message());
            // If it failed to create a swapchain, the old swapchain handle is invalid.
            old_swapchain.swapchain.swapchain = VK_NULL_HANDLE;
        }
        // Even though we recycled the previous swapchain, we need to free its resources.
        vkb::destroy_swapchain(old_swapchain.swapchain);

        for (const auto &image : old_swapchain.images)
        {
            vkDestroyImageView(device.device, image.view, nullptr);
        }

        // Get the new swapchain and place it in our variable
        return swap_ret;
    }

    VkBufferUsageFlags _to_vk_buffer_usage_flags(BufferUsage usage)
    {
        switch (usage)
        {
        case BufferUsage::VERTEX:
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        case BufferUsage::INDEX:
            return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        case BufferUsage::INDIRECT:
            return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        case BufferUsage::VERTEXINDEX:
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        case BufferUsage::UNIFORM:
            return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        case BufferUsage::STORAGE:
            return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        case BufferUsage::DEVICE_ADDRESS:
            return VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        default:
            return static_cast<VkBufferUsageFlags>(0);
        }
    }

    VkShaderStageFlags _to_vk_shader_stage_flags(ShaderType stage)
    {
        switch (stage)
        {
        case ShaderType::VERTEX:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderType::FRAGMENT:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderType::COMPUTE:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        default:
            return static_cast<VkShaderStageFlags>(0);
        }
    }

    VkPrimitiveTopology _to_vk_topology(PrimitiveTopology topology)
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

    VkFormat _to_vk_format(Format format)
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

    VkIndexType _to_vk_index_type(Format format)
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

    Format _to_format(VkFormat vkFormat)
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
        default:
            return Format::UNKNOWN;
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
        case TextureUsage::SAMPLED_DEPTH_STENCIL_ATTACHMENT:
            return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        case TextureUsage::TRANSIENT_ATTACHMENT:
            return VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        case TextureUsage::INPUT_ATTACHMENT:
            return VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        default:
            return 0;
        }
    }

    VkImageAspectFlags _to_vk_aspect_mask(TextureUsage usage)
    {
        switch (usage)
        {
        case TextureUsage::COLOR_ATTACHMENT:
        case TextureUsage::SAMPLED:
        case TextureUsage::SAMPLED_COLOR_ATTACHMENT:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
        case TextureUsage::SAMPLED_DEPTH_STENCIL_ATTACHMENT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        default:
            return 0;
        }
    }

    VkDescriptorType _to_vk_descriptor_type(ResourceType type)
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

    const char *_to_vk_result_string(VkResult result)
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
        default:
            return "Unknown Error";
        }
    }
}
