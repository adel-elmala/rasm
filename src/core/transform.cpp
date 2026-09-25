#include "rasm/core/transform.h"

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

}
