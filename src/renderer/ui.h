// clang-format off
#pragma once

#include "rasm/renderer.h"
#include "rasm/handle.h"
#include "rasm/types.h"
#include "platform/uiPlatform.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct ImGuiContext;

namespace rasm
{

    struct UiScissor
    {
        int32_t                         x                   = 0;
        int32_t                         y                   = 0;
        uint32_t                        width               = 0;
        uint32_t                        height              = 0;
    };

    struct UiDraw
    {
        uint64_t                        vertices            = 0;    // device address of the draw's first vertex
        uint32_t                        firstIndex          = 0;
        uint32_t                        indexCount          = 0;
        uint32_t                        texture             = 0;    // bindless index, | 1 << 31 for an sRGB texture
        UiScissor                       scissor;
    };

    struct UiSlot
    {
        BufferHandle                    vertices;
        BufferHandle                    indices;
        uint64_t                        vertexBytes         = 0;    // capacity
        uint64_t                        indexBytes          = 0;
    };

    // What IMGUI_CHECKVERSION compares: two copies of Dear ImGui share a context only when these are the same.
    struct UiLayout
    {
        std::string                     version;                                // IMGUI_VERSION
        uint64_t                        io                  = 0;                // sizeof(ImGuiIO)
        uint64_t                        style               = 0;
        uint64_t                        vec2                = 0;
        uint64_t                        vec4                = 0;
        uint64_t                        drawVert            = 0;
        uint64_t                        drawIdx             = 0;
    };

    struct Ui
    {
        ImGuiContext*                                   context                         = nullptr;
        UiPlatformState                                 platformState;                              // the cursor and text input the platform was given
        float                                           scale                           = 1.0f;     // the window's content scale the style was made for
        bool                                            encoded                         = false;    // draws through the swapchain's UNORM view
        PipelineHandle                                  pipeline;
        UiSlot                                          slots[MAX_FRAMES_IN_FLIGHT];
        std::vector<UiDraw>                             draws;                                      // this frame's, recorded by the UI pass
        std::unordered_map<uint32_t, TextureHandle>     textures;                                   // ImGui's textures by bindless slot
        std::vector<uint8_t>                            expanded;                                   // an Alpha8 texture's pixels as RGBA8
        std::function<void()>                           callback;
        FrameStats                                      stats;                                      // what the RASM window shows, renewed every 250 ms of frame time
        float                                           statsAge                        = 1.0f;     // seconds of frame time since then; due at the start
        uint32_t                                        displaySize                     = 0;        // width | height << 16, in points
        bool                                            active                          = false;    // this frame built a UI
        bool                                            drawn                           = false;    // a UI pass drew it
        bool                                            fallbackLogged                  = false;
    };

    // The draw's clip rectangle in framebuffer pixels, clamped to the framebuffer; false when nothing is left.
    bool                                getUiScissor(const float clip[4], const float displayPos[2], const float framebufferScale[2], uint32_t width, uint32_t height, UiScissor& scissor);
    // The capacity to grow a UI buffer to: the next power of two that holds size bytes, at least minimum.
    uint64_t                            growUiCapacity(uint64_t size, uint64_t minimum);
    // The heat colors of a count (lib/heat.slang) as a row of swatches, in the current ImGui window.
    void                                drawHeatKey();
    // The layout RASM's code was compiled with.
    UiLayout                            getUiLayout();
    // "" when other is RASM's layout, else the first difference, naming other's copy as owner ("the caller's").
    std::string                         findUiLayoutMismatch(const UiLayout& rasm, const UiLayout& other, const char* owner);

}
// clang-format on
