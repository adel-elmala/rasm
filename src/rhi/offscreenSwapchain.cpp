#include "rhi/offscreenSwapchain.h"

#include "spdlog/spdlog.h"

#include <string>

namespace rasm::rhi
{
    OffscreenSwapchain::OffscreenSwapchain(Device &device) : device(device) {}

    OffscreenSwapchain::~OffscreenSwapchain()
    {
        device.waitIdle();
        destroyImages();
    }

    bool OffscreenSwapchain::initialize(const SwapchainDesc &desc)
    {
        return createImages(desc.width, desc.height);
    }

    // Readable, so Renderer::readFrame can copy any frame, and with a UNORM view, which the UI draws through as on a window.
    bool OffscreenSwapchain::createImages(uint32_t width, uint32_t height)
    {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            images[i] = device.createTexture({.name = "Offscreen Swapchain Image " + std::to_string(i),
                                              .width = width,
                                              .height = height,
                                              .format = FORMAT,
                                              .usage = TextureUsage::COLOR_ATTACHMENT,
                                              .readable = true,
                                              .encodedView = true});
            if (images[i] == nullptr)
            {
                spdlog::error("Failed to create the offscreen swapchain's images ({} x {}).", width, height);
                destroyImages();
                return false;
            }
            lastUse[i] = {};
        }
        extent = {width, height};
        return true;
    }

    void OffscreenSwapchain::destroyImages()
    {
        for (auto &image : images)
        {
            if (image != nullptr)
                device.destroyTexture(image);
            image = nullptr;
        }
        extent = glm::uvec2(0);
    }

    // Normally the image's last submit is long done, since the engine waited for that frame slot before it acquired.
    SwapchainStatus OffscreenSwapchain::acquireNextImage(uint32_t &imageIndex)
    {
        if (images[next] == nullptr)
            return SwapchainStatus::OUT_OF_DATE;
        const FenceWait &use = lastUse[next];
        if (use.fence != nullptr && !device.waitForFence(use.fence, use.value, UINT64_MAX))
            return SwapchainStatus::FAILURE;
        imageIndex = acquired = next;
        next = (next + 1) % MAX_FRAMES_IN_FLIGHT;
        return SwapchainStatus::OK;
    }

    SwapchainStatus OffscreenSwapchain::present()
    {
        return SwapchainStatus::OK;
    }

    void OffscreenSwapchain::submitted(Fence *fence, uint64_t value)
    {
        lastUse[acquired] = {.fence = fence, .value = value};
    }

    SwapchainStatus OffscreenSwapchain::resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return SwapchainStatus::OUT_OF_DATE;
        device.waitIdle();
        destroyImages();
        next = 0;
        return createImages(width, height) ? SwapchainStatus::OK : SwapchainStatus::FAILURE;
    }

    uint32_t OffscreenSwapchain::getImageCount() const
    {
        return images[0] != nullptr ? MAX_FRAMES_IN_FLIGHT : 0;
    }

    Texture *OffscreenSwapchain::getImage(uint32_t index)
    {
        return index < MAX_FRAMES_IN_FLIGHT ? images[index] : nullptr;
    }

    Format OffscreenSwapchain::getFormat() const
    {
        return FORMAT;
    }

    Format OffscreenSwapchain::getEncodedFormat() const
    {
        return Format::R8G8B8A8_UNORM;
    }

    glm::uvec2 OffscreenSwapchain::getExtent() const
    {
        return extent;
    }
}
