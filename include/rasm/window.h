// clang-format off
#pragma once

#include <cstdint>

#include "rasm/types.h"
#include "rasm/handle.h"

namespace rasm
{

    struct Window
    {
        WindowHandle    handle;                         // the window the SDL platform opened; invalid for NATIVE and HEADLESS
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
