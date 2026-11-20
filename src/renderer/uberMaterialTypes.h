// clang-format off
#pragma once

#include "glm/glm.hpp"

#include "rasm/light.h"
#include "rasm/mesh.h"
#include "rasm/scene.h"

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

    // Each shadowed light of a view has up to MAX_SHADOW_CASCADES maps. Must match shaders/lib/core.slang.
    constexpr uint32_t      MAX_SHADOW_MAPS     = MAX_VIEW_SHADOWS * MAX_SHADOW_CASCADES;

    enum class LightType : uint32_t { DIRECTIONAL, POINT, SPOT };

    struct Light
    {
        glm::vec3   position;       // point and spot lights
        float       intensity;
        glm::vec3   color;
        float       range;          // point and spot lights
        glm::vec3   direction;      // directional and spot lights
        LightType   type;
        float       spotScale;      // the cone's falloff: saturate(dot(direction, -l) * spotScale + spotOffset)
        float       spotOffset;
        int32_t     shadowMap;      // the light's first map in SceneData::shadowMaps, -1 without a shadow
        uint32_t    shadowMapCount; // its cascades, or 1 for a spot or a point light (a cube)
    };

    struct ShadowMap
    {
        glm::mat4   matrix;         // world to clip: a cascade's or a spot's projection * view; a point light's face projection
        uint32_t    texture;        // bindless index of the depth; a point light's is a cube
        float       split;          // a cascade's: the view depth where it ends
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
        ShadowMap        shadowMaps[MAX_SHADOW_MAPS];
        float            lodPixelsPerUnit;
        glm::vec4        lodDepthPlane;
    };

    struct PushConstants
    {
        uint64_t         scene;
        uint32_t         firstDraw;
        uint32_t         argument;
    };

    // The tone-map pass's own push constants (shaders/passes/toneMap.slang).
    struct ToneMapPushConstants
    {
        uint32_t         source;
        float            exposure;
    };

    // These structs are read by shaders/passes/rasm.slang through buffer-device-address pointers,
    // compiled with GLSL scalar layout. Any drift here silently corrupts the GPU reads (a wrong
    // vertsPtr/instancesPtr turns into a device lost on the first draw).
    static_assert(sizeof(Light) == 64, "Light must match the shader's scalar layout");
    static_assert(offsetof(Light, spotScale) == 48, "Light must match the shader's scalar layout");
    static_assert(offsetof(Light, shadowMap) == 56, "Light must match the shader's scalar layout");
    static_assert(sizeof(ShadowMap) == 72, "ShadowMap must match the shader's scalar layout");
    static_assert(offsetof(ShadowMap, texture) == 64, "ShadowMap must match the shader's scalar layout");
    static_assert(offsetof(ShadowMap, split) == 68, "ShadowMap must match the shader's scalar layout");
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
    static_assert(sizeof(SceneData) == 2064, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lights) == 144, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, vertsPtr) == 656, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, instancesPtr) == 672, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, geometryPtr) == 688, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, allDrawsPtr) == 696, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, drawCountsPtr) == 704, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, cullSlotsPtr) == 712, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, groupCountsPtr) == 720, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, cullStatsPtr) == 728, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, visibilityPtr) == 736, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, hizPtr) == 744, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, frustumPlanes) == 752, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, drawCount) == 848, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runCount) == 852, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runFirst) == 856, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, hizLevelCount) == 868, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, depthTexture) == 872, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, viewportWidth) == 876, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, viewportHeight) == 880, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodErrorPixels) == 884, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, forcedLod) == 888, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, shadowMaps) == 892, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodPixelsPerUnit) == 2044, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodDepthPlane) == 2048, "SceneData must match the shader's scalar layout");
    static_assert(sizeof(PushConstants) == PUSH_CONSTANTS_SIZE, "PushConstants must fill the pipelines' push-constant range");
    static_assert(sizeof(ToneMapPushConstants) == 8, "ToneMapPushConstants must match the shader's scalar layout");
    static_assert(offsetof(ToneMapPushConstants, exposure) == 4, "ToneMapPushConstants must match the shader's scalar layout");


} // namespace rasm::gfx
// clang-format on