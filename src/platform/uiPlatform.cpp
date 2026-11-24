#include "platform/uiPlatform.h"

#include "imgui.h"

#include <cfloat>

// Dear ImGui's input comes from RASM's InputEvents, whichever platform made them: the SDL platform, the application of
// a NATIVE engine, or a script of a headless one. No ImGui platform backend runs. What only the window's owner can do
// (cursor shapes, the clipboard, starting text input) goes through the Platform, and the SDL platform does it.
namespace rasm
{
    namespace
    {
        // In the order of Key, from Key::NONE.
        // clang-format off
        constexpr ImGuiKey IMGUI_KEYS[] = {
            ImGuiKey_None,
            ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F, ImGuiKey_G, ImGuiKey_H, ImGuiKey_I, ImGuiKey_J,
            ImGuiKey_K, ImGuiKey_L, ImGuiKey_M, ImGuiKey_N, ImGuiKey_O, ImGuiKey_P, ImGuiKey_Q, ImGuiKey_R, ImGuiKey_S, ImGuiKey_T,
            ImGuiKey_U, ImGuiKey_V, ImGuiKey_W, ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z,
            ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9,
            ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7, ImGuiKey_F8, ImGuiKey_F9,
            ImGuiKey_F10, ImGuiKey_F11, ImGuiKey_F12,
            ImGuiKey_Escape, ImGuiKey_Enter, ImGuiKey_Tab, ImGuiKey_Backspace, ImGuiKey_Space, ImGuiKey_Insert, ImGuiKey_Delete,
            ImGuiKey_Home, ImGuiKey_End, ImGuiKey_PageUp, ImGuiKey_PageDown,
            ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
            ImGuiKey_LeftShift, ImGuiKey_RightShift, ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl, ImGuiKey_LeftAlt, ImGuiKey_RightAlt,
            ImGuiKey_LeftSuper, ImGuiKey_RightSuper, ImGuiKey_Menu,
            ImGuiKey_Apostrophe, ImGuiKey_Comma, ImGuiKey_Minus, ImGuiKey_Period, ImGuiKey_Slash, ImGuiKey_Semicolon, ImGuiKey_Equal,
            ImGuiKey_LeftBracket, ImGuiKey_Backslash, ImGuiKey_RightBracket, ImGuiKey_GraveAccent,
            ImGuiKey_CapsLock, ImGuiKey_ScrollLock, ImGuiKey_NumLock, ImGuiKey_PrintScreen, ImGuiKey_Pause,
            ImGuiKey_Keypad0, ImGuiKey_Keypad1, ImGuiKey_Keypad2, ImGuiKey_Keypad3, ImGuiKey_Keypad4, ImGuiKey_Keypad5,
            ImGuiKey_Keypad6, ImGuiKey_Keypad7, ImGuiKey_Keypad8, ImGuiKey_Keypad9,
            ImGuiKey_KeypadDecimal, ImGuiKey_KeypadDivide, ImGuiKey_KeypadMultiply, ImGuiKey_KeypadSubtract, ImGuiKey_KeypadAdd,
            ImGuiKey_KeypadEnter, ImGuiKey_KeypadEqual,
        };
        // clang-format on
        static_assert(sizeof(IMGUI_KEYS) / sizeof(IMGUI_KEYS[0]) == static_cast<size_t>(Key::COUNT), "IMGUI_KEYS must list every Key in order");
    }

    namespace
    {
        ImGuiKey toImGuiKey(Key key)
        {
            return key < Key::COUNT ? IMGUI_KEYS[static_cast<uint32_t>(key)] : ImGuiKey_None;
        }

        Platform *getPlatform()
        {
            return static_cast<Platform *>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
        }

        const char *getClipboardText(ImGuiContext *)
        {
            Platform *platform = getPlatform();
            return platform != nullptr ? platform->getClipboardText() : nullptr;
        }

        void setClipboardText(ImGuiContext *, const char *text)
        {
            if (Platform *platform = getPlatform())
                platform->setClipboardText(text);
        }
    }

    void initUiPlatform(Platform &platform)
    {
        ImGuiIO &io = ImGui::GetIO();
        io.BackendPlatformName = "rasm";
        if (platform.hasCursors())
            io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        ImGuiPlatformIO &platformIo = ImGui::GetPlatformIO();
        platformIo.Platform_ClipboardUserData = &platform;
        platformIo.Platform_GetClipboardTextFn = getClipboardText;
        platformIo.Platform_SetClipboardTextFn = setClipboardText;
    }

    void shutdownUiPlatform()
    {
        ImGuiIO &io = ImGui::GetIO();
        io.BackendPlatformName = nullptr;
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
        ImGuiPlatformIO &platformIo = ImGui::GetPlatformIO();
        platformIo.Platform_ClipboardUserData = nullptr;
        platformIo.Platform_GetClipboardTextFn = nullptr;
        platformIo.Platform_SetClipboardTextFn = nullptr;
    }

