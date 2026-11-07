// clang-format off
#pragma once

#include "glm/glm.hpp"

#include "rasm/light.h"
#include "rasm/mesh.h"

#include <cstddef>
#include <cstdint>

namespace rasm::gfx
{

    // A draw list has one run per cull mode (CullMode in rasm/types.h). Must match shaders/lib/core.slang.
    constexpr uint32_t      MAX_DRAW_RUNS       = 3;

    // Selects how the uber shader's fragMain shades a model. Must match shaders/lib/core.slang.
    enum class MaterialId : uint32_t { PBR, UNLIT };

    // Must match rasm::Vertex (static asserted in scene.cpp) and shaders/lib/core.slang.
    struct VertexFull       { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv[MAX_VERTEX_UV_SETS]; glm::vec4 tangent; };
    struct VertexPosOnly    { glm::vec3 pos; };

    enum class LightType : uint32_t { DIRECTIONAL, POINT };

    struct Light
    {
        glm::vec3   position;
        float       intensity;
        glm::vec3   color;
        float       range;      // Only used for point lights
        glm::vec3   direction;  // Only used for directional lights
        LightType   type;
    };


    struct DrawInfo
    {
        uint32_t        indexCount;
        uint32_t        instanceCount;
        uint32_t        firstIndex;
        int32_t         vertexOffset;
        uint32_t        firstInstance;
    };

    struct MetallicRoughnessMaterial
    {
        glm::vec3       baseColorFactor;
        uint32_t        baseColorTextureIndex;
        uint32_t        baseColorTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        uint32_t        metallicRoughnessTextureIndex;
        uint32_t        metallicRoughnessTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           metallicFactor;
        float           roughnessFactor;
    };

    struct NormalMaterial
    {
        uint32_t        normalTextureIndex;
        uint32_t        normalTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           scale;
    };

    struct OcclusionMaterial
    {
        uint32_t        occlusionTextureIndex;
        uint32_t        occlusionTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        float           strength;
    };

    struct EmissiveMaterial
    {
        uint32_t        emissiveTextureIndex;
        uint32_t        emissiveTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
        glm::vec3       emissiveFactor;
    };

    struct Material
    {
        MetallicRoughnessMaterial   pbr;
        NormalMaterial              normal;
        OcclusionMaterial           occlusion;
        EmissiveMaterial            emissive;
    };

    struct MaterialData
    {
        Material        material;
        MaterialId      materialId;
    };

    struct InstanceData
    {
        glm::mat4       worldMatrix;
        glm::mat3       normalMatrix;
        uint32_t        geometryIndex;
        uint32_t        materialIndex;
    };

    struct GeometryLod
    {
        uint32_t        firstIndex;
        uint32_t        indexCount;
        float           error;
    };

    struct GeometryData
    {
        int32_t         vertexOffset;
        glm::vec3       boundsCenter;
        float           boundsRadius;
        uint32_t        lodCount;
        GeometryLod     lods[MAX_MESH_LODS];
    };

    struct SceneData
    {
        glm::mat4        projection;
        glm::mat4        view;
        glm::vec3        cameraWorldPosition;
        uint32_t         numLights;
        Light            lights[MAX_LIGHTS];
        uint64_t         vertsPtr;
        uint64_t         drawInfoPtr;
        uint64_t         instancesPtr;
        uint64_t         materialsPtr;
        uint64_t         geometryPtr;
        uint64_t         allDrawsPtr;
        uint64_t         drawCountsPtr;
        uint64_t         cullSlotsPtr;
        uint64_t         groupCountsPtr;
        uint64_t         cullStatsPtr;
        uint64_t         visibilityPtr;
        uint64_t         hizPtr;
        glm::vec4        frustumPlanes[6];
        uint32_t         drawCount;
        uint32_t         runCount;
        uint32_t         runFirst[MAX_DRAW_RUNS];
        uint32_t         hizLevelCount;
        uint32_t         depthTexture;
        uint32_t         viewportWidth;
        uint32_t         viewportHeight;
        float            lodErrorPixels;
        int32_t          forcedLod;
        uint32_t         padding;
    };

    struct PushConstants
    {
        uint64_t         scene;
        uint32_t         firstDraw;
        uint32_t         argument;
    };

    // These structs are read by shaders/passes/rasm.slang through buffer-device-address pointers,
    // compiled with GLSL scalar layout. Any drift here silently corrupts the GPU reads (a wrong
    // vertsPtr/instancesPtr turns into a device lost on the first draw).
    static_assert(sizeof(Light) == 48, "Light must match the shader's scalar layout");
    static_assert(sizeof(VertexFull) == 64, "VertexFull must match the shader's scalar layout");
    static_assert(offsetof(VertexFull, tangent) == 48, "VertexFull must match the shader's scalar layout");
    static_assert(sizeof(MetallicRoughnessMaterial) == 36, "MetallicRoughnessMaterial must match the shader's scalar layout");
    static_assert(sizeof(Material) == 80, "Material must match the shader's scalar layout");
    static_assert(sizeof(MaterialData) == 84, "MaterialData must match the shader's scalar layout");
    static_assert(offsetof(MaterialData, materialId) == 80, "MaterialData must match the shader's scalar layout");
    static_assert(sizeof(InstanceData) == 108, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, normalMatrix) == 64, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, geometryIndex) == 100, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, materialIndex) == 104, "InstanceData must match the shader's scalar layout");
    static_assert(sizeof(GeometryLod) == 12, "GeometryLod must match the shader's scalar layout");
    static_assert(offsetof(GeometryLod, error) == 8, "GeometryLod must match the shader's scalar layout");
    static_assert(sizeof(GeometryData) == 24 + 12 * MAX_MESH_LODS, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, boundsCenter) == 4, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, boundsRadius) == 16, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, lodCount) == 20, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, lods) == 24, "GeometryData must match the shader's scalar layout");
    static_assert(sizeof(SceneData) == 768, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lights) == 144, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, vertsPtr) == 528, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, instancesPtr) == 544, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, geometryPtr) == 560, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, allDrawsPtr) == 568, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, drawCountsPtr) == 576, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, cullSlotsPtr) == 584, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, groupCountsPtr) == 592, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, cullStatsPtr) == 600, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, visibilityPtr) == 608, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, hizPtr) == 616, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, frustumPlanes) == 624, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, drawCount) == 720, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runCount) == 724, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runFirst) == 728, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, hizLevelCount) == 740, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, depthTexture) == 744, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, viewportWidth) == 748, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, viewportHeight) == 752, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodErrorPixels) == 756, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, forcedLod) == 760, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, padding) == 764, "SceneData must match the shader's scalar layout");
    static_assert(sizeof(PushConstants) == PUSH_CONSTANTS_SIZE, "PushConstants must fill the pipelines' push-constant range");


} // namespace rasm::gfx
// clang-format on