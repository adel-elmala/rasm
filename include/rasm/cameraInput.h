// clang-format off
#pragma once

#include "rasm/cameraController.h"
#include "rasm/input.h"

#include <cstdint>

namespace rasm
{

    // The rectangle a camera's input belongs to, in window points like InputEvent mouse positions: the whole window,
    // or the part one view covers.
    struct InputArea
    {
        float       x               = 0.0f;
        float       y               = 0.0f;
        float       width           = 0.0f;
        float       height          = 0.0f;
    };

    // What one camera's input keeps between events: the keys held, the pointer and the drag. Feed it a frame's events
    // with addCameraInputEvent, then take the frame's CameraInput with takeCameraInput.
    struct CameraInputState
    {
        bool        keysDown[static_cast<uint32_t>(Key::COUNT)] = {};  // by position (InputEvent::physicalKey)
        float       pointerX        = 0.0f;     // the pointer, in window points
        float       pointerY        = 0.0f;
        bool        pointerInWindow = false;    // a move or a button event put the pointer in the window, and it has not left
        bool        dragActive      = false;    // a button went down in the area, outside the UI, and is not up yet
        glm::vec2   dragStart       = glm::vec2(0.0f);  // NDC, this frame's
        glm::vec2   dragEnd         = glm::vec2(0.0f);
        float       wheel           = 0.0f;     // this frame's steps, with the pointer in the area
    };

    // Window points to the camera's NDC: both axes divided by the area's smaller side, so the trackball stays round
    // on a wide area, the area's center at 0, and y up.
    glm::vec2       toInputNdc(float x, float y, const InputArea& area);
    // A press in the area starts a drag, which goes on outside it until the release; the wheel counts with the pointer
    // in the area. capture is what the UI took (Engine::getUiInputCapture), and keeps presses and the wheel from it.
    void            addCameraInputEvent(CameraInputState& state, const InputEvent& event, const InputArea& area, const UiInputCapture& capture);
    // The frame's input without what the UI took, W/A/S/D held by position. The next frame's drag starts where this
    // one ended, with no wheel steps yet.
    CameraInput     takeCameraInput(CameraInputState& state, const UiInputCapture& capture, float deltaTime);

}
// clang-format on
