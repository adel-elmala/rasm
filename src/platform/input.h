// clang-format off
#pragma once

#include "rasm/cameraInput.h"
#include "rasm/input.h"

#include <cstdint>

namespace rasm
{

    // The engine's input for the main window: the camera's, and what one frame's events asked of the engine itself.
    // Every platform's events, and the application's, go through it.
    struct InputState
    {
        CameraInputState    camera          = {};
        uint32_t            statsToggles    = 0;        // F1 presses this frame
        uint32_t            forcedLodSteps  = 0;        // F2
        uint32_t            escapes         = 0;        // Escape
        bool                dumpGraph       = false;    // F3
    };

    // Starts a frame's events: no key presses yet.
    void            beginInputFrame(InputState& state);
    void            applyInputEvent(InputState& state, const InputEvent& event, const InputArea& area, const UiInputCapture& capture);

}
// clang-format on
