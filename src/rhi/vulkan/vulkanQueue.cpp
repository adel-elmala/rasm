#include "rhi/vulkan/vulkanQueue.h"
#include "rhi/vulkan/vulkanCommandBuffer.h"
#include "rhi/vulkan/vulkanSwapchain.h"
#include "rhi/vulkan/vulkanUtils.h"

#include "spdlog/spdlog.h"

namespace rasm::rhi
{
    namespace
    {
        constexpr uint32_t MAX_SUBMIT_COMMAND_BUFFERS = 8;
    }

    VulkanQueue::VulkanQueue(VkQueue queue, uint32_t familyIndex) : queue(queue), familyIndex(familyIndex) {}

    bool VulkanQueue::submit(const SubmitDesc &desc)
    {
        if (desc.commandBufferCount > MAX_SUBMIT_COMMAND_BUFFERS)
        {
            spdlog::error("A submit takes at most {} command buffers, got {}.", MAX_SUBMIT_COMMAND_BUFFERS, desc.commandBufferCount);
            return false;
        }

        VkCommandBufferSubmitInfo submitInfos[MAX_SUBMIT_COMMAND_BUFFERS] = {};
        const uint32_t commandBufferCount = desc.commandBufferCount;
        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            submitInfos[i] = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                              .commandBuffer = static_cast<VulkanCommandBuffer *>(desc.commandBuffers[i])->getVkCommandBuffer()};
        }

        VkSemaphoreSubmitInfo waits[1] = {};
        VkSemaphoreSubmitInfo signals[3] = {};
        uint32_t waitCount = 0;
        uint32_t signalCount = 0;

        auto *swapchain = static_cast<VulkanSwapchain *>(desc.swapchain);
        if (swapchain != nullptr)
        {
            if (!swapchain->getSubmitSemaphores(waits[waitCount], signals[signalCount], signals[signalCount + 1]))
                return false;
            waitCount += 1;
            signalCount += 2;
        }

        if (desc.signalFence != nullptr)
        {
            signals[signalCount++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                      .semaphore = static_cast<VulkanFence *>(desc.signalFence)->semaphore,
                                      .value = desc.signalValue,
                                      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        }

        VkSubmitInfo2 submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = waitCount,
            .pWaitSemaphoreInfos = waits,
            .commandBufferInfoCount = commandBufferCount,
            .pCommandBufferInfos = submitInfos,
            .signalSemaphoreInfoCount = signalCount,
            .pSignalSemaphoreInfos = signals};

        // A failed submit leaves every semaphore as it was, so the swapchain only moves on after a success.
        auto result = vkQueueSubmit2(queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
        {
            spdlog::error("Failed to submit command buffer. Error: {}", toVkResultString(result));
            return false;
        }
        if (swapchain != nullptr)
            swapchain->onSubmitted();
        return true;
    }

    VkQueue VulkanQueue::getVkQueue() const
    {
        return queue;
    }

    uint32_t VulkanQueue::getFamilyIndex() const
    {
        return familyIndex;
    }
}
