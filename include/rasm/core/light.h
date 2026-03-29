// clang-format off
#pragma once

#include "glm/glm.hpp"

namespace rasm
{

    enum class LightType
    {
        DIRECTIONAL,
        POINT,
        SPOT
    };
    
    class Engine;

    class Light
    {
    friend class Engine;
    public:
             Light(LightType type = LightType::POINT);
             ~Light();
        void setColor(const glm::vec3 &color);
        void setIntensity(float intensity);

    protected:
        LightType   type;
        float       intensity   = 1.0f;
        glm::vec3   color       = glm::vec3(1.0f);
    };

}
// clang-format on