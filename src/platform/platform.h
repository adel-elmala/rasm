// clang-format off
#pragma once

#include "rasm/input.h"
#include "rasm/window.h"

#include <memory>
#include <vector>

namespace rasm
{

    // The cursor shapes Dear ImGui asks for, in ImGuiMouseCursor's order after HIDDEN.
    enum class PlatformCursor
    {
        HIDDEN,
        ARROW,
        TEXT_INPUT,
        RESIZE_ALL,
        RESIZE_NS,
        RESIZE_EW,
        RESIZE_NESW,
        RESIZE_NWSE,
        HAND,
        WAIT,
        PROGRESS,
        NOT_ALLOWED,
    };

    // The window and the input of an engine. SDL opens a window and turns its events into InputEvents; NATIVE draws
    // into the application's window, whose events the application passes on itself; headless has neither, and the
    // frames go to the offscreen swapchain of the window's size.
    class Platform
    {
    public:
        virtual                     ~Platform() = default;
        // A headless window has no native handle, and its pixel size is the size asked for.
        virtual Window              createWindow(uint32_t width, uint32_t height) = 0;
        virtual void                destroyWindow(const Window& window) = 0;
        // Appends the events since the last poll, and updates the window's size, scale and minimized state. closed:
        // the user closed the window.
        virtual void                pollEvents(Window& window, std::vector<InputEvent>& events, bool& closed) = 0;

        // What the UI asks of a window RASM owns. The others do nothing: an application that owns its window reads
        // Dear ImGui's cursor, clipboard and text input requests itself.
        virtual bool                hasCursors() const { return false; }
        virtual void                setCursor(const Window& window, PlatformCursor cursor) { (void)window; (void)cursor; }
        virtual void                setTextInput(const Window& window, bool on) { (void)window; (void)on; }
        virtual const char*         getClipboardText() { return nullptr; }
        virtual void                setClipboardText(const char* text) { (void)text; }
    };

    std::unique_ptr<Platform>       createSdlPlatform();
    std::unique_ptr<Platform>       createHeadlessPlatform();
    std::unique_ptr<Platform>       createNativePlatform(const NativeWindow& native);

}
// clang-format on
