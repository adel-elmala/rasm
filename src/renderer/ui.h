// clang-format off
#pragma once

#include "rasm/engine.h"
#include "rasm/handle.h"
#include "rasm/types.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct ImGuiContext;

namespace rasm
{

    // A UI texture's bindless slot is reused once no frame in flight can sample it.
    struct UiRelease
    {
        TextureHandle                   texture;
        uint32_t                        slot                = 0;
        uint64_t                        frame               = 0;    // the UI frame that last drew with it
    };

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

    struct Ui
    {
        ImGuiContext*                                   context                         = nullptr;
        bool                                            platform                        = false;    // the SDL3 backend runs
        bool                                            encoded                         = false;    // draws through the swapchain's UNORM view
        PipelineHandle                                  pipeline;
        UiSlot                                          slots[MAX_FRAMES_IN_FLIGHT];
        std::vector<UiDraw>                             draws;                                      // this frame's, recorded by the UI pass
        std::unordered_map<uint32_t, TextureHandle>     textures;                                   // ImGui's textures by bindless slot
        std::vector<uint32_t>                           freeSlots;                                  // bindless slots of released UI textures
        std::vector<UiRelease>                          releases;
        std::vector<uint8_t>                            expanded;                                   // an Alpha8 texture's pixels as RGBA8
        std::function<void()>                           callback;
        FrameStats                                      stats;                                      // what the RASM window shows, renewed every 250 ms
        std::chrono::steady_clock::time_point           statsTime                       = {};
        std::string                                     graphDump;                                  // the last F3 dump
        uint64_t                                        frame                           = 0;        // UI frames built so far
        uint32_t                                        displaySize                     = 0;        // width | height << 16, in points
        bool                                            active                          = false;    // this frame built a UI
        bool                                            drawn                           = false;    // a UI pass drew it
        bool                                            fallbackLogged                  = false;
    };

    // The draw's clip rectangle in framebuffer pixels, clamped to the framebuffer; false when nothing is left.
    bool                                getUiScissor(const float clip[4], const float displayPos[2], const float framebufferScale[2], uint32_t width, uint32_t height, UiScissor& scissor);
    // The capacity to grow a UI buffer to: the next power of two that holds size bytes, at least minimum.
    uint64_t                            growUiCapacity(uint64_t size, uint64_t minimum);
    // Takes the releases whose frame is MAX_FRAMES_IN_FLIGHT or more UI frames before frame out of releases, into done.
    void                                takeUiReleases(std::vector<UiRelease>& releases, uint64_t frame, std::vector<UiRelease>& done);

}
// clang-format on
