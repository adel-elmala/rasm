#include "rasm/core/math.h"

namespace rasm
{

    Transform::Transform(glm::vec3 position, glm::quat rotation, glm::vec3 scale)
        : position(position), rotation(rotation), scale(scale) {}
    Transform::~Transform() {}

    void Transform::setPosition(const glm::vec3 &value) { position = value; }
    void Transform::setRotation(const glm::quat &value) { rotation = value; }
    void Transform::setScale(const glm::vec3 &value) { scale = value; }

    void Transform::rotate(const glm::vec3 &axis, float angle) {
        const glm::quat delta = glm::angleAxis(angle, glm::normalize(axis));
        rotation = delta * rotation;
    }

}
