// clang-format off
#pragma once

#include <cstdint>

#include "rasm/types.h"
#include "rasm/handle.h"

namespace rasm
{

    // What Engine::addWindow opens or wraps, by platform: SDL opens a window of width x height points with the
    // title; NATIVE draws into native, the application's window; HEADLESS renders into an offscreen image of width x
    // height pixels.
    struct WindowDesc
    {
        const char*     title           = "RASM";       // SDL
        uint32_t        width           = 0;            // points; HEADLESS: pixels; NATIVE: used when native has no pixel size
        uint32_t        height          = 0;
        NativeWindow    native          = {};           // NATIVE only
        bool            hidden          = false;        // SDL: opened hidden, as a script or a test does
    };

    struct Window
    {
        WindowHandle    handle;                         // the engine's handle of the window; getMainWindow's is the main one
        void*           platformWindow  = nullptr;      // the SDL platform's SDL_Window
        NativeWindow    native          = {};           // what the surface is made from; type NONE headless
        uint32_t        width           = 0;            // logical size in points, matches mouse coordinates
        uint32_t        height          = 0;
        uint32_t        pixelWidth      = 0;            // drawable size in pixels, differs from width/height on HiDPI displays
        uint32_t        pixelHeight     = 0;
        float           contentScale    = 1.0f;         // the display's scale setting, without the pixel density: the UI's scale
        bool            resized         = false;
        bool            minimized       = false;
    };

}
// clang-format on
