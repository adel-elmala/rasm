#include "rhi/vulkan/vulkanCommandBuffer.h"
#include "rhi/vulkan/vulkanDevice.h"
#include "rhi/vulkan/vulkanUtils.h"

#include "spdlog/spdlog.h"

namespace rasm::rhi
{
    namespace
    {
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

        VkImageLayout toVkImageLayout(TextureUsage usage)
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
                return VK_IMAGE_LAYOUT_UNDEFINED;
            }
        }

        // What the barrier waits for depends only on how the image was used before.
        bool getSrcScope(TextureUsage oldUsage, VkAccessFlags2 &srcAccessMask, VkPipelineStageFlags2 &srcStageMask)
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
        }

        bool getDstScope(TextureUsage newUsage, VkAccessFlags2 &dstAccessMask, VkPipelineStageFlags2 &dstStageMask)
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
        }

        VkCullModeFlags toVkCullMode(CullMode cullMode)
        {
            switch (cullMode)
            {
            case CullMode::FRONT:
                return VK_CULL_MODE_FRONT_BIT;
            case CullMode::BACK:
                return VK_CULL_MODE_BACK_BIT;
            default:
                return VK_CULL_MODE_NONE;
            }
        }
    }

    VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice &device) : device(device) {}

    VulkanCommandBuffer::~VulkanCommandBuffer()
    {
        // Frees the command buffer with it.
        if (pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device.getVkDevice(), pool, nullptr);
    }

    bool VulkanCommandBuffer::initialize(uint32_t queueFamilyIndex, const std::string &name)
    {
        // One pool per command buffer, reset as a whole in begin(): the buffer is re-recorded every time it is used.
        VkCommandPoolCreateInfo poolInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = queueFamilyIndex,
        };
        auto result = vkCreateCommandPool(device.getVkDevice(), &poolInfo, nullptr, &pool);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to create command pool. Error: {}", toVkResultString(result));
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        result = vkAllocateCommandBuffers(device.getVkDevice(), &allocInfo, &commandBuffer);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to allocate command buffer. Error: {}", toVkResultString(result));
            return false;
        }

        device.setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, reinterpret_cast<uint64_t>(pool), name);
        device.setObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<uint64_t>(commandBuffer), name);
        return true;
    }

    bool VulkanCommandBuffer::begin()
    {
        auto result = vkResetCommandPool(device.getVkDevice(), pool, 0);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to reset command pool. Error: {}", toVkResultString(result));
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to begin command buffer. Error: {}", toVkResultString(result));
            return false;
        }
        return true;
    }

    bool VulkanCommandBuffer::end()
    {
        auto result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to end command buffer. Error: {}", toVkResultString(result));
            return false;
        }
        return true;
    }

    bool VulkanCommandBuffer::transitionTexture(Texture *texture, TextureUsage oldUsage, TextureUsage newUsage)
    {
        auto *vkTexture = static_cast<VulkanTexture *>(texture);

        VkAccessFlags2 srcAccessMask = 0;
        VkAccessFlags2 dstAccessMask = 0;
        VkPipelineStageFlags2 srcStageMask = 0;
        VkPipelineStageFlags2 dstStageMask = 0;
        if (!getSrcScope(oldUsage, srcAccessMask, srcStageMask) || !getDstScope(newUsage, dstAccessMask, dstStageMask))
            return false;

        VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                         .srcStageMask = srcStageMask,
                                         .srcAccessMask = srcAccessMask,
                                         .dstStageMask = dstStageMask,
                                         .dstAccessMask = dstAccessMask,
                                         .oldLayout = toVkImageLayout(oldUsage),
                                         .newLayout = toVkImageLayout(newUsage),
                                         .image = vkTexture->image,
                                         .subresourceRange = {
                                             .aspectMask = toVkAspectMask(texture->desc.usage),
                                             .baseMipLevel = 0,
                                             .levelCount = vkTexture->mipLevels,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1,
                                         }};

        VkDependencyInfo dependencyInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                           .imageMemoryBarrierCount = 1,
                                           .pImageMemoryBarriers = &barrier};

        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
        return true;
    }

    void VulkanCommandBuffer::beginRendering(Texture *colorAttachment, Texture *depthAttachment, const glm::vec4 &clearColor)
    {
        auto *color = static_cast<VulkanTexture *>(colorAttachment);
        auto *depth = static_cast<VulkanTexture *>(depthAttachment);

        VkRenderingAttachmentInfo colorAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = color->view,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{.color{clearColor.r, clearColor.g, clearColor.b, clearColor.a}}};

        VkRenderingAttachmentInfo depthAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depth->view,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = {.depthStencil = {1.0f, 0}}};

        // Must match the pipeline's stencilAttachmentFormat (see createGraphicsPipeline).
        VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{
                .extent{
                    .width = color->desc.width,
                    .height = color->desc.height}},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachmentInfo,
            .pDepthAttachment = &depthAttachmentInfo,
            .pStencilAttachment = hasStencil(depth->desc.format) ? &depthAttachmentInfo : nullptr};

        vkCmdBeginRendering(commandBuffer, &renderingInfo);
    }

    void VulkanCommandBuffer::endRendering()
    {
        vkCmdEndRendering(commandBuffer);
    }

    void VulkanCommandBuffer::setViewport(float x, float y, float width, float height, float minDepth, float maxDepth)
    {
        // RASM clip space is Y-up, Vulkan's is Y-down. A negative height starting at the bottom edge flips it (D8).
        VkViewport viewport = {
            .x = x,
            .y = y + height,
            .width = width,
            .height = -height,
            .minDepth = minDepth,
            .maxDepth = maxDepth};

        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    }

    void VulkanCommandBuffer::setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        VkRect2D scissor = {
            .offset = {x, y},
            .extent = {width, height}};

        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void VulkanCommandBuffer::setCullMode(CullMode cullMode)
    {
        vkCmdSetCullMode(commandBuffer, toVkCullMode(cullMode));
    }

    void VulkanCommandBuffer::bindPipeline(Pipeline *pipeline)
    {
        auto *vkPipeline = static_cast<VulkanPipeline *>(pipeline);
        vkCmdBindPipeline(commandBuffer, vkPipeline->bindPoint, vkPipeline->pipeline);
    }

    void VulkanCommandBuffer::bindIndexBuffer(Buffer *buffer, uint64_t offset, Format indexType)
    {
        vkCmdBindIndexBuffer(commandBuffer, static_cast<VulkanBuffer *>(buffer)->buffer, offset, toVkIndexType(indexType));
    }

    void VulkanCommandBuffer::bindDescriptorSet(Pipeline *pipeline, DescriptorSet *descriptorSet, uint32_t setIndex)
    {
        auto *vkPipeline = static_cast<VulkanPipeline *>(pipeline);
        vkCmdBindDescriptorSets(commandBuffer, vkPipeline->bindPoint, vkPipeline->layout, setIndex, 1, &static_cast<VulkanDescriptorSet *>(descriptorSet)->set, 0, nullptr);
    }

    void VulkanCommandBuffer::pushConstants(Pipeline *pipeline, ShaderType stages, const void *data, uint32_t size, uint32_t offset)
    {
        vkCmdPushConstants(commandBuffer, static_cast<VulkanPipeline *>(pipeline)->layout, toVkShaderStageFlags(stages), offset, size, data);
    }

    void VulkanCommandBuffer::draw(uint32_t vertexCount, uint32_t instanceCount)
    {
        vkCmdDraw(commandBuffer, vertexCount, instanceCount, 0, 0);
    }

    void VulkanCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t vertexOffset, uint32_t firstIndex, uint32_t firstInstance)
    {
        vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void VulkanCommandBuffer::drawIndexedIndirect(Buffer *buffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        vkCmdDrawIndexedIndirect(commandBuffer, static_cast<VulkanBuffer *>(buffer)->buffer, offset, drawCount, stride);
    }

    void VulkanCommandBuffer::copyBuffers(Buffer *src, const BufferCopy *copies, uint32_t copyCount)
    {
        // The copies may overwrite data that earlier submissions on this queue still read or write, and any
        // later command may read the result. Earlier submissions are in the first barrier's scope, later
        // ones in the second's, so the same queue needs no semaphore.
        VkMemoryBarrier2 before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
        VkDependencyInfo dependencyInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &before};
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

        auto srcBuffer = static_cast<VulkanBuffer *>(src)->buffer;
        for (uint32_t i = 0; i < copyCount; ++i)
        {
            const auto &copy = copies[i];
            VkBufferCopy region = {.srcOffset = copy.srcOffset, .dstOffset = copy.dstOffset, .size = copy.size};
            vkCmdCopyBuffer(commandBuffer, srcBuffer, static_cast<VulkanBuffer *>(copy.dst)->buffer, 1, &region);
        }

        VkMemoryBarrier2 after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        dependencyInfo.pMemoryBarriers = &after;
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

    VkCommandBuffer VulkanCommandBuffer::getVkCommandBuffer() const
    {
        return commandBuffer;
    }
}
