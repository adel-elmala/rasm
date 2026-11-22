// clang-format off
#pragma once

#include "rasm/camera.h"
#include "rasm/window.h"

#include <memory>

namespace rasm
{

    // What a platform saw since the last poll, for the engine to apply. The camera input has no deltaTime: the engine
    // measures the frame time itself.
    struct PlatformEvents
    {
        bool                        quit                = false;    // the window closed, or Escape
        bool                        toggleStats         = false;    // F1
        bool                        nextForcedLod       = false;    // F2
        bool                        dumpGraph           = false;    // F3
        CameraInput                 camera              = {};
    };

    // The window and the input of an engine. SDL opens a window and reads its events; headless has neither, and the
    // frames go to the offscreen swapchain of the window's size.
    class Platform
    {
    public:
        virtual                     ~Platform() = default;
        // A headless window has no handle, and its pixel size is the size asked for.
        virtual Window              createWindow(uint32_t width, uint32_t height) = 0;
        virtual void                destroyWindow(const Window& window) = 0;
        // Updates the window's size, minimized and mouse state. With uiActive the events also go to Dear ImGui, and what
        // the UI captures is left out of the camera input.
        virtual void                pollEvents(Window& window, bool uiActive, PlatformEvents& events) = 0;
    };

    std::unique_ptr<Platform>       createSdlPlatform();
    std::unique_ptr<Platform>       createHeadlessPlatform();

}
// clang-format on
