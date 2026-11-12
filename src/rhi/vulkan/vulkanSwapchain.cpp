#include "rhi/vulkan/vulkanSwapchain.h"
#include "rhi/vulkan/vulkanDevice.h"
#include "rhi/vulkan/vulkanUtils.h"

#include "spdlog/spdlog.h"

#include <string>

namespace rasm::rhi
{
    namespace
    {
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

        // FIFO, because with NVIDIA's overlay on, the other present modes crash vkDestroyDevice when the window fills the screen (B21).
        // The extent is only used when the surface leaves it to the swapchain (currentExtent == 0xFFFFFFFF, e.g. Wayland).
        vkb::Result<vkb::Swapchain> buildSwapchain(const vkb::Device &device, uint32_t width, uint32_t height, const vkb::Swapchain *oldSwapchain)
        {
            vkb::SwapchainBuilder swapchainBuilder{device};
            swapchainBuilder.set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                .set_desired_extent(width, height);
            if (oldSwapchain != nullptr)
                swapchainBuilder.set_old_swapchain(*oldSwapchain);

            auto swapRet = swapchainBuilder.build();
            if (!swapRet)
                spdlog::error("Failed to create swapchain. Error: {}", swapRet.error().message());
            return swapRet;
        }

        VkSemaphore createSemaphore(VulkanDevice &device, VkSemaphoreType type, const std::string &name)
        {
            VkSemaphoreTypeCreateInfo typeInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = type, .initialValue = 0};
            VkSemaphoreCreateInfo semaphoreInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &typeInfo};
            VkSemaphore semaphore = VK_NULL_HANDLE;
            auto result = vkCreateSemaphore(device.getVkDevice(), &semaphoreInfo, nullptr, &semaphore);
            if (result != VK_SUCCESS)
            {
                spdlog::error("Failed to create semaphore. Error: {}", toVkResultString(result));
                return VK_NULL_HANDLE;
            }
            device.setObjectName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(semaphore), name);
            return semaphore;
        }
    }

    VulkanSwapchain::VulkanSwapchain(VulkanDevice &device) : device(device) {}

    VulkanSwapchain::~VulkanSwapchain()
    {
        releasePendingSemaphores();
        device.waitIdle();
        destroySwapchain();

        for (auto semaphore : presentSemaphores)
            vkDestroySemaphore(device.getVkDevice(), semaphore, nullptr);
        for (auto semaphore : acquireSemaphores)
        {
            if (semaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device.getVkDevice(), semaphore, nullptr);
        }
        if (releaseTimeline != VK_NULL_HANDLE)
            vkDestroySemaphore(device.getVkDevice(), releaseTimeline, nullptr);
    }

    bool VulkanSwapchain::initialize(const SwapchainDesc &desc)
    {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            acquireSemaphores[i] = createSemaphore(device, VK_SEMAPHORE_TYPE_BINARY, "Swapchain Acquire " + std::to_string(i));
            if (acquireSemaphores[i] == VK_NULL_HANDLE)
                return false;
        }
        releaseTimeline = createSemaphore(device, VK_SEMAPHORE_TYPE_TIMELINE, "Swapchain Release");
        if (releaseTimeline == VK_NULL_HANDLE)
            return false;

        auto swapRet = buildSwapchain(device.getVkbDevice(), desc.width, desc.height, nullptr);
        if (!swapRet)
            return false;

        swapchain = swapRet.value();
        return createImages();
    }

    bool VulkanSwapchain::createImages()
    {
        auto imagesResult = swapchain.get_images();
        if (!imagesResult)
        {
            spdlog::error("Failed to get swapchain images: {}", imagesResult.error().message());
            return false;
        }
        auto viewsResult = swapchain.get_image_views();
        if (!viewsResult)
        {
            spdlog::error("Failed to get swapchain image views: {}", viewsResult.error().message());
            return false;
        }
        const auto &vkImages = imagesResult.value();
        const auto &vkViews = viewsResult.value();

        images.resize(vkImages.size());
        for (size_t i = 0; i < vkImages.size(); ++i)
        {
            auto &image = images[i];
            image.image = vkImages[i];
            image.view = vkViews[i];
            image.desc.name = "Swapchain Image " + std::to_string(i);
            image.desc.width = swapchain.extent.width;
            image.desc.height = swapchain.extent.height;
            image.desc.format = toFormat(swapchain.image_format);
            image.desc.usage = TextureUsage::COLOR_ATTACHMENT;
            device.setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(image.view), image.desc.name);
        }

        // One per image rather than per frame: an image is only acquired again after its present finished, so
        // its semaphore is free by then. They outlive a resize, because a present can still be waiting on one
        // after the device is idle; only images beyond the old count need a new semaphore.
        for (size_t i = presentSemaphores.size(); i < images.size(); ++i)
        {
            auto semaphore = createSemaphore(device, VK_SEMAPHORE_TYPE_BINARY, "Swapchain Present " + std::to_string(i));
            if (semaphore == VK_NULL_HANDLE)
                return false;
            presentSemaphores.push_back(semaphore);
        }
        return true;
    }

    void VulkanSwapchain::destroyImages()
    {
        for (const auto &image : images)
        {
            if (image.view != VK_NULL_HANDLE)
                vkDestroyImageView(device.getVkDevice(), image.view, nullptr);
        }
        images.clear();
    }

    void VulkanSwapchain::destroySwapchain()
    {
        destroyImages();
        if (swapchain.swapchain != VK_NULL_HANDLE)
            vkb::destroy_swapchain(swapchain);
        swapchain = {};
    }

    // A binary semaphore that is signaled but never waited on can't be signaled again. When the swapchain is
    // replaced between acquire and present, a wait-only submit consumes those signals; the caller then idles the device.
    void VulkanSwapchain::releasePendingSemaphores()
    {
        VkSemaphoreSubmitInfo waits[2] = {};
        uint32_t waitCount = 0;
        if (acquiredSlot != NO_ACQUIRE)
        {
            waits[waitCount++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = acquireSemaphores[acquiredSlot],
                                  .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        }
        if (presentPending)
        {
            waits[waitCount++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = presentSemaphores[imageIndex],
                                  .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        }
        acquiredSlot = NO_ACQUIRE;
        presentPending = false;
        if (waitCount == 0)
            return;

        VkSubmitInfo2 submitInfo = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = waitCount, .pWaitSemaphoreInfos = waits};
        auto result = vkQueueSubmit2(device.getGraphicsQueue()->getVkQueue(), 1, &submitInfo, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
            spdlog::error("Failed to release the swapchain semaphores. Error: {}", toVkResultString(result));
    }

    SwapchainStatus VulkanSwapchain::acquireNextImage(uint32_t &outImageIndex)
    {
        if (swapchain.swapchain == VK_NULL_HANDLE)
            return SwapchainStatus::OUT_OF_DATE;

        // The last acquired image was never submitted, e.g. because the submit failed. It is still ours and its
        // semaphore is still signaled, so hand it out again rather than acquiring another.
        if (acquiredSlot != NO_ACQUIRE)
        {
            outImageIndex = imageIndex;
            return SwapchainStatus::OK;
        }
        if (presentPending)
        {
            spdlog::error("The last swapchain image was submitted but never presented.");
            return SwapchainStatus::FAILURE;
        }

        // An acquire semaphore is free again once the submit that waited on it has finished. Tracking that here
        // keeps the swapchain correct whatever cadence the caller acquires at.
        const uint32_t slot = nextAcquireSlot;
        if (acquireReleaseValues[slot] > 0)
        {
            VkSemaphoreWaitInfo waitInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                            .semaphoreCount = 1,
                                            .pSemaphores = &releaseTimeline,
                                            .pValues = &acquireReleaseValues[slot]};
            auto waitResult = vkWaitSemaphores(device.getVkDevice(), &waitInfo, UINT64_MAX);
            if (waitResult != VK_SUCCESS)
            {
                spdlog::error("Failed to wait for a swapchain acquire semaphore. Error: {}", toVkResultString(waitResult));
                return SwapchainStatus::FAILURE;
            }
        }

        uint32_t acquiredIndex = 0;
        auto result = vkAcquireNextImageKHR(device.getVkDevice(), swapchain.swapchain, UINT64_MAX, acquireSemaphores[slot], VK_NULL_HANDLE, &acquiredIndex);
        auto status = toSwapchainStatus(result);
        if (status == SwapchainStatus::FAILURE)
            spdlog::error("Failed to acquire next image. Error: {}", toVkResultString(result));

        // On OUT_OF_DATE the semaphore was not signaled, so it stays free for the next attempt.
        if (status == SwapchainStatus::OK || status == SwapchainStatus::SUBOPTIMAL)
        {
            acquiredSlot = slot;
            nextAcquireSlot = (slot + 1) % MAX_FRAMES_IN_FLIGHT;
            imageIndex = acquiredIndex;
            outImageIndex = acquiredIndex;
        }
        return status;
    }

    bool VulkanSwapchain::getSubmitSemaphores(VkSemaphoreSubmitInfo &acquireWait, VkSemaphoreSubmitInfo &presentSignal, VkSemaphoreSubmitInfo &releaseSignal) const
    {
        if (acquiredSlot == NO_ACQUIRE)
        {
            spdlog::error("A submit that presents needs a swapchain image acquired since the last such submit.");
            return false;
        }

        // Writing the image starts at color output, which the barrier out of UNKNOWN also waits for, so the
        // layout transition happens after the acquire.
        acquireWait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                       .semaphore = acquireSemaphores[acquiredSlot],
                       .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
        presentSignal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                         .semaphore = presentSemaphores[imageIndex],
                         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        releaseSignal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                         .semaphore = releaseTimeline,
                         .value = releaseValue + 1,
                         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        return true;
    }

    void VulkanSwapchain::onSubmitted()
    {
        ++releaseValue;
        acquireReleaseValues[acquiredSlot] = releaseValue;
        acquiredSlot = NO_ACQUIRE;
        presentPending = true;
    }

    SwapchainStatus VulkanSwapchain::present()
    {
        if (swapchain.swapchain == VK_NULL_HANDLE)
            return SwapchainStatus::OUT_OF_DATE;
        if (!presentPending)
        {
            spdlog::error("Present needs a successful submit that names the swapchain.");
            return SwapchainStatus::FAILURE;
        }

        // Even a present the engine rejects with OUT_OF_DATE still waits on the semaphore.
        presentPending = false;

        VkPresentInfoKHR presentInfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &presentSemaphores[imageIndex],
            .swapchainCount = 1,
            .pSwapchains = &swapchain.swapchain,
            .pImageIndices = &imageIndex};

        auto result = vkQueuePresentKHR(device.getGraphicsQueue()->getVkQueue(), &presentInfo);
        auto status = toSwapchainStatus(result);
        if (status == SwapchainStatus::FAILURE)
            spdlog::error("Failed to present swapchain image. Error: {}", toVkResultString(result));
        return status;
    }

    SwapchainStatus VulkanSwapchain::resize(uint32_t width, uint32_t height)
    {
        // A minimized window has a 0x0 surface, and no swapchain can be created for it. Keep the
        // old swapchain until the surface has pixels again (B5). Both early returns leave it untouched.
        VkSurfaceCapabilitiesKHR capabilities{};
        auto capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.getVkPhysicalDevice(), device.getSurface(), &capabilities);
        if (capsResult != VK_SUCCESS)
        {
            spdlog::error("Failed to query surface capabilities. Error: {}", toVkResultString(capsResult));
            return SwapchainStatus::FAILURE;
        }
        if (capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0)
            return SwapchainStatus::OUT_OF_DATE;

        releasePendingSemaphores();
        device.waitIdle();

        destroyImages();
        auto swapRet = buildSwapchain(device.getVkbDevice(), width, height, swapchain.swapchain != VK_NULL_HANDLE ? &swapchain : nullptr);

        // Passing it as oldSwapchain retired the old swapchain, even if creation failed, so it is destroyed
        // either way. Without a swapchain, acquire reports OUT_OF_DATE so the caller keeps trying to recreate it.
        if (swapchain.swapchain != VK_NULL_HANDLE)
            vkb::destroy_swapchain(swapchain);
        swapchain = {};
        if (!swapRet)
            return SwapchainStatus::FAILURE;

        swapchain = swapRet.value();
        if (!createImages())
        {
            destroySwapchain();
            return SwapchainStatus::FAILURE;
        }
        return SwapchainStatus::OK;
    }

    uint32_t VulkanSwapchain::getImageCount() const
    {
        return static_cast<uint32_t>(images.size());
    }

    Texture *VulkanSwapchain::getImage(uint32_t index)
    {
        return &images[index];
    }

    Format VulkanSwapchain::getFormat() const
    {
        return toFormat(swapchain.image_format);
    }

    glm::uvec2 VulkanSwapchain::getExtent() const
    {
        return {swapchain.extent.width, swapchain.extent.height};
    }
}
