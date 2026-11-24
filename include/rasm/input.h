// clang-format off
#pragma once

#include "rasm/handle.h"

#include <cstdint>

namespace rasm
{

    // A key, named after its place on a US keyboard. InputEvent::key uses it for the key's meaning in the current
    // layout, and InputEvent::physicalKey for its position.
    enum class Key : uint16_t
    {
        NONE,
        A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        DIGIT_0, DIGIT_1, DIGIT_2, DIGIT_3, DIGIT_4, DIGIT_5, DIGIT_6, DIGIT_7, DIGIT_8, DIGIT_9,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        ESCAPE, ENTER, TAB, BACKSPACE, SPACE, INSERT, DEL, HOME, END, PAGE_UP, PAGE_DOWN,
        LEFT, RIGHT, UP, DOWN,
        LEFT_SHIFT, RIGHT_SHIFT, LEFT_CTRL, RIGHT_CTRL, LEFT_ALT, RIGHT_ALT, LEFT_SUPER, RIGHT_SUPER, MENU,
        APOSTROPHE, COMMA, MINUS, PERIOD, SLASH, SEMICOLON, EQUAL, LEFT_BRACKET, BACKSLASH, RIGHT_BRACKET, GRAVE,
        CAPS_LOCK, SCROLL_LOCK, NUM_LOCK, PRINT_SCREEN, PAUSE,
        KEYPAD_0, KEYPAD_1, KEYPAD_2, KEYPAD_3, KEYPAD_4, KEYPAD_5, KEYPAD_6, KEYPAD_7, KEYPAD_8, KEYPAD_9,
        KEYPAD_DECIMAL, KEYPAD_DIVIDE, KEYPAD_MULTIPLY, KEYPAD_SUBTRACT, KEYPAD_ADD, KEYPAD_ENTER, KEYPAD_EQUAL,
        COUNT
    };

    enum class MouseButton : uint8_t
    {
        LEFT,
        RIGHT,
        MIDDLE,
        BACK,
        FORWARD,
    };

    enum class InputEventType : uint8_t
    {
        NONE,
        KEY,            // key, physicalKey, down, repeat, modifiers
        MOUSE_BUTTON,   // button, down, x, y
        MOUSE_MOVE,     // x, y
        MOUSE_WHEEL,    // x, y: steps; y > 0 away from the user, x > 0 to the right
        MOUSE_LEAVE,    // the pointer left the window
        TEXT,           // codepoint: one character of typed text
        FOCUS,          // down: the window got the keyboard focus, else lost it
        WINDOW_CLOSE,   // the user closed a window other than the main one: the application decides (Engine::removeWindow)
    };

    // InputEvent::modifiers: the modifier keys held during a key event.
    constexpr uint8_t KEY_MOD_SHIFT     = 1u << 0;
    constexpr uint8_t KEY_MOD_CTRL      = 1u << 1;
    constexpr uint8_t KEY_MOD_ALT       = 1u << 2;
    constexpr uint8_t KEY_MOD_SUPER     = 1u << 3;

    // One input event from the application (Engine::addInputEvent) or from RASM's SDL platform. Only the fields its
    // type names are read. Mouse positions are in window points from the top-left corner, like Window::width. Only the
    // main window's events reach Dear ImGui and the engine's own camera input.
    struct InputEvent
    {
        InputEventType  type            = InputEventType::NONE;
        WindowHandle    window;                                 // the window it happened in; invalid: the main window
        Key             key             = Key::NONE;            // the key's meaning in the keyboard layout: Dear ImGui and the F keys
        Key             physicalKey     = Key::NONE;            // where the key is, as a US key: camera movement (W/A/S/D); NONE: the same as key
        bool            down            = false;
        bool            repeat          = false;                // a key held down repeats
        uint8_t         modifiers       = 0;                    // KEY_MOD_*
        MouseButton     button          = MouseButton::LEFT;
        float           x               = 0.0f;
        float           y               = 0.0f;
        uint32_t        codepoint       = 0;                    // Unicode
    };

    // What Dear ImGui took from the last UI frame (Engine::getUiInputCapture). All false without a UI.
    struct UiInputCapture
    {
        bool            mouse           = false;                // the pointer is over a UI window, or drags one
        bool            keyboard        = false;                // a UI window has the keyboard focus
        bool            text            = false;                // a text field takes typing
    };

}
// clang-format on
