// clang-format off
#pragma once

#include "rasm/window.h"

namespace rasm
{

    struct UiInputCapture
    {
        bool        mouse       = false;    // the pointer is over a UI window, or drags one
        bool        keyboard    = false;    // a UI window has the keyboard focus
        bool        text        = false;    // a text field takes typing
    };

    bool            initUiPlatform(const Window& window);
    void            shutdownUiPlatform(const Window& window);
    void            processUiEvent(const void* event);
    void            newUiPlatformFrame(const Window& window, float deltaTime);
    void            resumeUiInput(const Window& window);
    float           getUiContentScale(const Window& window);
    UiInputCapture  getUiInputCapture();

}
// clang-format on
