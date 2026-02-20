#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

namespace rasm
{

    class Transform
    {
    public:
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 scale;

        // Position, rotation, scale
        Transform(const glm::vec3& position = glm::vec3(0.0f), const glm::quat& rotation = glm::quat(), const glm::vec3& scale = glm::vec3(1.0f));
        ~Transform();

        void setPosition(const glm::vec3 &position);
        void setRotation(const glm::quat &rotation);
        void setScale(const glm::vec3 &scale);

        void rotate(const glm::vec3 &axis, float radians);
    };

}