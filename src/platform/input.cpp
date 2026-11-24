#include "platform/input.h"

namespace rasm
{
    void beginInputFrame(InputState &state)
    {
        state.statsToggles = 0;
        state.forcedLodSteps = 0;
        state.escapes = 0;
        state.dumpGraph = false;
    }

    // The engine's own keys follow the layout, and a text field keeps them as typing.
    void applyInputEvent(InputState &state, const InputEvent &event, const InputArea &area, const UiInputCapture &capture)
    {
        addCameraInputEvent(state.camera, event, area, capture);
        if (event.type != InputEventType::KEY || !event.down || capture.text)
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
