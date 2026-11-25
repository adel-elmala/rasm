// clang-format off
#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/render/resource.h"

#include <cstdint>

namespace rasm
{
    // TAA jitters a view's projection by a sub-pixel offset that repeats every TAA_JITTER_PHASES frames.
    constexpr uint32_t          TAA_JITTER_PHASES       = 8;
    // Mip bias of the material textures while a view is jittered: the frames blend into a sharper image than one.
    constexpr float             TAA_TEXTURE_LOD_BIAS    = -1.0f;
    // A camera that turns more than this in one frame (radians) has jumped: TAA drops its history.
    constexpr float             TAA_JUMP_ANGLE          = 0.5235988f;
    // A perspective camera that moves more than this many times its near plane in one frame has jumped.
    constexpr float             TAA_JUMP_NEAR_PLANES    = 100.0f;

    // What one TAA pass remembers between frames, to know when its history no longer fits this frame. Renderer::render
    // keeps it with the view's history target, so a new graph goes on with the history.
    struct TaaState
    {
        glm::uvec2              size                    = glm::uvec2(0);
        uint32_t                version                 = 0;    // the history's RenderTarget::version: a resize or a new swapchain makes new, unwritten textures
        uint32_t                historyResets           = 0;
        glm::dvec3              position                = glm::dvec3(0.0);
        glm::quat               rotation                = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        bool                    valid                   = false;
    };

    // A TAA history target for a source target: its size, one SCENE_COLOR_FORMAT storage color with a copy per frame slot.
    RenderTargetDesc            getTaaHistoryDesc(const RenderTargetDesc& source);
    // The offset of frame `frame`, in pixels from the pixel's center (-0.5 to 0.5): the Halton (2, 3) sequence.
    glm::vec2                   getTaaJitter(uint32_t frame);
    // Whether the camera moved too far since the last frame for its history to help: a turn above TAA_JUMP_ANGLE, or
    // a move above jumpDistance (TAA_JUMP_NEAR_PLANES near planes for a perspective camera, half the view's width
    // for an orthographic one).
    bool                        isCameraJump(const glm::dvec3& previousPosition, const glm::quat& previousRotation,
                                             const glm::dvec3& position, const glm::quat& rotation, float jumpDistance);
}
// clang-format on
