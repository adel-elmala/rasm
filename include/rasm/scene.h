// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/entity.h"
#include "rasm/mesh.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace rasm
{

    constexpr int MAX_ENTITIES_PER_SCENE = 1024;
    constexpr int MAX_CAMERAS_PER_SCENE = 8;
    constexpr int MAX_LIGHTS_PER_SCENE = 8;
    constexpr int MAX_SCENES = 16;
    constexpr uint32_t MAX_SCENE_DRAWS_PER_FRAME = 8;

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

    struct SceneDraw
    {
        const Entity*   entity;
        MeshGeometry    geometry;
        glm::mat4       transform;
        MaterialHandle  material;
    };

    struct PreparedScene
    {
        BufferHandle                                       megaModelDataBuffer[MAX_FRAMES_IN_FLIGHT];
        BufferHandle                                       drawInfoBuffer;
        BufferHandle                                       sceneDataBuffer[MAX_FRAMES_IN_FLIGHT]; // one SceneData per drawScene of a frame
        PipelineHandle                                     uberMaterialPipeline;
        std::vector<std::pair<EntityHandle, glm::mat4>>    modelUniforms;
        uint32_t                                           drawInfoCount;
    };

}
// clang-format on