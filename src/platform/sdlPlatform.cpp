#include "platform/platform.h"

#include <cstring>

#include "SDL3/SDL.h"
#if defined(__APPLE__)
#include "SDL3/SDL_metal.h"
#endif
#include "spdlog/spdlog.h"

namespace rasm
{
    namespace
    {
        struct SdlKey
        {
            Key key;
            SDL_Scancode scancode;
            SDL_Keycode keycode;
        };

#define RASM_SDL_KEY(key, name) {Key::key, SDL_SCANCODE_##name, SDLK_##name}
        // clang-format off
        constexpr SdlKey SDL_KEYS[] = {
            RASM_SDL_KEY(A, A), RASM_SDL_KEY(B, B), RASM_SDL_KEY(C, C), RASM_SDL_KEY(D, D), RASM_SDL_KEY(E, E), RASM_SDL_KEY(F, F),
            RASM_SDL_KEY(G, G), RASM_SDL_KEY(H, H), RASM_SDL_KEY(I, I), RASM_SDL_KEY(J, J), RASM_SDL_KEY(K, K), RASM_SDL_KEY(L, L),
            RASM_SDL_KEY(M, M), RASM_SDL_KEY(N, N), RASM_SDL_KEY(O, O), RASM_SDL_KEY(P, P), RASM_SDL_KEY(Q, Q), RASM_SDL_KEY(R, R),
            RASM_SDL_KEY(S, S), RASM_SDL_KEY(T, T), RASM_SDL_KEY(U, U), RASM_SDL_KEY(V, V), RASM_SDL_KEY(W, W), RASM_SDL_KEY(X, X),
            RASM_SDL_KEY(Y, Y), RASM_SDL_KEY(Z, Z),
            RASM_SDL_KEY(DIGIT_0, 0), RASM_SDL_KEY(DIGIT_1, 1), RASM_SDL_KEY(DIGIT_2, 2), RASM_SDL_KEY(DIGIT_3, 3), RASM_SDL_KEY(DIGIT_4, 4),
            RASM_SDL_KEY(DIGIT_5, 5), RASM_SDL_KEY(DIGIT_6, 6), RASM_SDL_KEY(DIGIT_7, 7), RASM_SDL_KEY(DIGIT_8, 8), RASM_SDL_KEY(DIGIT_9, 9),
            RASM_SDL_KEY(F1, F1), RASM_SDL_KEY(F2, F2), RASM_SDL_KEY(F3, F3), RASM_SDL_KEY(F4, F4), RASM_SDL_KEY(F5, F5), RASM_SDL_KEY(F6, F6),
            RASM_SDL_KEY(F7, F7), RASM_SDL_KEY(F8, F8), RASM_SDL_KEY(F9, F9), RASM_SDL_KEY(F10, F10), RASM_SDL_KEY(F11, F11), RASM_SDL_KEY(F12, F12),
            RASM_SDL_KEY(ESCAPE, ESCAPE), RASM_SDL_KEY(ENTER, RETURN), RASM_SDL_KEY(TAB, TAB), RASM_SDL_KEY(BACKSPACE, BACKSPACE),
            RASM_SDL_KEY(SPACE, SPACE), RASM_SDL_KEY(INSERT, INSERT), RASM_SDL_KEY(DEL, DELETE), RASM_SDL_KEY(HOME, HOME), RASM_SDL_KEY(END, END),
            RASM_SDL_KEY(PAGE_UP, PAGEUP), RASM_SDL_KEY(PAGE_DOWN, PAGEDOWN),
            RASM_SDL_KEY(LEFT, LEFT), RASM_SDL_KEY(RIGHT, RIGHT), RASM_SDL_KEY(UP, UP), RASM_SDL_KEY(DOWN, DOWN),
            RASM_SDL_KEY(LEFT_SHIFT, LSHIFT), RASM_SDL_KEY(RIGHT_SHIFT, RSHIFT), RASM_SDL_KEY(LEFT_CTRL, LCTRL), RASM_SDL_KEY(RIGHT_CTRL, RCTRL),
            RASM_SDL_KEY(LEFT_ALT, LALT), RASM_SDL_KEY(RIGHT_ALT, RALT), RASM_SDL_KEY(LEFT_SUPER, LGUI), RASM_SDL_KEY(RIGHT_SUPER, RGUI),
            RASM_SDL_KEY(MENU, APPLICATION),
            RASM_SDL_KEY(APOSTROPHE, APOSTROPHE), RASM_SDL_KEY(COMMA, COMMA), RASM_SDL_KEY(MINUS, MINUS), RASM_SDL_KEY(PERIOD, PERIOD),
            RASM_SDL_KEY(SLASH, SLASH), RASM_SDL_KEY(SEMICOLON, SEMICOLON), RASM_SDL_KEY(EQUAL, EQUALS), RASM_SDL_KEY(LEFT_BRACKET, LEFTBRACKET),
            RASM_SDL_KEY(BACKSLASH, BACKSLASH), RASM_SDL_KEY(RIGHT_BRACKET, RIGHTBRACKET), RASM_SDL_KEY(GRAVE, GRAVE),
            RASM_SDL_KEY(CAPS_LOCK, CAPSLOCK), RASM_SDL_KEY(SCROLL_LOCK, SCROLLLOCK), RASM_SDL_KEY(NUM_LOCK, NUMLOCKCLEAR),
            RASM_SDL_KEY(PRINT_SCREEN, PRINTSCREEN), RASM_SDL_KEY(PAUSE, PAUSE),
            RASM_SDL_KEY(KEYPAD_0, KP_0), RASM_SDL_KEY(KEYPAD_1, KP_1), RASM_SDL_KEY(KEYPAD_2, KP_2), RASM_SDL_KEY(KEYPAD_3, KP_3),
            RASM_SDL_KEY(KEYPAD_4, KP_4), RASM_SDL_KEY(KEYPAD_5, KP_5), RASM_SDL_KEY(KEYPAD_6, KP_6), RASM_SDL_KEY(KEYPAD_7, KP_7),
            RASM_SDL_KEY(KEYPAD_8, KP_8), RASM_SDL_KEY(KEYPAD_9, KP_9),
            RASM_SDL_KEY(KEYPAD_DECIMAL, KP_PERIOD), RASM_SDL_KEY(KEYPAD_DIVIDE, KP_DIVIDE), RASM_SDL_KEY(KEYPAD_MULTIPLY, KP_MULTIPLY),
            RASM_SDL_KEY(KEYPAD_SUBTRACT, KP_MINUS), RASM_SDL_KEY(KEYPAD_ADD, KP_PLUS), RASM_SDL_KEY(KEYPAD_ENTER, KP_ENTER),
            RASM_SDL_KEY(KEYPAD_EQUAL, KP_EQUALS),
        };
        // clang-format on
#undef RASM_SDL_KEY

