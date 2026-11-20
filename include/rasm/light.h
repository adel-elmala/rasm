// clang-format off
#pragma once

#include "glm/glm.hpp"
#include "rasm/types.h"

namespace rasm
{
    constexpr uint32_t MAX_LIGHTS = 8;
    constexpr float    SPOT_OUTER_CONE_ANGLE = 0.785398163f;    // pi / 4, glTF's default

    enum class LightType
    {
        DIRECTIONAL,
        POINT,
        SPOT
    };

    struct Light
    {
        LightHandle handle;
        LightType   type;
        float       intensity       = 1.0f;
        float       range           = 10.0f;
        glm::vec3   color           = glm::vec3(1.0f);
        glm::vec3   position        = glm::vec3(1.0f);
        glm::vec3   direction       = glm::vec3(0.0f, -1.0f, 0.0f);
        float       innerConeAngle  = 0.0f;                     // radians from the direction, spot lights
        float       outerConeAngle  = SPOT_OUTER_CONE_ANGLE;
    };

}
// clang-format on