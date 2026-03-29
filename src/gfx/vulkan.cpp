#include <cassert>

#include "rasm/core/engine.h"
#include "rasm/gfx/vulkan.h"

#include "spdlog/spdlog.h"

#define VMA_VULKAN_VERSION 1004000 // Vulkan 1.4
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

namespace rasm::gfx
{
    // clang-format off

    // Forward declarations of private helper functions for Vulkan setup and management.
    vkb::Result<vkb::Instance>          _init_instance(const char *app_name, bool enable_validation_layers);
    vkb::Result<vkb::PhysicalDevice>    _init_physical_device(const vkb::Instance &vkb_instance, VkSurfaceKHR surface);
    vkb::Result<vkb::Device>            _init_logical_device(const vkb::PhysicalDevice &physical_device);
    vkb::Result<VkQueue>                _init_queue(const vkb::Device &device, vkb::QueueType type);
    vkb::Result<uint32_t>               _get_queue_index(const vkb::Device &device, vkb::QueueType type);
    vkb::Result<VkSurfaceKHR>           _init_surface(const vkb::Instance &instance, const Window &window, WindowHandle handle);
    vkb::Result<vkb::Swapchain>         _init_swapchain(const vkb::Device &device);
    VkBufferUsageFlagBits               _to_vk_buffer_usage_flags(BufferUsage usage);
    VkImageUsageFlags                   _to_vk_image_usage_flags(TextureUsage usage);
    VkFormat                            _to_vk_format(TextureFormat format);
    const char *                        _to_vk_result_string(VkResult result);

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

        // get graphics queue
        auto vkb_queue = _init_queue(vkb_device.value(), vkb::QueueType::graphics);

        if (!vkb_queue)
            return false;
        this->graphics_queue = vkb_queue.value();

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

    std::optional<ShaderVKHandle> VulkanContext::createShader(ResourceDesc desc)
    {
        assert(desc.type == ResourceDesc::Type::SHADER);

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

        ShaderVKHandle rawHandle = {desc, {}, shaderModule};
        return rawHandle;
    }

    VkViewport createViewPort(float width, float height, float minDepth, float maxDepth)
    {
        VkViewport viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = width,
            .height = height,
            .minDepth = minDepth,
            .maxDepth = maxDepth,
        };

