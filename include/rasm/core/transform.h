// clang-format off
#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

namespace rasm
{
    struct Transform
    {
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat();
        glm::vec3 scale    = glm::vec3(1.0f);

        Transform rotate(const glm::vec3 &axis, float radians);
    };
}
// clang-format on