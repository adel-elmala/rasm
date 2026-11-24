// clang-format off
#pragma once

#include "platform/input.h"
#include "platform/platform.h"

namespace rasm
{

    // What the platform was last told, so it hears only of changes.
    struct UiPlatformState
    {
        int             cursor          = -2;       // ImGuiMouseCursor; -2: none set yet
        bool            textInput       = false;
    };

    void            initUiPlatform(Platform& platform);
    void            shutdownUiPlatform();
    void            processUiEvent(const InputEvent& event);
    void            newUiPlatformFrame(const Window& window, float deltaTime);
    void            resumeUiInput(const InputState& input);
    void            updateUiPlatform(Platform& platform, const Window& window, UiPlatformState& state);
    void            suspendUiPlatform(Platform& platform, const Window& window, UiPlatformState& state);
    void            setUiScale(float from, float to);
    UiInputCapture  readUiInputCapture();

}
// clang-format on
