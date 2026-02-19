#include "rasm/core/math.h"

namespace rasm
{

    Transform::Transform(glm::vec3 position, glm::quat rotation, glm::vec3 scale)
        : position(position), rotation(rotation), scale(scale) {}
    Transform::~Transform() {}

    void Transform::setPosition(const glm::vec3 &position) {}
    void Transform::setRotation(const glm::quat &rotation) {}
    void Transform::setScale(const glm::vec3 &scale) {}

    void Transform::rotate(const glm::vec3 &axis, float angle) {}

}