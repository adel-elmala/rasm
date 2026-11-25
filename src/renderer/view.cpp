#include "rasm/view.h"

#include <algorithm>
#include <cmath>

namespace rasm
{
    namespace
    {
        // An edge at a fraction of size, rounded to a pixel and kept inside the image.
        uint32_t toEdge(float fraction, uint32_t size)
        {
            if (!(fraction > 0.0f))
                return 0;
            return static_cast<uint32_t>(std::min(std::round(fraction * static_cast<float>(size)), static_cast<float>(size)));
        }
    }

    ViewPixels getViewPixels(const ViewRect &rect, uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return {};
        const uint32_t x0 = std::min(toEdge(rect.x, width), width - 1);
        const uint32_t y0 = std::min(toEdge(rect.y, height), height - 1);
        const uint32_t x1 = std::max(toEdge(rect.x + rect.width, width), x0 + 1);
        const uint32_t y1 = std::max(toEdge(rect.y + rect.height, height), y0 + 1);
        return {.x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0};
    }

    bool getViewPixel(const Window &window, const ViewRect &rect, float x, float y, uint32_t &pixelX, uint32_t &pixelY)
    {
        if (window.width == 0 || window.height == 0 || !std::isfinite(x) || !std::isfinite(y))
            return false;
        const ViewPixels view = getViewPixels(rect, window.pixelWidth, window.pixelHeight);
        // Points to the window's pixels: they differ on a HiDPI display.
        const double px = static_cast<double>(x) * window.pixelWidth / window.width;
        const double py = static_cast<double>(y) * window.pixelHeight / window.height;
        if (px < view.x || py < view.y || px >= static_cast<double>(view.x) + view.width || py >= static_cast<double>(view.y) + view.height)
            return false;
        pixelX = static_cast<uint32_t>(px) - view.x;
        pixelY = static_cast<uint32_t>(py) - view.y;
        return true;
    }

    InputArea getInputArea(const Window &window, const ViewRect &rect)
    {
        const float width = static_cast<float>(window.width);
        const float height = static_cast<float>(window.height);
        return {.x = rect.x * width, .y = rect.y * height, .width = rect.width * width, .height = rect.height * height};
    }
}
