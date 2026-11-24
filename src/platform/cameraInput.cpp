#include "rasm/cameraInput.h"

#include <algorithm>

namespace rasm
{
    namespace
    {
        bool isDown(const CameraInputState &state, Key key)
        {
            return state.keysDown[static_cast<uint32_t>(key)];
        }

        bool isInArea(float x, float y, const InputArea &area)
        {
            return x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height;
        }
    }

    glm::vec2 toInputNdc(float x, float y, const InputArea &area)
    {
        const float minDim = std::max(1.0f, std::min(area.width, area.height));
        // Screen y grows downward; NDC y grows upward like the camera's +Y (D8).
        return glm::vec2((2.0f * (x - area.x) - area.width) / minDim, (area.height - 2.0f * (y - area.y)) / minDim);
    }

    // The capture is the last UI frame's, as Dear ImGui decided it: a press or the wheel over a UI window does not
    // reach the camera, and a drag that started outside keeps going over one.
    void addCameraInputEvent(CameraInputState &state, const InputEvent &event, const InputArea &area, const UiInputCapture &capture)
    {
        switch (event.type)
        {
        case InputEventType::KEY:
        {
            const Key position = event.physicalKey != Key::NONE ? event.physicalKey : event.key;
            if (position != Key::NONE && position < Key::COUNT)
                state.keysDown[static_cast<uint32_t>(position)] = event.down;
            break;
        }
        case InputEventType::MOUSE_BUTTON:
            state.pointerX = event.x;
            state.pointerY = event.y;
            state.pointerInWindow = true;
            state.pointerSeen = true;
            if (!event.down)
            {
                state.dragActive = false;
                break;
            }
            if (capture.mouse || !isInArea(event.x, event.y, area))
                break;
            state.dragStart = toInputNdc(event.x, event.y, area);
            state.dragEnd = state.dragStart;
            state.dragActive = true;
            break;
        case InputEventType::MOUSE_MOVE:
            state.pointerX = event.x;
            state.pointerY = event.y;
            state.pointerInWindow = true;
            state.pointerSeen = true;
            if (state.dragActive)
                state.dragEnd = toInputNdc(event.x, event.y, area);
            break;
        case InputEventType::MOUSE_WHEEL:
            // Wheel events carry no position. Before the first move, a wheel over the window counts.
            if (!capture.mouse && (!state.pointerSeen || (state.pointerInWindow && isInArea(state.pointerX, state.pointerY, area))))
                state.wheel += event.y;
            break;
        case InputEventType::MOUSE_LEAVE:
            state.pointerInWindow = false;
            state.pointerSeen = true;
            break;
        case InputEventType::FOCUS:
            // Keys and buttons released while another window has the focus never send their release here.
            if (!event.down)
            {
                std::fill(std::begin(state.keysDown), std::end(state.keysDown), false);
                state.dragActive = false;
            }
            break;
        case InputEventType::TEXT:
        case InputEventType::WINDOW_CLOSE:
        case InputEventType::NONE:
            break;
        }
    }

    CameraInput takeCameraInput(CameraInputState &state, const UiInputCapture &capture, float deltaTime)
    {
        CameraInput input = {.wheelDelta = state.wheel, .deltaTime = deltaTime};
        if (state.dragActive)
        {
            input.mouseStart = state.dragStart;
            input.mouseEnd = state.dragEnd;
        }
        if (!capture.keyboard)
        {
            input.forward = isDown(state, Key::W);
            input.backward = isDown(state, Key::S);
            input.left = isDown(state, Key::A);
            input.right = isDown(state, Key::D);
        }
        state.dragStart = state.dragEnd;
        state.wheel = 0.0f;
        return input;
    }
}
