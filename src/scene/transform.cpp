#include "rasm/transform.h"

#include "glm/gtc/matrix_transform.hpp"

namespace rasm
{
    Transform Transform::rotate(const glm::vec3 &axis, float radians)
    {
        if (glm::length(axis) == 0.0f)
        {
            return *this; // No rotation if the axis is zero-length
        }
        const glm::quat delta = glm::angleAxis(radians, glm::normalize(axis));
        rotation = delta * rotation;
        return *this;
    }

    // Scale, then rotate, then translate.
    glm::mat4 Transform::toMatrix() const
    {
        return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
    }

}
