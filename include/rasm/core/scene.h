// clang-format off
#pragma once

#include "rasm/core/types.h"
#include "rasm/core/mesh.h"

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

    // uint32_t underlying type: DrawInfo is read through a buffer-device-address pointer in the
    // shader, and 8-bit loads there would require the storageBuffer8BitAccess feature.
    enum  VertexFormat : uint32_t { Full, PosOnly };

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
        uint32_t        textureIndex; // Index of the texture to use for this draw call
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

    // These structs are read by shaders/common/rasm.slang through buffer-device-address pointers,
    // compiled with scalar layout. Any drift here silently corrupts the GPU reads.
    static_assert(sizeof(VertexFull) == 32, "VertexFull must match the shader's scalar layout");
    static_assert(sizeof(VertexFull) == sizeof(Vertex), "The mega vertex buffer is filled with Vertex");
    static_assert(sizeof(DrawInfo) == 28, "DrawInfo must match the shader's scalar layout");
    static_assert(offsetof(DrawInfo, firstInstance) == 16, "DrawInfo must stay VkDrawIndexedIndirectCommand compatible");
    static_assert(sizeof(SceneData) == 152, "SceneData must match the shader's scalar layout");
}
// clang-format on