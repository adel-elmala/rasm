// clang-format off
#pragma once

#include "rhi/device.h"

namespace rasm::rhi
{

    // The swapchain of a headless engine: MAX_FRAMES_IN_FLIGHT textures of the device that the frames render into, in
    // turn. Presenting shows nothing. An image is handed out again once the last submit that drew into it is done,
    // which RenderContext reports through submitted.
    class OffscreenSwapchain final : public Swapchain
    {
    public:
                                    OffscreenSwapchain(Device& device);
                                    ~OffscreenSwapchain() override;
        bool                        initialize(const SwapchainDesc& desc);
        SwapchainStatus             acquireNextImage(uint32_t& imageIndex) override;
        SwapchainStatus             present() override;
        SwapchainStatus             resize(uint32_t width, uint32_t height) override;
        uint32_t                    getImageCount() const override;
        Texture*                    getImage(uint32_t index) override;
        Format                      getFormat() const override;
        Format                      getEncodedFormat() const override;
        glm::uvec2                  getExtent() const override;
        void                        submitted(Fence* fence, uint64_t value);

        static constexpr Format     FORMAT              = Format::R8G8B8A8_SRGB;

    private:
        bool                        createImages(uint32_t width, uint32_t height);
        void                        destroyImages();

        Device&                     device;
        Texture*                    images[MAX_FRAMES_IN_FLIGHT]        = {};
        FenceWait                   lastUse[MAX_FRAMES_IN_FLIGHT]       = {};   // the submit that last drew into each image
        uint32_t                    acquired                            = 0;
        uint32_t                    next                                = 0;
        glm::uvec2                  extent                              = glm::uvec2(0);
    };

}
// clang-format on