        return viewport;
    }

    VkRect2D createScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        VkRect2D scissor = {
            .offset = {x, y},
            .extent = {width, height}};

        return scissor;
    }

    VkPipelineRasterizationStateCreateInfo createRasterizer(VkPolygonMode polygonMode, VkCullModeFlags cullMode, VkFrontFace frontFace)
    {
        VkPipelineRasterizationStateCreateInfo rasterizer{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                          .depthClampEnable = VK_FALSE,
                                                          .rasterizerDiscardEnable = VK_FALSE,
                                                          .polygonMode = polygonMode,
                                                          .cullMode = cullMode,
                                                          .frontFace = frontFace,
                                                          .depthBiasEnable = VK_FALSE,
                                                          .lineWidth = 1.0f};
        return rasterizer;
    }

    std::optional<PipelineVKHandle> VulkanContext::createGraphicsPipeline(ResourceDesc desc, const ShaderVKHandle &vertexShader, const ShaderVKHandle &fragmentShader)
    {
        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        VkPipelineDynamicStateCreateInfo dynamicStateInfo = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicStateInfo.pDynamicStates = dynamic_states;
        dynamicStateInfo.dynamicStateCount = 2;

        // skip for now
        VkPipelineVertexInputStateCreateInfo vertexInputInfo = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        // skip for now
        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport = createViewPort(static_cast<float>(swapchain.extent.width), static_cast<float>(swapchain.extent.height), 0.0f, 1.0f);

        viewport.maxDepth = 1.0f;

        VkRect2D scissor = createScissor(0, 0, swapchain.extent.width, swapchain.extent.height);

        VkPipelineRasterizationStateCreateInfo rasterizerInfo = createRasterizer(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);

        VkPipelineMultisampleStateCreateInfo multisampleInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                                                                .sampleShadingEnable = VK_FALSE};

        VkPipelineDepthStencilStateCreateInfo depthStencilInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                                  .depthTestEnable = VK_TRUE,
                                                                  .depthWriteEnable = VK_TRUE,
                                                                  .depthCompareOp = VK_COMPARE_OP_LESS,
                                                                  .depthBoundsTestEnable = VK_FALSE,
                                                                  .stencilTestEnable = VK_FALSE};

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

        VkPipelineColorBlendStateCreateInfo colorBlendInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                              .logicOpEnable = VK_FALSE,
                                                              .attachmentCount = 1,
                                                              .pAttachments = &colorBlendAttachment};

        VkPipelineLayoutCreateInfo pipelineLayoutInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                         .setLayoutCount = 0,
                                                         .pSetLayouts = nullptr,
                                                         .pushConstantRangeCount = 0,
                                                         .pPushConstantRanges = nullptr};

        VkPipelineLayout pipelineLayout;
        auto result = vkCreatePipelineLayout(this->device.device, &pipelineLayoutInfo, nullptr, &pipelineLayout);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create pipeline layout. Error: {}", _to_vk_result_string(result));
            return {};
        }

        VkPipelineShaderStageCreateInfo shaderStages[] = {
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertexShader.module,
                .pName = "main",
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragmentShader.module,
                .pName = "main",
            }};

        // Dynamic rendering: describe the formats used by this pipeline.
        // NOTE: These formats must match the actual color and depth attachments
        // used when beginning dynamic rendering.
        VkFormat colorAttachmentFormat = VK_FORMAT_B8G8R8A8_UNORM;
        VkFormat depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

        VkPipelineRenderingCreateInfo pipelineRenderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext = nullptr,
            .viewMask = 0,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorAttachmentFormat,
            .depthAttachmentFormat = depthAttachmentFormat,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };

        VkPipelineViewportStateCreateInfo viewportStateInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewportCount = 1,
            .pViewports = nullptr, // viewports are set dynamically
            .scissorCount = 1,
            .pScissors = nullptr, // scissors are set dynamically
        };

        VkGraphicsPipelineCreateInfo pipelineInfo = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
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
                                                     .layout = pipelineLayout,
                                                     .renderPass = VK_NULL_HANDLE, // dynamic rendering, will be set later
                                                     .subpass = 0};

        VkPipeline pipeline;
        result = vkCreateGraphicsPipelines(this->device.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create graphics pipeline. Error: {}", _to_vk_result_string(result));
            vkDestroyPipelineLayout(this->device.device, pipelineLayout, nullptr);
            return {};
        }

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

    std::optional<SwapchainVKHandle> VulkanContext::recreateSwapchain()
    {
        auto vkb_swapchain = _init_swapchain(this->device);

        if (!vkb_swapchain)
        {
            spdlog::error("Failed to recreate swapchain.");
            return {};
        }

        this->swapchain = vkb_swapchain.value();
        SwapchainVKHandle rawHandle = {{}, swapchain};
        return rawHandle;
    }

    void VulkanContext::transitionImageLayout(const CommandBufferVKHandle &commandBuffer, VkImage image, VkAccessFlags2 srcAccessMask, VkAccessFlags2 dstAccessMask, VkPipelineStageFlags2 srcStageMask, VkPipelineStageFlags2 dstStageMask, VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                         .srcStageMask = srcStageMask,
                                         .srcAccessMask = srcAccessMask,
                                         .dstStageMask = dstStageMask,
                                         .dstAccessMask = dstAccessMask,
                                         .oldLayout = oldLayout,
                                         .newLayout = newLayout,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = image,
                                         .subresourceRange = {
                                             .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                             .baseMipLevel = 0,
                                             .levelCount = 1,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1,
                                         }};

        VkDependencyInfo dependencyInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                           .imageMemoryBarrierCount = 1,
                                           .pImageMemoryBarriers = &barrier};

        vkCmdPipelineBarrier2(commandBuffer.commandBuffer, &dependencyInfo);
    }

    void VulkanContext::beginCommandBuffer(const CommandBufferVKHandle &commandBuffer)
    {
        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = 0;

        auto result = vkBeginCommandBuffer(commandBuffer.commandBuffer, &beginInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to begin command buffer. Error: {}", _to_vk_result_string(result));
        }
    }

    void VulkanContext::endCommandBuffer(const CommandBufferVKHandle &commandBuffer)
    {
        auto result = vkEndCommandBuffer(commandBuffer.commandBuffer);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to end command buffer. Error: {}", _to_vk_result_string(result));
        }
    }

    void VulkanContext::beginRendering(const CommandBufferVKHandle &commandBuffer)
    {
        // transition swapchain image to color attachment optimal layout before rendering
        auto swapchainImagesResult = swapchain.get_images();
        auto swapchainImageViewsResult = swapchain.get_image_views();
        if (!swapchainImagesResult || !swapchainImageViewsResult)
        {
            spdlog::error("Failed to get swapchain images or image views. Error: {} {}", swapchainImagesResult.error().message(), swapchainImageViewsResult.error().message());
            return;
        }
        auto swapchainImage = swapchainImagesResult.value()[this->current_swapchain_image];
        auto swapchainImageView = swapchainImageViewsResult.value()[this->current_swapchain_image];

        transitionImageLayout(commandBuffer, swapchainImage,
                              0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAttachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                     .imageView = swapchainImageView,
                                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                     .clearValue = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}}};

        VkRenderingInfo renderingInfo = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea = {.offset = {0, 0}, .extent = swapchain.extent},
                                         .layerCount = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments = &colorAttachment};

        vkCmdBeginRendering(commandBuffer.commandBuffer, &renderingInfo);
    }

    void VulkanContext::endRendering(const CommandBufferVKHandle &commandBuffer)
    {
        // transition swapchain image back to present src layout after rendering
        auto swapchainImagesResult = swapchain.get_images();
        if (!swapchainImagesResult)
        {
            spdlog::error("Failed to get swapchain images. Error: {}", swapchainImagesResult.error().message());
            return;
        }
        auto swapchainImage = swapchainImagesResult.value()[this->current_swapchain_image];

        transitionImageLayout(commandBuffer, swapchainImage,
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        vkCmdEndRendering(commandBuffer.commandBuffer);
    }

    void VulkanContext::removeBuffer(const BufferVKHandle &buffer)
    {
        vmaDestroyBuffer(this->allocator, buffer.buffer, buffer.allocation);
    }

    void VulkanContext::removeTexture(const TextureVKHandle &texture)
    {
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
        vkCmdBindPipeline(commandBuffer.commandBuffer, pipeline.desc.type == ResourceDesc::Type::GRAPHICS_PIPELINE ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    }

    void VulkanContext::setUniform(const CommandBufferVKHandle &commandBuffer, const std::string &name, const void *data, size_t size)
    {
        commandBuffer;
        name;
        data;
        size;
    }

    void VulkanContext::draw(const CommandBufferVKHandle &commandBuffer, uint32_t vertexCount, uint32_t instanceCount)
    {
        vkCmdDraw(commandBuffer.commandBuffer, vertexCount, instanceCount, 0, 0);
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

    vkb::Result<vkb::Swapchain> _recreate_swapchain(const vkb::Device &device, vkb::Swapchain &old_swapchain)
    {
        vkb::SwapchainBuilder swapchain_builder{device};
        auto swap_ret = swapchain_builder.set_old_swapchain(old_swapchain).build();
        if (!swap_ret)
        {
            spdlog::error("Failed to recreate swapchain. Error: {}", swap_ret.error().message());
            // If it failed to create a swapchain, the old swapchain handle is invalid.
            old_swapchain.swapchain = VK_NULL_HANDLE;
        }
        // Even though we recycled the previous swapchain, we need to free its resources.
        vkb::destroy_swapchain(old_swapchain);

        // Get the new swapchain and place it in our variable
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
        default:
            return "Unknown Error";
        }
    }
}
