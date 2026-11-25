// clang-format off
#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

namespace rasm
{
    struct Transform
    {
        glm::dvec3  position = glm::dvec3(0.0);                // world space, in doubles
        glm::quat   rotation = glm::identity<glm::quat>();
        glm::vec3   scale    = glm::vec3(1.0f);

        Transform   rotate(const glm::vec3& axis, float radians);
        glm::dmat4  toMatrix() const;
    };
}
// clang-format on