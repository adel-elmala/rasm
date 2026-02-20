#include "rasm/core/math.h"

namespace rasm
{

    Transform::Transform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
        : position(position), rotation(rotation), scale(scale) {}
    Transform::~Transform() {}

    void Transform::setPosition(const glm::vec3 &value) { position = value; }
    void Transform::setRotation(const glm::quat &value) { rotation = value; }
    void Transform::setScale(const glm::vec3 &value) { scale = value; }

    void Transform::rotate(const glm::vec3 &axis, float radians) {
        if (glm::length(axis) == 0.0f) {
            return; // No rotation if the axis is zero-length
        }
        const glm::quat delta = glm::angleAxis(radians, glm::normalize(axis));
        rotation = delta * rotation;
    }

}