        // ImGuiMouseCursor's shapes after HIDDEN, as PlatformCursor orders them.
        constexpr SDL_SystemCursor SDL_CURSORS[] = {
            SDL_SYSTEM_CURSOR_DEFAULT,
            SDL_SYSTEM_CURSOR_TEXT,
            SDL_SYSTEM_CURSOR_MOVE,
            SDL_SYSTEM_CURSOR_NS_RESIZE,
            SDL_SYSTEM_CURSOR_EW_RESIZE,
            SDL_SYSTEM_CURSOR_NESW_RESIZE,
            SDL_SYSTEM_CURSOR_NWSE_RESIZE,
            SDL_SYSTEM_CURSOR_POINTER,
            SDL_SYSTEM_CURSOR_WAIT,
            SDL_SYSTEM_CURSOR_PROGRESS,
            SDL_SYSTEM_CURSOR_NOT_ALLOWED,
        };
        constexpr uint32_t SDL_CURSOR_COUNT = sizeof(SDL_CURSORS) / sizeof(SDL_CURSORS[0]);
    }

    namespace
    {
        Key toKey(SDL_Keycode keycode)
        {
            for (const SdlKey &key : SDL_KEYS)
            {
                if (key.keycode == keycode)
                    return key.key;
            }
            return Key::NONE;
        }

