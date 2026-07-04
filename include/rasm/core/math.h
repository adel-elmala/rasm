// clang-format off
#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

namespace rasm
{
    class Engine;

    class Transform
    {
    friend class Engine;
    public:
                        Transform(const glm::vec3 &position = glm::vec3(0.0f), const glm::quat &rotation = glm::quat(), const glm::vec3 &scale = glm::vec3(1.0f));
                        ~Transform();
        void            setPosition(const glm::vec3 &position);
        void            setRotation(const glm::quat &rotation);
        void            setScale(const glm::vec3 &scale);
        glm::vec3       getPosition() const;
        glm::quat       getRotation() const;
        glm::vec3       getScale() const;
        void            rotate(const glm::vec3 &axis, float radians);

    protected:
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 scale;
    };

}
// clang-format on