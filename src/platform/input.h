// clang-format off
#pragma once

#include "rasm/camera.h"
#include "rasm/input.h"
#include "rasm/window.h"

#include <cstdint>

namespace rasm
{

    // What Dear ImGui took from the last UI frame. All false without a UI.
    struct UiInputCapture
    {
        bool        mouse           = false;    // the pointer is over a UI window, or drags one
        bool        keyboard        = false;    // a UI window has the keyboard focus
        bool        text            = false;    // a text field takes typing
    };

    // The input the engine keeps between events: the keys held, the pointer, the camera drag, and what one frame's
    // events asked for. Every platform's events, and the application's, go through it.
    struct InputState
    {
        bool        keysDown[static_cast<uint32_t>(Key::COUNT)] = {};  // by position (InputEvent::physicalKey)
        float       mouseX          = 0.0f;     // the pointer, in window points
        float       mouseY          = 0.0f;
        bool        mouseInside     = false;    // a move or a button event put the pointer in the window, and it has not left
        bool        dragActive      = false;    // a button went down outside the UI and is not up yet
        glm::vec2   lastMousePos    = glm::vec2(0.0f);  // NDC, where the last frame's drag ended
        glm::vec2   dragStart       = glm::vec2(0.0f);  // NDC, this frame's
        glm::vec2   dragEnd         = glm::vec2(0.0f);
        float       wheel           = 0.0f;     // this frame's steps
        uint32_t    statsToggles    = 0;        // F1 presses this frame
        uint32_t    forcedLodSteps  = 0;        // F2
        uint32_t    escapes         = 0;        // Escape
        bool        dumpGraph       = false;    // F3
    };

    // Window points to the camera's NDC: both axes divided by the smaller window side, so the trackball stays round
    // on a wide window, and y up.
    glm::vec2       toInputNdc(float x, float y, const Window& window);
    // Starts a frame's events: no wheel and no key presses yet, and the drag starts where the last one ended.
    void            beginInputFrame(InputState& state);
    void            applyInputEvent(InputState& state, const InputEvent& event, const Window& window, const UiInputCapture& capture);
    // The frame's camera input, without what the UI took.
    CameraInput     endInputFrame(InputState& state, const UiInputCapture& capture, float deltaTime);

}
// clang-format on
