// clang-format off
#pragma once

#include "rhi/device.h"
#include "rhi/vulkan/vulkanTypes.h"

#include "VkBootstrap.h"

#include <vector>

namespace rasm::rhi
{
    class VulkanDevice;

    class VulkanSwapchain final : public Swapchain
    {
    public:
                                    VulkanSwapchain(VulkanDevice& device);
                                    ~VulkanSwapchain() override;
        bool                        initialize(const SwapchainDesc& desc);
        SwapchainStatus             acquireNextImage(uint32_t& outImageIndex) override;
        SwapchainStatus             present() override;
        SwapchainStatus             resize(uint32_t width, uint32_t height) override;
        uint32_t                    getImageCount() const override;
        Texture*                    getImage(uint32_t index) override;
        Format                      getFormat() const override;
        glm::uvec2                  getExtent() const override;
        bool                        getSubmitSemaphores(VkSemaphoreSubmitInfo& acquireWait, VkSemaphoreSubmitInfo& presentSignal, VkSemaphoreSubmitInfo& releaseSignal) const;
        void                        onSubmitted();

    private:
        static constexpr uint32_t   NO_ACQUIRE                                  = UINT32_MAX;

        bool                        createImages();
        void                        destroyImages();
        void                        destroySwapchain();
        void                        releasePendingSemaphores();

        VulkanDevice&               device;
        vkb::Swapchain              swapchain                                   = {};
        std::vector<VulkanTexture>  images                                      = {};
        std::vector<VkSemaphore>    presentSemaphores                           = {};
        VkSemaphore                 acquireSemaphores[MAX_FRAMES_IN_FLIGHT]     = {};
        uint64_t                    acquireReleaseValues[MAX_FRAMES_IN_FLIGHT]  = {};
        VkSemaphore                 releaseTimeline                             = VK_NULL_HANDLE;
        uint64_t                    releaseValue                                = 0;
        uint32_t                    nextAcquireSlot                             = 0;
        uint32_t                    acquiredSlot                                = NO_ACQUIRE;
        bool                        presentPending                              = false;
        uint32_t                    imageIndex                                  = 0;
    };
}
// clang-format on
