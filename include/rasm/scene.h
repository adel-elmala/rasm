// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/mesh.h"

#include <string>
#include <unordered_map>

namespace rasm {

    constexpr int MAX_ENTITIES_PER_SCENE = 1024;
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

    struct CompiledScene
    {
        std::unordered_map<MaterialHandle, std::unordered_set<EntityHandle, HandleHash>, HandleHash> materialToMeshes;
        std::unordered_map<MaterialHandle, PipelineHandle, HandleHash>                               materialToPipeline;
        std::unordered_map<EntityHandle, BufferHandle, HandleHash>                                   meshData;
    };

    struct PreparedScene
    {
        BufferHandle                                       megaVertexBuffer;
        BufferHandle                                       megaIndexBuffer;
        BufferHandle                                       megaModelDataBuffer[MAX_FRAMES_IN_FLIGHT];
        BufferHandle                                       drawInfoBuffer;
        BufferHandle                                       sceneDataBuffer[MAX_FRAMES_IN_FLIGHT];
        PipelineHandle                                     uberMaterialPipeline;
        std::vector<std::pair<EntityHandle, glm::mat4>>    modelUniforms;
        uint32_t                                           drawInfoCount;
    };

}
// clang-format on