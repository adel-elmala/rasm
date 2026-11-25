// clang-format off
#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include <cstdint>

namespace rasm
{

    // A reference grid a camera draws over its view's opaque scene, under its see-through draws (Renderer::setCameraGrid).
    // The plane is the local XZ plane of origin and orientation. Its lines come in levels of cellSize x subdivisions^k:
    // each pixel shows the levels whose cells are wide enough there, so the grid stays crisp at any zoom, in perspective
    // and orthographic views alike, and anywhere in a large world.
    struct Grid
    {
        bool        enabled         = false;
        glm::dvec3  origin          = glm::dvec3(0.0);                  // world space: a point of the plane, where the axes cross
        glm::quat   orientation     = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // the plane's X and Z axes and its normal +Y
        double      cellSize        = 1.0;                              // the finest level's cells, in world units: a double, so lines far out sit on exact multiples
        uint32_t    subdivisions    = 10;                               // each level's cells hold this many of the level below a side, 2 or more
        glm::vec2   extent          = glm::vec2(0.0f);                  // half the grid's size along X and Z; 0: endless along that axis
        glm::vec4   minorColor      = glm::vec4(0.5f, 0.5f, 0.5f, 0.35f); // linear, with alpha, through exposure and tone mapping
        glm::vec4   majorColor      = glm::vec4(0.5f, 0.5f, 0.5f, 0.7f);
        float       lineWidth       = 1.0f;                             // pixels
        float       majorLineWidth  = 1.5f;                             // pixels: every subdivisions-th line
        bool        axes            = true;                             // the plane's X and Z axes, through origin
        glm::vec4   xAxisColor      = glm::vec4(0.85f, 0.12f, 0.1f, 1.0f); // the line along X
        glm::vec4   zAxisColor      = glm::vec4(0.1f, 0.28f, 0.9f, 1.0f);  // the line along Z (green for a Y axis)
        float       fadeDistance    = 0.0f;                             // a perspective view's lines fade out toward this distance; 0: no fade

        bool        operator==(const Grid&) const = default;
    };

}
// clang-format on