        Key toPhysicalKey(SDL_Scancode scancode)
        {
            for (const SdlKey &key : SDL_KEYS)
            {
                if (key.scancode == scancode)
                    return key.key;
            }
            return Key::NONE;
        }

        uint8_t toModifiers(SDL_Keymod mod)
        {
            return static_cast<uint8_t>(((mod & SDL_KMOD_SHIFT) != 0 ? KEY_MOD_SHIFT : 0) | ((mod & SDL_KMOD_CTRL) != 0 ? KEY_MOD_CTRL : 0) |
                                        ((mod & SDL_KMOD_ALT) != 0 ? KEY_MOD_ALT : 0) | ((mod & SDL_KMOD_GUI) != 0 ? KEY_MOD_SUPER : 0));
        }

        bool toMouseButton(uint8_t button, MouseButton &out)
        {
            switch (button)
            {
            case SDL_BUTTON_LEFT:
                out = MouseButton::LEFT;
                return true;
            case SDL_BUTTON_RIGHT:
                out = MouseButton::RIGHT;
                return true;
            case SDL_BUTTON_MIDDLE:
                out = MouseButton::MIDDLE;
                return true;
            case SDL_BUTTON_X1:
                out = MouseButton::BACK;
                return true;
            case SDL_BUTTON_X2:
                out = MouseButton::FORWARD;
                return true;
            default:
                return false;
            }
        }

        // One TEXT event per character of SDL's UTF-8 text (an IME commits several at once). A broken sequence ends it.
        void appendText(const char *text, std::vector<InputEvent> &events)
        {
            const auto *bytes = reinterpret_cast<const unsigned char *>(text);
            while (*bytes != 0)
            {
                uint32_t codepoint = *bytes;
                uint32_t extra = 0;
                if (codepoint >= 0xF0)
                {
                    codepoint &= 0x07;
                    extra = 3;
                }
                else if (codepoint >= 0xE0)
                {
                    codepoint &= 0x0F;
                    extra = 2;
                }
                else if (codepoint >= 0xC0)
                {
                    codepoint &= 0x1F;
                    extra = 1;
                }
                else if (codepoint >= 0x80)
                    return;
                ++bytes;
                for (uint32_t i = 0; i < extra; ++i, ++bytes)
                {
                    if ((*bytes & 0xC0) != 0x80)
                        return;
                    codepoint = (codepoint << 6) | (*bytes & 0x3F);
                }
                events.push_back({.type = InputEventType::TEXT, .codepoint = codepoint});
            }
        }

        // The display's content scale (1.5 at 150 % in Windows' settings), without the pixel density: the UI draws in
        // points, and the framebuffer scale gives it the pixels per point.
        float getContentScale(SDL_Window *window)
        {
            const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
            const float scale = display != 0 ? SDL_GetDisplayContentScale(display) : 0.0f;
            return scale > 0.0f ? scale : 1.0f;
        }

