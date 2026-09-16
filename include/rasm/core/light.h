// clang-format off
#pragma once

#include "glm/glm.hpp"
#include "rasm/core/types.h"

namespace rasm
{

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
        glm::vec3   color       = glm::vec3(1.0f);
        glm::vec3   position    = glm::vec3(1.0f);
        glm::vec3   direction   = glm::vec3(0.0f, -1.0f, 0.0f);
    };

}
// clang-format on