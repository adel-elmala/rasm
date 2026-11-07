// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/entity.h"
#include "rasm/light.h"

#include <cstddef>
#include <string>
#include <vector>

namespace rasm
{

    constexpr uint32_t  MAX_ENTITIES_PER_SCENE      = MAX_ENTITIES;
    constexpr uint32_t  MAX_CAMERAS_PER_SCENE       = 8;
    constexpr uint32_t  MAX_LIGHTS_PER_SCENE        = MAX_LIGHTS;
    constexpr uint32_t  MAX_SCENES                  = 16;
    constexpr uint32_t  MAX_SCENE_DRAWS_PER_FRAME   = 8;

    struct Scene
    {
        SceneHandle                 handle;
        std::vector<EntityHandle>   entities;
        std::vector<CameraHandle>   cameras;
        std::vector<LightHandle>    lights;
    };

}
// clang-format on
