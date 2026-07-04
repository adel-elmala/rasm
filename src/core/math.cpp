#include "rasm/core/math.h"

namespace rasm
{

    Transform::Transform(const glm::vec3 &position, const glm::quat &rotation, const glm::vec3 &scale)
    {
        this->position = position;
        this->rotation = rotation;
        this->scale = scale;
    }
    Transform::~Transform() {}

    void Transform::setPosition(const glm::vec3 &value) { position = value; }
    void Transform::setRotation(const glm::quat &value) { rotation = value; }
    void Transform::setScale(const glm::vec3 &value) { scale = value; }

    glm::vec3 Transform::getPosition() const { return position; }
    glm::quat Transform::getRotation() const { return rotation; }
    glm::vec3 Transform::getScale() const { return scale; }

    void Transform::rotate(const glm::vec3 &axis, float radians)
    {
        if (glm::length(axis) == 0.0f)
        {
            return; // No rotation if the axis is zero-length
        }
        const glm::quat delta = glm::angleAxis(radians, glm::normalize(axis));
        rotation = delta * rotation;
    }

}
