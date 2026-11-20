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
    constexpr uint32_t  MAX_SCENE_VIEWS             = 16;
    constexpr uint32_t  MAX_SHADOW_CASCADES         = 4;
    constexpr uint32_t  MAX_VIEW_SHADOWS            = 4;

    struct Scene
    {
        SceneHandle                 handle;
        std::vector<EntityHandle>   entities;
        std::vector<CameraHandle>   cameras;
        std::vector<LightHandle>    lights;
    };

    struct ShadowDesc
    {
        LightHandle                 light;
        uint32_t                    cascadeCount                        = 4;        // directional lights
        uint32_t                    size                                = 2048;
        float                       distance                            = 50.0f;    // from the camera (directional), or from the light
        float                       splitLambda                         = 0.7f;     // directional lights
    };

    struct SceneShadow
    {
        LightHandle                 light;
        RenderTargetHandle          maps[MAX_SHADOW_CASCADES]           = {};       // the cascades, a spot light's map, or a point light's cube
        uint32_t                    mapCount                            = 0;
        float                       distance                            = 0.0f;
        float                       splitLambda                         = 0.0f;

        bool                        operator==(const SceneShadow&) const = default;
    };

}
// clang-format on
