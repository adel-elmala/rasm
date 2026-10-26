// clang-format off
#pragma once

#include "glm/glm.hpp"
#include "rasm/types.h"

namespace rasm
{
    constexpr uint32_t MAX_LIGHTS = 8;

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
        float       intensity   = 1.0f;
        float       range       = 10.0f;
        glm::vec3   color       = glm::vec3(1.0f);
        glm::vec3   position    = glm::vec3(1.0f);
        glm::vec3   direction   = glm::vec3(0.0f, -1.0f, 0.0f);
    };

}
// clang-format on