#include "rhi/vulkan/vulkanCommandBuffer.h"
#include "rhi/vulkan/vulkanDevice.h"
#include "rhi/vulkan/vulkanUtils.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <vector>

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
            case TextureUsage::STORAGE:
                return VK_IMAGE_LAYOUT_GENERAL;
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
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                return true;
            case TextureUsage::COLOR_ATTACHMENT:
                srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
                srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT_KHR | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR;
                srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::STORAGE:
                srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                return true;
            case TextureUsage::SAMPLED:
                // Reads make nothing available, so only their stage is waited on.
                srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                srcAccessMask = VK_ACCESS_2_NONE;
                return true;
            default:
                spdlog::error("Unsupported old texture usage for layout transition.");
                return false;
            }
        }

        bool getDstScope(TextureUsage newUsage, VkAccessFlags2 &dstAccessMask, VkPipelineStageFlags2 &dstStageMask)
        {
            // Attachments are read too, by a pass that loads what an earlier one drew.
            switch (newUsage)
            {
            case TextureUsage::COLOR_ATTACHMENT:
                dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::PRESENT_SRC:
                dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                dstAccessMask = VK_ACCESS_2_NONE;
                return true;
            case TextureUsage::DEPTH_STENCIL_ATTACHMENT:
                dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT_KHR | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR;
                dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                return true;
            case TextureUsage::SAMPLED:
                dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                return true;
            case TextureUsage::STORAGE:
                // Compute writes storage images, and reads them too (one mip from the one before it).
                dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                return true;
            default:
                spdlog::error("Unsupported new texture usage for layout transition.");
                return false;
            }
        }

        VkPipelineStageFlags2 toVkStageMask(PipelineStage stages)
        {
            VkPipelineStageFlags2 mask = VK_PIPELINE_STAGE_2_NONE;
            if (hasStage(stages, PipelineStage::COMPUTE))
                mask |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            if (hasStage(stages, PipelineStage::GRAPHICS))
                mask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            if (hasStage(stages, PipelineStage::HOST))
                mask |= VK_PIPELINE_STAGE_2_HOST_BIT;
            return mask;
        }

        // Every way a stage can read a buffer: device addresses count as storage access, and graphics
        // also reads indirect commands, indices and uniforms.
        VkAccessFlags2 toVkReadMask(PipelineStage stages)
        {
            VkAccessFlags2 mask = VK_ACCESS_2_NONE;
            if (hasStage(stages, PipelineStage::COMPUTE))
                mask |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
            if (hasStage(stages, PipelineStage::GRAPHICS))
                mask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
            if (hasStage(stages, PipelineStage::HOST))
                mask |= VK_ACCESS_2_HOST_READ_BIT;
            return mask;
        }

        // Shaders write buffers only through storage access, and the host through host writes.
        VkAccessFlags2 toVkWriteMask(PipelineStage stages)
        {
            VkAccessFlags2 mask = VK_ACCESS_2_NONE;
            if (hasStage(stages, PipelineStage::COMPUTE) || hasStage(stages, PipelineStage::GRAPHICS))
                mask |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            if (hasStage(stages, PipelineStage::HOST))
                mask |= VK_ACCESS_2_HOST_WRITE_BIT;
            return mask;
        }

        constexpr VkPipelineStageFlags2 COMPUTE_QUEUE_STAGES = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                                                               VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        constexpr VkAccessFlags2 GRAPHICS_ONLY_ACCESS = VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT |
                                                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        // A compute queue runs no graphics stages. Graphics work on the other queue is ordered by the semaphore
        // the submit waits on, which blocks all commands, so a source scope that named it waits for all commands
        // instead and chains to that wait. Graphics accesses have nothing to make available or visible here.
        void fitToComputeQueue(VkPipelineStageFlags2 &srcStage, VkAccessFlags2 &srcAccess, VkPipelineStageFlags2 &dstStage, VkAccessFlags2 &dstAccess)
        {
            if ((srcStage & ~COMPUTE_QUEUE_STAGES) != 0)
                srcStage = (srcStage & COMPUTE_QUEUE_STAGES) | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            dstStage &= COMPUTE_QUEUE_STAGES;
            if (dstStage == 0)
                dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            srcAccess &= ~GRAPHICS_ONLY_ACCESS;
            dstAccess &= ~GRAPHICS_ONLY_ACCESS;
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

    bool VulkanCommandBuffer::initialize(uint32_t queueFamilyIndex, bool onComputeQueue, const std::string &name)
    {
        computeQueue = onComputeQueue;
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
        if (computeQueue)
            fitToComputeQueue(srcStageMask, srcAccessMask, dstStageMask, dstAccessMask);

        VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                         .srcStageMask = srcStageMask,
                                         .srcAccessMask = srcAccessMask,
                                         .dstStageMask = dstStageMask,
                                         .dstAccessMask = dstAccessMask,
                                         .oldLayout = toVkImageLayout(oldUsage),
                                         .newLayout = toVkImageLayout(newUsage),
                                         .image = vkTexture->image,
                                         .subresourceRange = {
                                             .aspectMask = toVkAspectMask(texture->desc.format),
                                             .baseMipLevel = 0,
                                             .levelCount = vkTexture->mipLevels,
                                             .baseArrayLayer = 0,
                                             .layerCount = VK_REMAINING_ARRAY_LAYERS,
                                         }};

        VkDependencyInfo dependencyInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                           .imageMemoryBarrierCount = 1,
                                           .pImageMemoryBarriers = &barrier};

        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
        return true;
    }

    // Every color attachment shares the load op and the clear color. The depth attachment is optional; of a cube
    // depth, the pass draws only into face.
    void VulkanCommandBuffer::beginRendering(Texture *const *colorAttachments, uint32_t colorCount, Texture *depthAttachment, const glm::vec4 &clearColor, const AttachmentOps &ops, uint32_t face)
    {
        auto *depth = static_cast<VulkanTexture *>(depthAttachment);
        VkImageView depthView = VK_NULL_HANDLE;
        if (depth)
            depthView = depth->desc.cube ? depth->faceViews[face] : depth->view;
        // A depth-only pass takes its size from the depth.
        const auto *area = colorCount > 0 ? static_cast<VulkanTexture *>(colorAttachments[0]) : depth;

        VkRenderingAttachmentInfo colorAttachmentInfos[MAX_COLOR_ATTACHMENTS] = {};
        for (uint32_t i = 0; i < colorCount; ++i)
        {
            colorAttachmentInfos[i] = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = static_cast<VulkanTexture *>(colorAttachments[i])->view,
                .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                .loadOp = ops.loadColor ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue{.color{.float32{clearColor.r, clearColor.g, clearColor.b, clearColor.a}}}};
        }

        VkRenderingAttachmentInfo depthAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depthView,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = ops.loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
            // Stored only when a later pass loads or samples it (RenderGraph::assignAttachmentOps).
            .storeOp = ops.storeDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            // Reversed-Z: the far plane is depth 0. Pairs with the GREATER_OR_EQUAL compare in createGraphicsPipeline.
            .clearValue = {.depthStencil = {0.0f, 0}}};

        // Must match the pipeline's stencilAttachmentFormat (see createGraphicsPipeline).
        VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{
                .extent{
                    .width = area->desc.width,
                    .height = area->desc.height}},
            .layerCount = 1,
            .colorAttachmentCount = colorCount,
            .pColorAttachments = colorAttachmentInfos,
            .pDepthAttachment = depth ? &depthAttachmentInfo : nullptr,
            .pStencilAttachment = depth && hasStencil(depth->desc.format) ? &depthAttachmentInfo : nullptr};

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

    void VulkanCommandBuffer::drawIndexedIndirectCount(Buffer *buffer, uint64_t offset, Buffer *countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride)
    {
        vkCmdDrawIndexedIndirectCount(commandBuffer, static_cast<VulkanBuffer *>(buffer)->buffer, offset, static_cast<VulkanBuffer *>(countBuffer)->buffer, countOffset, maxDrawCount, stride);
    }

    void VulkanCommandBuffer::dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        vkCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
    }

    void VulkanCommandBuffer::memoryBarrier(PipelineStage before, PipelineStage after)
    {
        // Only writes need to be made available: reads before the barrier just have to finish, which the stage
        // mask covers. Writes after it must wait for the writes before it.
        VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                    .srcStageMask = toVkStageMask(before),
                                    .srcAccessMask = toVkWriteMask(before),
                                    .dstStageMask = toVkStageMask(after),
                                    .dstAccessMask = toVkReadMask(after) | toVkWriteMask(after)};
        if (computeQueue)
            fitToComputeQueue(barrier.srcStageMask, barrier.srcAccessMask, barrier.dstStageMask, barrier.dstAccessMask);
        VkDependencyInfo dependencyInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

    // Textures placed in the same memory: the next one's attachment writes wait for every use of the ones there before.
    // An image barrier covers only its own image, so this one is global. Reads before it only have to finish.
    void VulkanCommandBuffer::aliasingBarrier()
    {
        constexpr VkPipelineStageFlags2 ATTACHMENT_STAGES = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT_KHR |
                                                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT_KHR;
        VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                    .srcStageMask = ATTACHMENT_STAGES | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                    .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                    .dstStageMask = ATTACHMENT_STAGES,
                                    .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        if (computeQueue)
            fitToComputeQueue(barrier.srcStageMask, barrier.srcAccessMask, barrier.dstStageMask, barrier.dstAccessMask);
        VkDependencyInfo dependencyInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
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

    // Each texture goes to TRANSFER_DST, takes its copies and goes to SHADER_READ_ONLY, the layout its bindless
    // descriptor names. A texture whose first copy is here starts from UNDEFINED; one that an earlier batch began keeps
    // what that batch wrote. Earlier submissions on this queue are in the first barrier's scope and later ones in the
    // second's, as for copyBuffers.
    void VulkanCommandBuffer::copyToTextures(Buffer *src, const TextureCopy *copies, uint32_t copyCount)
    {
        std::vector<VulkanTexture *> textures;
        std::vector<VkImageMemoryBarrier2> barriers;
        for (uint32_t i = 0; i < copyCount; ++i)
        {
            auto *texture = static_cast<VulkanTexture *>(copies[i].dst);
            const auto it = std::find(textures.begin(), textures.end(), texture);
            if (it != textures.end())
            {
                if (copies[i].discard)
                    barriers[it - textures.begin()].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                continue;
            }
            textures.push_back(texture);
            barriers.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = copies[i].discard ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = texture->image,
                                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = texture->mipLevels, .baseArrayLayer = 0, .layerCount = texture->desc.cube ? CUBE_FACES : 1}});
        }
        VkDependencyInfo dependencyInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()), .pImageMemoryBarriers = barriers.data()};
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

        auto srcBuffer = static_cast<VulkanBuffer *>(src)->buffer;
        for (uint32_t i = 0; i < copyCount; ++i)
        {
            const auto &copy = copies[i];
            auto *texture = static_cast<VulkanTexture *>(copy.dst);
            const VkBufferImageCopy2 region = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                                               .bufferOffset = copy.srcOffset,
                                               .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = copy.mip, .baseArrayLayer = 0, .layerCount = 1},
                                               .imageOffset = {0, static_cast<int32_t>(copy.y), 0},
                                               .imageExtent = {std::max(1u, texture->desc.width >> copy.mip), copy.height, 1}};
            const VkCopyBufferToImageInfo2 info = {.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                                                   .srcBuffer = srcBuffer,
                                                   .dstImage = texture->image,
                                                   .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                   .regionCount = 1,
                                                   .pRegions = &region};
            vkCmdCopyBufferToImage2(commandBuffer, &info);
        }

        for (auto &barrier : barriers)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

    // Written once all earlier commands are done, so the time between two timestamps covers the commands
    // between them, without overlap from earlier work.
    void VulkanCommandBuffer::writeTimestamp(QueryPool *queryPool, uint32_t index)
    {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, static_cast<VulkanQueryPool *>(queryPool)->pool, index);
    }

    VkCommandBuffer VulkanCommandBuffer::getVkCommandBuffer() const
    {
        return commandBuffer;
    }
}
