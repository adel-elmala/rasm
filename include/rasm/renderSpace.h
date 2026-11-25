// clang-format off
#pragma once

#include "glm/glm.hpp"

namespace rasm
{

    // A view draws in render space: the world moved so that the view's camera is at the origin. Its origin is the
    // camera's position (getCameraTransform) in the frame it drew; positions near the camera are small there, so
    // floats keep them precise anywhere in a large world.
    glm::vec3                   toRenderPosition(const glm::dvec3& worldPosition, const glm::dvec3& origin);
    glm::dvec3                  toWorldPosition(const glm::vec3& renderPosition, const glm::dvec3& origin);

}
// clang-format on