    void processUiEvent(const InputEvent &event)
    {
        ImGuiIO &io = ImGui::GetIO();
        switch (event.type)
        {
        case InputEventType::KEY:
            // ImGui takes the modifiers as keys of their own, before the key they change.
            io.AddKeyEvent(ImGuiMod_Ctrl, (event.modifiers & KEY_MOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (event.modifiers & KEY_MOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (event.modifiers & KEY_MOD_ALT) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (event.modifiers & KEY_MOD_SUPER) != 0);
            // A key whose meaning has no name (a 'ü' on a German keyboard) goes by its position, as ImGui's own backends do.
            if (const ImGuiKey key = toImGuiKey(event.key != Key::NONE ? event.key : event.physicalKey); key != ImGuiKey_None)
                io.AddKeyEvent(key, event.down);
            break;
        case InputEventType::MOUSE_BUTTON:
            // Also where it happened, for an application that sends no move before a click.
            io.AddMousePosEvent(event.x, event.y);
            io.AddMouseButtonEvent(static_cast<int>(event.button), event.down);
            break;
        case InputEventType::MOUSE_MOVE:
            io.AddMousePosEvent(event.x, event.y);
            break;
        case InputEventType::MOUSE_WHEEL:
            // ImGui scrolls left for a positive x.
            io.AddMouseWheelEvent(-event.x, event.y);
            break;
        case InputEventType::MOUSE_LEAVE:
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        case InputEventType::TEXT:
            io.AddInputCharacter(event.codepoint);
            break;
        case InputEventType::FOCUS:
            io.AddFocusEvent(event.down);
            break;
        case InputEventType::NONE:
            break;
        }
    }

    // ImGui lays out in points, the window's size, and draws at the pixels per point, so a HiDPI window gets sharp text
    // at the same size. The step is the engine's deltaTime (EngineConfig::fixedDeltaTime, else the measured frame
    // time); ImGui needs it above 0, which the first frame's is not.
    void newUiPlatformFrame(const Window &window, float deltaTime)
    {
        ImGuiIO &io = ImGui::GetIO();
        const float width = static_cast<float>(window.width > 0 ? window.width : window.pixelWidth);
        const float height = static_cast<float>(window.height > 0 ? window.height : window.pixelHeight);
        io.DisplaySize = ImVec2(width, height);
        io.DisplayFramebufferScale = ImVec2(width > 0.0f ? static_cast<float>(window.pixelWidth) / width : 1.0f,
                                            height > 0.0f ? static_cast<float>(window.pixelHeight) / height : 1.0f);
        io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;
    }

    // While no UI frame is built the events are not fed, so on the next one no key is down and the mouse is where the
    // engine last saw it, not where ImGui did: a click right away is tested against the windows under the pointer.
    void resumeUiInput(const InputState &input)
    {
        ImGuiIO &io = ImGui::GetIO();
        io.ClearInputKeys();
        io.ClearInputMouse();
        if (input.mouseInside)
            io.AddMousePosEvent(input.mouseX, input.mouseY);
        else
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    // After ImGui::Render: the cursor the frame asked for, and text input while a text field has the focus. Only
    // changes reach the platform.
    void updateUiPlatform(Platform &platform, const Window &window, UiPlatformState &state)
    {
        const ImGuiIO &io = ImGui::GetIO();
        if (io.WantTextInput != state.textInput)
        {
            state.textInput = io.WantTextInput;
            platform.setTextInput(window, state.textInput);
        }
        if ((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0)
            return;
        // ImGui draws the cursor itself when asked to, so the system's is hidden then.
        const int cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
        if (cursor != state.cursor)
        {
            state.cursor = cursor;
            platform.setCursor(window, static_cast<PlatformCursor>(cursor + 1));
        }
    }

    // The UI stops building frames: the window gets the arrow back and stops taking text.
    void suspendUiPlatform(Platform &platform, const Window &window, UiPlatformState &state)
    {
        if (state.textInput)
            platform.setTextInput(window, false);
        if (state.cursor != -2 && state.cursor != ImGuiMouseCursor_Arrow)
            platform.setCursor(window, PlatformCursor::ARROW);
        state = {};
    }

    // The style and the fonts follow the display's scale, so the UI keeps its size on a 150 % or 4K display. A new
    // scale scales the current style by the ratio, so the application's colors and sizes stay; ScaleAllSizes rounds
    // each size down, so a size may lose a pixel over moves between displays.
    void setUiScale(float from, float to)
    {
        ImGuiStyle &style = ImGui::GetStyle();
        style.ScaleAllSizes(to / from);
        style.FontScaleDpi = to;
    }

    UiInputCapture getUiInputCapture()
    {
        const ImGuiIO &io = ImGui::GetIO();
        return {.mouse = io.WantCaptureMouse, .keyboard = io.WantCaptureKeyboard, .text = io.WantTextInput};
    }
}
