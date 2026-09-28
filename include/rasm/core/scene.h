// clang-format off
#pragma once

#include "rasm/core/types.h"

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
        std::unordered_map<MaterialHandle, PipelineHandle, HandleHash> materialToPipeline;
        std::unordered_map<EntityHandle, BufferHandle, HandleHash> meshData;
    };

    struct PreparedScene
    {
        BufferHandle megaVertexBuffer;
        BufferHandle megaIndexBuffer;
        BufferHandle megaModelMatsBuffer;
        BufferHandle drawInfoBuffer;
        BufferHandle sceneDataBuffer;
        PipelineHandle uberMaterialPipeline;
        uint32_t drawInfoCount;
    };

    enum  VertexFormat : uint8_t { Full, PosOnly };

    struct VertexFull       { glm::vec3 pos;  glm::vec3 normal; glm::vec2 uv; };
    struct VertexPosOnly    { glm::vec3 pos; };

    struct DrawInfo
    {
        // Information about a single draw call
        uint32_t        indexCount;
        uint32_t        instanceCount;
        uint32_t        firstIndex;
        int32_t         vertexOffset;
        uint32_t        firstInstance;
        VertexFormat    vertFormat;
    };

    struct SceneData
    {
        glm::mat4   projection;
        glm::mat4   view;
        uint64_t    vertsPtr;
        uint64_t    drawInfoPtr;
        uint64_t    modeldataPtr;
    };
}
// clang-format on