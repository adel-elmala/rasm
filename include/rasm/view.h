// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/handle.h"
#include "rasm/cameraInput.h"
#include "rasm/window.h"

#include <cstdint>

namespace rasm
{

    // A part of the window, or of a target, as fractions of its size from the top-left corner: (0, 0, 1, 1) is all of it.
    struct ViewRect
    {
        float               x               = 0.0f;
        float               y               = 0.0f;
        float               width           = 1.0f;
        float               height          = 1.0f;

        bool                operator==(const ViewRect&) const = default;
    };

    // Where the tone-map pass draws a view: a rect of a window, or a rect of a color target instead (a view texture,
    // which materials and ImGui::Image can show).
    struct ViewOutput
    {
        ViewRect            rect            = {};
        RenderTargetHandle  target;                         // valid: into this target, one color and no depth (R8G8B8A8_SRGB for a picture)
        WindowHandle        window;                         // without a target: this window (Engine::addWindow); invalid: the main window

        bool                operator==(const ViewOutput&) const = default;
    };

    // A view's choice for a setting EngineConfig also has.
    enum class ViewToggle : uint8_t
    {
        DEFAULT,                                            // the EngineConfig value
        OFF,
        ON,
    };

    // One view of Engine::render(views, count): a camera's picture of a scene, tone mapped into its output.
    struct View
    {
        SceneHandle         scene;
        CameraHandle        camera;
        ViewOutput          output          = {};
        ViewToggle          bloom           = ViewToggle::DEFAULT;      // DEFAULT: EngineConfig::bloom
        AntiAliasing        antiAliasing    = AntiAliasing::DEFAULT;    // DEFAULT: EngineConfig::antiAliasing

        bool                operator==(const View&) const = default;
    };

    // A rect in pixels.
    struct ViewPixels
    {
        uint32_t            x               = 0;
        uint32_t            y               = 0;
        uint32_t            width           = 0;
        uint32_t            height          = 0;

        bool                operator==(const ViewPixels&) const = default;
    };

    // The pixels a rect covers in an image of width x height: each edge is rounded, so rects that touch share their
    // edge and leave no gap, and the result is at least 1 x 1 inside the image (0 x 0 for an empty image).
    ViewPixels              getViewPixels(const ViewRect& rect, uint32_t width, uint32_t height);
    // The rect in the window's points, for a CameraInputState of the view.
    InputArea               getInputArea(const Window& window, const ViewRect& rect);

}
// clang-format on
