#include "platform/input.h"

#include <algorithm>

namespace rasm
{
    namespace
    {
        bool isDown(const InputState &state, Key key)
        {
            return state.keysDown[static_cast<uint32_t>(key)];
        }

        void applyKey(InputState &state, const InputEvent &event, const UiInputCapture &capture)
        {
            const Key position = event.physicalKey != Key::NONE ? event.physicalKey : event.key;
            if (position != Key::NONE && position < Key::COUNT)
                state.keysDown[static_cast<uint32_t>(position)] = event.down;

            // The engine's own keys follow the layout, and a text field keeps them as typing.
            if (!event.down || capture.text)
                return;
            if (event.key == Key::ESCAPE)
                ++state.escapes;
            if (event.repeat)
                return;
            if (event.key == Key::F1)
                ++state.statsToggles;
            if (event.key == Key::F2)
                ++state.forcedLodSteps;
            if (event.key == Key::F3)
                state.dumpGraph = true;
        }
    }

    glm::vec2 toInputNdc(float x, float y, const Window &window)
    {
        const float minDim = static_cast<float>(std::max(1u, std::min(window.width, window.height)));
        // Screen y grows downward; NDC y grows upward like the camera's +Y (D8).
        return glm::vec2((2.0f * x - static_cast<float>(window.width)) / minDim, (static_cast<float>(window.height) - 2.0f * y) / minDim);
    }

    void beginInputFrame(InputState &state)
    {
        state.dragStart = state.lastMousePos;
        state.dragEnd = state.lastMousePos;
        state.wheel = 0.0f;
        state.statsToggles = 0;
        state.forcedLodSteps = 0;
        state.escapes = 0;
        state.dumpGraph = false;
    }

    // The capture is the last UI frame's, as Dear ImGui decided it: a press or the wheel over a UI window does not
    // reach the camera, and a drag that started outside keeps going over one.
    void applyInputEvent(InputState &state, const InputEvent &event, const Window &window, const UiInputCapture &capture)
    {
        switch (event.type)
        {
        case InputEventType::KEY:
            applyKey(state, event, capture);
            break;
        case InputEventType::MOUSE_BUTTON:
            state.mouseX = event.x;
            state.mouseY = event.y;
            state.mouseInside = true;
            if (!event.down)
            {
                state.dragActive = false;
                break;
            }
            if (capture.mouse)
                break;
            state.lastMousePos = toInputNdc(event.x, event.y, window);
            state.dragStart = state.lastMousePos;
            state.dragEnd = state.lastMousePos;
            state.dragActive = true;
            break;
        case InputEventType::MOUSE_MOVE:
            state.mouseX = event.x;
            state.mouseY = event.y;
            state.mouseInside = true;
            if (state.dragActive)
                state.dragEnd = toInputNdc(event.x, event.y, window);
            break;
        case InputEventType::MOUSE_WHEEL:
            if (!capture.mouse)
                state.wheel += event.y;
            break;
        case InputEventType::MOUSE_LEAVE:
            state.mouseInside = false;
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
        case InputEventType::NONE:
            break;
        }
    }

    CameraInput endInputFrame(InputState &state, const UiInputCapture &capture, float deltaTime)
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
        state.lastMousePos = state.dragEnd;
        return input;
    }
}
