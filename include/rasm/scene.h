// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/entity.h"
#include "rasm/mesh.h"

#include <cstddef>
#include <string>
#include <vector>

namespace rasm
{

    constexpr uint32_t MAX_ENTITIES_PER_SCENE       = MAX_ENTITIES;
    constexpr int MAX_CAMERAS_PER_SCENE             = 8;
    constexpr int MAX_LIGHTS_PER_SCENE              = 8;
    constexpr int MAX_SCENES                        = 16;
    constexpr uint32_t MAX_SCENE_DRAWS_PER_FRAME    = 8;

    struct Scene
    {
        SceneHandle                 handle;
        std::vector<EntityHandle>   entities;

        CameraHandle                cameras[MAX_CAMERAS_PER_SCENE];
        size_t                      cameraCount     = 0;

        LightHandle                 lights[MAX_LIGHTS_PER_SCENE];
        size_t                      lightCount      = 0;
    };

}
// clang-format on
