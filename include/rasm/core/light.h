#pragma once

#include <stdint.h>

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
        uint32_t handle;
        LightType type;
    public:
        Light(LightType type = LightType::Point);
        ~Light();

        void setColor(const glm::vec3& color);
        void setIntensity(float intensity);
    };

}