        // The handles the device makes its surface from, the same an application passes for PlatformType::NATIVE. On
        // macOS a Metal view gives the window a CAMetalLayer.
        bool getNativeWindow(SDL_Window *window, NativeWindow &native, SDL_MetalView &metalView)
        {
            const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
            (void)properties;
            (void)metalView;
#if defined(_WIN32)
            native.type = NativeWindowType::WINDOWS;
            native.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
            native.window = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
            metalView = SDL_Metal_CreateView(window);
            native.type = NativeWindowType::METAL;
            native.window = metalView != nullptr ? SDL_Metal_GetLayer(metalView) : nullptr;
#else
            const char *driver = SDL_GetCurrentVideoDriver();
            if (driver != nullptr && std::strcmp(driver, "wayland") == 0)
            {
                native.type = NativeWindowType::WAYLAND;
                native.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
                native.window = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
            }
            else if (driver != nullptr && std::strcmp(driver, "x11") == 0)
            {
                native.type = NativeWindowType::X11;
                native.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
                native.window = reinterpret_cast<void *>(static_cast<uintptr_t>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
            }
#endif
            if (native.window == nullptr)
            {
                spdlog::error("SDL gave no native handle for its window (video driver {}): {}", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none", SDL_GetError());
                return false;
            }
            return true;
        }

        class SdlPlatform final : public Platform
        {
        public:
            ~SdlPlatform() override
            {
                for (SDL_Cursor *cursor : cursors)
                {
                    if (cursor != nullptr)
                        SDL_DestroyCursor(cursor);
                }
                SDL_free(clipboard);
            }

            Window createWindow(uint32_t width, uint32_t height) override
            {
                if (!SDL_Init(SDL_INIT_VIDEO))
                {
                    spdlog::error("Failed to initialize SDL: {}", SDL_GetError());
                    return Window{};
                }

#if defined(__APPLE__)
                const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL;
#else
                const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
#endif
                SDL_Window *sdlWindow = SDL_CreateWindow("RASM Engine", static_cast<int>(width), static_cast<int>(height), flags);
                if (sdlWindow == nullptr)
                {
                    spdlog::error("Failed to create SDL window: {}", SDL_GetError());
                    SDL_Quit();
                    return Window{};
                }

                Window window{};
                if (!getNativeWindow(sdlWindow, window.native, metalView))
                {
                    destroySdlWindow(sdlWindow);
                    return Window{};
                }

                int pixelWidth = 0;
                int pixelHeight = 0;
                if (!SDL_GetWindowSizeInPixels(sdlWindow, &pixelWidth, &pixelHeight))
                {
                    spdlog::warn("Failed to query window size in pixels: {}", SDL_GetError());
                    pixelWidth = static_cast<int>(width);
                    pixelHeight = static_cast<int>(height);
                }

                window.handle = {.index = reinterpret_cast<uint64_t>(sdlWindow), .generation = 1};
                window.width = width;
                window.height = height;
                window.pixelWidth = static_cast<uint32_t>(pixelWidth);
                window.pixelHeight = static_cast<uint32_t>(pixelHeight);
                window.contentScale = getContentScale(sdlWindow);
                return window;
            }

            void destroyWindow(const Window &window) override
            {
                if (window.handle.isValid())
                    destroySdlWindow(toSdl(window));
            }

            void pollEvents(Window &window, std::vector<InputEvent> &events, bool &closed) override
            {
                // Nothing is rendered while minimized, so block until an event arrives instead of spinning.
                if (window.minimized)
                    SDL_WaitEventTimeout(nullptr, 100);

                SDL_Event event;
                while (SDL_PollEvent(&event))
                {
                    switch (event.type)
                    {
                    case SDL_EVENT_QUIT:
                        spdlog::info("Quit event received, shutting down.");
                        closed = true;
                        break;
                    case SDL_EVENT_KEY_DOWN:
                    case SDL_EVENT_KEY_UP:
                    {
                        const InputEvent key = {.type = InputEventType::KEY,
                                                .key = toKey(event.key.key),
                                                .physicalKey = toPhysicalKey(event.key.scancode),
                                                .down = event.key.down,
                                                .repeat = event.key.repeat,
                                                .modifiers = toModifiers(event.key.mod)};
                        if (key.key != Key::NONE || key.physicalKey != Key::NONE)
                            events.push_back(key);
                        break;
                    }
                    case SDL_EVENT_TEXT_INPUT:
                        appendText(event.text.text, events);
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    case SDL_EVENT_MOUSE_BUTTON_UP:
                    {
                        MouseButton button = MouseButton::LEFT;
                        if (!toMouseButton(event.button.button, button))
                            break;
                        events.push_back({.type = InputEventType::MOUSE_BUTTON, .down = event.button.down, .button = button, .x = event.button.x, .y = event.button.y});
                        // The window keeps the mouse while a button is held, so a drag that leaves it still ends here.
                        if (event.button.down)
                            SDL_CaptureMouse(true);
                        else if (SDL_GetMouseState(nullptr, nullptr) == 0)
                            SDL_CaptureMouse(false);
                        break;
                    }
                    case SDL_EVENT_MOUSE_MOTION:
                        events.push_back({.type = InputEventType::MOUSE_MOVE, .x = event.motion.x, .y = event.motion.y});
                        break;
                    case SDL_EVENT_MOUSE_WHEEL:
                        events.push_back({.type = InputEventType::MOUSE_WHEEL, .x = event.wheel.x, .y = event.wheel.y});
                        break;
                    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                        events.push_back({.type = InputEventType::MOUSE_LEAVE});
                        break;
                    case SDL_EVENT_WINDOW_FOCUS_GAINED:
                        events.push_back({.type = InputEventType::FOCUS, .down = true});
                        break;
                    case SDL_EVENT_WINDOW_FOCUS_LOST:
                        events.push_back({.type = InputEventType::FOCUS, .down = false});
                        break;
                    case SDL_EVENT_WINDOW_RESIZED:
                        // Logical size, only used for mouse coordinates.
                        window.width = static_cast<uint32_t>(event.window.data1);
                        window.height = static_cast<uint32_t>(event.window.data2);
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        // The swapchain, depth textures, viewport and camera aspect follow the pixel size (B17).
                        window.resized = true;
                        window.pixelWidth = static_cast<uint32_t>(event.window.data1);
                        window.pixelHeight = static_cast<uint32_t>(event.window.data2);
                        break;
                    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
                        // Moved to another monitor, or its scale setting changed: the UI rescales on its next frame.
                        window.contentScale = getContentScale(toSdl(window));
                        break;
                    case SDL_EVENT_WINDOW_MINIMIZED:
                        window.minimized = true;
                        break;
                    case SDL_EVENT_WINDOW_RESTORED:
                    case SDL_EVENT_WINDOW_MAXIMIZED:
                        window.minimized = false;
                        break;
                    default:
                        break;
                    }
                }
            }

            bool hasCursors() const override
            {
                return true;
            }

            void setCursor(const Window &, PlatformCursor cursor) override
            {
                if (cursor == PlatformCursor::HIDDEN)
                {
                    SDL_HideCursor();
                    return;
                }
                const uint32_t index = static_cast<uint32_t>(cursor) - 1;
                if (index >= SDL_CURSOR_COUNT)
                    return;
                if (cursors[index] == nullptr)
                    cursors[index] = SDL_CreateSystemCursor(SDL_CURSORS[index]);
                SDL_SetCursor(cursors[index] != nullptr ? cursors[index] : SDL_GetDefaultCursor());
                SDL_ShowCursor();
            }

            // SDL sends text events only between these, and may show an on-screen keyboard.
            void setTextInput(const Window &window, bool on) override
            {
                if (!window.handle.isValid())
                    return;
                if (on)
                    SDL_StartTextInput(toSdl(window));
                else
                    SDL_StopTextInput(toSdl(window));
            }

            // Dear ImGui keeps the pointer until the next call.
            const char *getClipboardText() override
            {
                SDL_free(clipboard);
                clipboard = SDL_GetClipboardText();
                return clipboard;
            }

            void setClipboardText(const char *text) override
            {
                SDL_SetClipboardText(text);
            }

        private:
            static SDL_Window *toSdl(const Window &window)
            {
                return reinterpret_cast<SDL_Window *>(window.handle.index);
            }

            void destroySdlWindow(SDL_Window *window)
            {
#if defined(__APPLE__)
                if (metalView != nullptr)
                    SDL_Metal_DestroyView(metalView);
#endif
                metalView = nullptr;
                SDL_DestroyWindow(window);
                SDL_Quit();
            }

            SDL_MetalView metalView = nullptr;
            SDL_Cursor *cursors[SDL_CURSOR_COUNT] = {};
            char *clipboard = nullptr;
        };
    }

    std::unique_ptr<Platform> createSdlPlatform()
    {
        return std::make_unique<SdlPlatform>();
    }
}
