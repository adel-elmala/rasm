// clang-format off
#pragma once

#include "rasm/renderSpace.h"

namespace rasm
{

    // A double as two floats: high is the nearest float, low the rest. The GPU subtracts a view's origin from each half
    // on its own (toRenderSpace in shaders/lib/core.slang), so the difference keeps the double's precision.
    struct SplitPosition
    {
        glm::vec3               high;
        glm::vec3               low;
    };

    SplitPosition               splitPosition(const glm::dvec3& position);
    // The rows of an affine matrix, their w the translation's high part, and its low part.
    void                        writeWorldRows(const glm::dmat4& world, glm::vec4 rows[3], glm::vec3& translationLow);
    // The rows of the frame before's matrix, their w its translation minus this frame's.
    void                        writePreviousRows(const glm::dmat4& previous, const glm::dmat4& world, glm::vec4 rows[3]);
    // A world-space view (or projection * view) as the matrix that takes render-space points of origin instead.
    glm::mat4                   toRenderMatrix(const glm::dmat4& worldMatrix, const glm::dvec3& origin);
    // A plane (inside where dot(xyz, p) + w >= 0) of the render space of one origin, in that of another.
    glm::vec4                   moveRenderPlane(const glm::vec4& plane, const glm::dvec3& from, const glm::dvec3& to);

}
// clang-format on
