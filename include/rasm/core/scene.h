// clang-format off
#pragma once

#include <string>
#include <optional>

#include "rasm/core/types.h"
#include "rasm/core/camera.h"
#include "rasm/core/light.h"

#include <unordered_map>

namespace rasm {

    constexpr int MAX_ENTITIES_PER_SCENE = 128;
    constexpr int MAX_CAMERAS_PER_SCENE = 8;
    constexpr int MAX_LIGHTS_PER_SCENE = 8;
    constexpr int MAX_SCENES = 16;

    struct Scene
    {
        SceneHandle handle;
        EntityHandle entities[MAX_ENTITIES_PER_SCENE];
        size_t entityCount = 0;

        CameraHandle cameras[MAX_CAMERAS_PER_SCENE];
        size_t cameraCount = 0;

        LightHandle lights[MAX_LIGHTS_PER_SCENE];
        size_t lightCount = 0;
    };

    struct SceneArray
    {
        Scene scenes[MAX_SCENES]{};
        size_t count = 0;
    };

    struct CompiledScene
    {
        std::unordered_map<MaterialHandle, std::unordered_set<EntityHandle, HandleHash>, HandleHash> materialToMeshes;
        std::unordered_map<MaterialHandle, PipelineHandle, HandleHash> materialToPipeline;
        std::unordered_map<EntityHandle, BufferHandle, HandleHash> meshData;
    };
}
// clang-format on