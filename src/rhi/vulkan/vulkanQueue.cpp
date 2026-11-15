#include "rhi/vulkan/vulkanQueue.h"
#include "rhi/vulkan/vulkanCommandBuffer.h"
#include "rhi/vulkan/vulkanSwapchain.h"
#include "rhi/vulkan/vulkanUtils.h"

#include "spdlog/spdlog.h"

namespace rasm::rhi
{
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

        if (desc.waitCount > MAX_SUBMIT_WAITS)
        {
            spdlog::error("A submit waits for at most {} fences, got {}.", MAX_SUBMIT_WAITS, desc.waitCount);
            return false;
        }

        // The fence waits block every stage: the first commands of the submit are often barriers whose layout
        // transitions must come after them.
        VkSemaphoreSubmitInfo waits[MAX_SUBMIT_WAITS + 1] = {};
        VkSemaphoreSubmitInfo signals[3] = {};
        uint32_t waitCount = 0;
        uint32_t signalCount = 0;
        for (uint32_t i = 0; i < desc.waitCount; ++i)
        {
            waits[waitCount++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = static_cast<VulkanFence *>(desc.waits[i].fence)->semaphore,
                                  .value = desc.waits[i].value,
                                  .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        }

        auto *acquireSwapchain = static_cast<VulkanSwapchain *>(desc.acquireSwapchain);
        uint32_t acquireWaitCount = 0;
        if (acquireSwapchain != nullptr)
        {
            if (!acquireSwapchain->getAcquireWait(waits[waitCount], acquireWaitCount))
                return false;
            waitCount += acquireWaitCount;
        }

        auto *presentSwapchain = static_cast<VulkanSwapchain *>(desc.presentSwapchain);
        if (presentSwapchain != nullptr)
        {
            if (!presentSwapchain->getPresentSignals(signals[signalCount], signals[signalCount + 1]))
                return false;
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
        if (acquireWaitCount > 0)
            acquireSwapchain->onAcquireWaited();
        if (presentSwapchain != nullptr)
            presentSwapchain->onSubmitted();
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
