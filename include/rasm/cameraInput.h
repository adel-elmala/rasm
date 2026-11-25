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

    // What a mouse button's drag does: a trackball orbits or pans, a free-fly camera looks with either.
    enum class CameraDrag : uint8_t
    {
        NONE,
        ORBIT,
        PAN,
    };

    // Which drag each mouse button starts. Every button ORBIT is the behaviour before pans existed.
    struct CameraButtons
    {
        CameraDrag  left            = CameraDrag::ORBIT;
        CameraDrag  right           = CameraDrag::PAN;
        CameraDrag  middle          = CameraDrag::PAN;
        CameraDrag  back            = CameraDrag::NONE;
        CameraDrag  forward         = CameraDrag::NONE;
        CameraDrag  shiftLeft       = CameraDrag::PAN;     // the left button with shift held

        bool        operator==(const CameraButtons&) const = default;
    };

    // What one camera's input keeps between events: the keys held, the pointer and the drag. Feed it a frame's events
    // with addCameraInputEvent, then take the frame's CameraInput with takeCameraInput.
    struct CameraInputState
    {
        bool            keysDown[static_cast<uint32_t>(Key::COUNT)] = {};  // by position (InputEvent::physicalKey)
        float           pointerX        = 0.0f;                 // the pointer, in window points
        float           pointerY        = 0.0f;
        bool            pointerInWindow = false;                // a move or a button event put the pointer in the window, and it has not left
        bool            pointerSeen     = false;                // a move, button or leave event told where the pointer is
        bool            dragActive      = false;                // a mapped button went down in the area, outside the UI, and is not up yet
        CameraDrag      drag            = CameraDrag::NONE;     // what it does; NONE once a released drag was taken
        MouseButton     dragButton      = MouseButton::LEFT;    // its release ends the drag
        glm::vec2       dragStart       = glm::vec2(0.0f);      // NDC, this frame's
        glm::vec2       dragEnd         = glm::vec2(0.0f);
        glm::vec2       pointerNdc      = glm::vec2(0.0f);      // the pointer in the area's NDC, from the last event that told
        float           wheel           = 0.0f;                 // this frame's steps, with the pointer in the area or not seen yet
        CameraButtons   buttons         = {};
    };

    // Window points to the camera's NDC: both axes divided by the area's smaller side, so the trackball stays round
    // on a wide area, the area's center at 0, and y up.
    glm::vec2       toInputNdc(float x, float y, const InputArea& area);
    // A press in the area starts the drag state.buttons maps it to, which goes on outside the area until that button's
    // release; the wheel counts with the pointer in the area, or before any event told where it is. capture is what the
    // UI took (Engine::getUiInputCapture).
    void            addCameraInputEvent(CameraInputState& state, const InputEvent& event, const InputArea& area, const UiInputCapture& capture);
    // The frame's input without what the UI took, W/A/S/D held by position. The next frame's drag starts where this
    // one ended, with no wheel steps yet.
    CameraInput     takeCameraInput(CameraInputState& state, const UiInputCapture& capture, float deltaTime);

}
// clang-format on
