#include "platform/uiPlatform.h"

#include "SDL3/SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "spdlog/spdlog.h"

// Dear ImGui's input comes from SDL through ImGui's own SDL3 backend: keys, text, the mouse, the clipboard, cursors
// and the display's size and pixel density. Only this file knows of it, so a platform layer the application owns
// (D7) replaces this file and nothing else. A headless window has no handle: ImGui then runs without a backend and
// without input, on a display of the window's size at a scale of 1, and the UI is drawn as with a window.
namespace rasm
{
    namespace
    {
        bool isHeadless(const Window &window)
        {
            return !window.handle.isValid();
        }
    }

    bool initUiPlatform(const Window &window)
    {
        if (isHeadless(window))
            return true;
        if (!ImGui_ImplSDL3_InitForOther(reinterpret_cast<SDL_Window *>(window.handle.index)))
        {
            spdlog::error("Failed to start Dear ImGui's SDL3 backend.");
            return false;
        }
        return true;
    }

    void shutdownUiPlatform(const Window &window)
    {
        if (!isHeadless(window))
            ImGui_ImplSDL3_Shutdown();
    }

    void processUiEvent(const void *event)
    {
        ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event *>(event));
    }

    // Headless, the delta time is the engine's frame time; ImGui needs it above 0, which the first frame's is not.
    void newUiPlatformFrame(const Window &window, float deltaTime)
    {
        if (!isHeadless(window))
        {
            ImGui_ImplSDL3_NewFrame();
            return;
        }
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(window.pixelWidth), static_cast<float>(window.pixelHeight));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;
    }

    // While no UI frame is built the events are not fed, so on the next one no key is down and the mouse is where SDL
    // says, not where it was: a click right away is tested against the windows under the cursor.
    void resumeUiInput(const Window &window)
    {
        ImGuiIO &io = ImGui::GetIO();
        io.ClearInputKeys();
        io.ClearInputMouse();
        if (isHeadless(window))
            return;
        float x = 0.0f;
        float y = 0.0f;
        SDL_GetMouseState(&x, &y);
        io.AddMousePosEvent(x, y);
    }

    // The display's content scale (1.5 at 150 % in Windows' settings), without the pixel density: ImGui draws in
    // points, and SDL3's backend gives it the pixels per point.
    float getUiContentScale(const Window &window)
    {
        if (isHeadless(window))
            return 1.0f;
        const SDL_DisplayID display = SDL_GetDisplayForWindow(reinterpret_cast<SDL_Window *>(window.handle.index));
        const float scale = display != 0 ? SDL_GetDisplayContentScale(display) : 0.0f;
        return scale > 0.0f ? scale : 1.0f;
    }

    UiInputCapture getUiInputCapture()
    {
        const ImGuiIO &io = ImGui::GetIO();
        return {.mouse = io.WantCaptureMouse, .keyboard = io.WantCaptureKeyboard, .text = io.WantTextInput};
    }
}
