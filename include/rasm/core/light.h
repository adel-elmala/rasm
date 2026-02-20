#pragma once

#include "glm/glm.hpp"

namespace rasm {

    enum class LightType
    {
        Directional,
        Point,
        Spot
    };

    class Light
    {
    private:
        LightType type;

    public:
        Light(LightType type = LightType::Point);
        ~Light();

        void setColor(const glm::vec3& color);
        void setIntensity(float intensity);
    };

}
