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

    // The windows and the input of an engine. SDL opens windows and turns their events into InputEvents; NATIVE draws
    // into the application's windows, whose events the application passes on itself; headless has neither, and the
    // frames go to offscreen swapchains of the windows' sizes.
    class Platform
    {
    public:
        virtual                     ~Platform() = default;
        // SDL opens windows and reads events; NATIVE wraps the application's windows; HEADLESS has none.
        virtual PlatformType        type() const = 0;
        // The window without its handle, which the engine gives. A headless window has no native handle, and its pixel
        // size is the size asked for. A window without a native handle failed (except headless).
        virtual Window              createWindow(const WindowDesc& desc) = 0;
        virtual void                destroyWindow(const Window& window) = 0;
        // Appends the events since the last poll, each with its window's handle, and updates each window's size, scale
        // and minimized state. windows has count entries; those without a handle are free. closed: the user closed the
        // main window (windows[MAIN_WINDOW_INDEX]); closing another is a WINDOW_CLOSE event.
        virtual void                pollEvents(Window* windows, uint32_t count, std::vector<InputEvent>& events, bool& closed) = 0;

        // What the UI asks of a window RASM owns. The others do nothing: an application that owns its window reads
        // Dear ImGui's cursor, clipboard and text input requests itself.
        virtual bool                hasCursors() const { return false; }
        virtual void                setCursor(const Window& window, PlatformCursor cursor) { (void)window; (void)cursor; }
        virtual void                setTextInput(const Window& window, bool on) { (void)window; (void)on; }
        virtual const char*         getClipboardText() { return nullptr; }
        virtual void                setClipboardText(const char* text) { (void)text; }
        // A camera drag's grab: the pointer hides and stays, and moves come as dx and dy only, so a drag goes on past
        // the window's edge. Off puts the pointer back where the grab began. An application's own window does the same
        // itself and sends the motion in InputEvent::dx and dy.
        virtual void                setMouseGrab(const Window& window, bool on) { (void)window; (void)on; }
    };

    std::unique_ptr<Platform>       createSdlPlatform();
    std::unique_ptr<Platform>       createHeadlessPlatform();
    std::unique_ptr<Platform>       createNativePlatform();

}
// clang-format on
