// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/view.h"

#include "glm/glm.hpp"

#include <cstdint>

namespace rasm
{

    // A pick's rect may be this many pixels on each side at most.
    constexpr uint32_t              MAX_PICK_SIZE           = 256;

    // What a pick looks through: each kind left out draws nothing in the pick, so what is behind it is picked.
    constexpr uint32_t              PICK_SKIP_MESHES        = 1u << 0;  // opaque and cut-out (MASK) meshes, not terrain
    constexpr uint32_t              PICK_SKIP_BLEND         = 1u << 1;  // see-through (BLEND) meshes, not vector features
    constexpr uint32_t              PICK_SKIP_TERRAIN       = 1u << 2;
    constexpr uint32_t              PICK_SKIP_VECTORS       = 1u << 3;  // lines, polygons and points
    constexpr uint32_t              PICK_SKIP_LABELS        = 1u << 4;
    constexpr uint32_t              PICK_SKIP_OVERLAYS      = 1u << 5;  // VectorDepth::OVERLAY features, labels too
    constexpr uint32_t              PICK_SKIP_POINTS        = 1u << 6;  // point cloud chunks
    constexpr uint32_t              PICK_SKIP_GLYPHS        = 1u << 7;
    constexpr uint32_t              PICK_SKIP_ALL           = (1u << 8) - 1;

    // What a hit is.
    constexpr uint32_t              PICK_HIT_FEATURE        = 1u << 0;  // a vector feature (a label too): PickHit::feature is its id
    constexpr uint32_t              PICK_HIT_LABEL          = 1u << 1;
    constexpr uint32_t              PICK_HIT_OVERLAY        = 1u << 2;
    constexpr uint32_t              PICK_HIT_TERRAIN        = 1u << 3;
    constexpr uint32_t              PICK_HIT_BACK_FACE      = 1u << 4;  // the back of a double-sided surface; the normal faces the camera
    constexpr uint32_t              PICK_HIT_POINT          = 1u << 5;  // a point of a point cloud chunk: PickHit::feature is its index (with PICK_HIT_FEATURE)
    constexpr uint32_t              PICK_HIT_GLYPH          = 1u << 6;

    // A question to a view: what is under these pixels (from the view's top-left corner, in its pixels)?
    struct PickDesc
    {
        ViewPixels                  rect            = {.x = 0, .y = 0, .width = 1, .height = 1};
        float                       minAlpha        = 0.5f;     // a see-through surface (BLEND, every vector feature) is picked where its alpha is at least this
        uint32_t                    skip            = 0;        // PICK_SKIP_*
    };

    // One thing the rect covers: an entity's sub-mesh, and for a vector feature its id.
    struct PickHit
    {
        EntityHandle                entity;                     // the entity the frame drew, which may be destroyed since
        uint32_t                    subMesh         = 0;        // of the entity's mesh (a glTF primitive)
        uint32_t                    feature         = 0;        // a vector feature's id or a point's index (PICK_HIT_FEATURE), else 0
        uint32_t                    flags           = 0;        // PICK_HIT_*
        uint32_t                    pixels          = 0;        // of the rect that show it
        uint32_t                    x               = 0;        // its pixel nearest the rect's centre, in the view's pixels
        uint32_t                    y               = 0;
        glm::dvec3                  position        = glm::dvec3(0.0);  // world space, at that pixel's centre
        glm::vec3                   normal          = glm::vec3(0.0f, 1.0f, 0.0f);  // world space, the surface's (not its normal map's)
        double                      distance        = 0.0;      // from the camera to position
    };

}
// clang-format on
