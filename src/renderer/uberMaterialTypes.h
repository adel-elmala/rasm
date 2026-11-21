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

    // A draw list has a run per alpha mode and cull mode, and one blend run, which is not culled by face. Must match
    // shaders/lib/core.slang.
    constexpr uint32_t      MAX_DRAW_RUNS       = 7;
    // The texture slots with a KHR_texture_transform each. Must match shaders/lib/core.slang.
    constexpr uint32_t      TEXTURE_TRANSFORMS  = 5;
    enum class TextureTransformSlot : uint32_t { BASE_COLOR, METALLIC_ROUGHNESS, NORMAL, OCCLUSION, EMISSIVE };
    // MaterialData::flags. Must match shaders/lib/core.slang.
    constexpr uint32_t      MATERIAL_DOUBLE_SIDED       = 1u << 0;
    constexpr uint32_t      MATERIAL_TEXTURE_TRANSFORM  = 1u << 1;  // a texture has a KHR_texture_transform that is not the identity

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
        glm::vec4       baseColorFactor;
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

    // uv' = (dot(matrix.xy, uv), dot(matrix.zw, uv)) + offset
    struct TextureTransform
    {
        glm::vec4       matrix;
        glm::vec2       offset;
    };

    struct Material
    {
        MetallicRoughnessMaterial   pbr;
        NormalMaterial              normal;
        OcclusionMaterial           occlusion;
        EmissiveMaterial            emissive;           // emissiveFactor holds KHR_materials_emissive_strength too
    };

    struct MaterialData
    {
        Material            material;
        MaterialId          materialId;
        AlphaMode           alphaMode;
        float               alphaCutoff;
        uint32_t            flags;
        TextureTransform    textureTransforms[TEXTURE_TRANSFORMS];  // read only with MATERIAL_TEXTURE_TRANSFORM
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
        int32_t         colorOffset;        // the sub-mesh's first vertex color in the color pool, -1 without colors
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
        uint64_t         colorsPtr;
        uint64_t         sortKeysPtr;
        uint64_t         sortDrawsPtr;
        glm::vec4        frustumPlanes[6];
        uint32_t         drawCount;
        uint32_t         runCount;
        uint32_t         runFirst[MAX_DRAW_RUNS];
        uint32_t         blendRun;                  // the run drawn by the Transparent pass, MAX_DRAW_RUNS without one
        uint32_t         sortCapacity;
        uint32_t         hizLevelCount;
        uint32_t         depthTexture;
        uint32_t         viewportWidth;
        uint32_t         viewportHeight;
        float            lodErrorPixels;
        int32_t          forcedLod;
        ShadowMap        shadowMaps[MAX_SHADOW_MAPS];
        float            lodPixelsPerUnit;
        glm::vec4        lodDepthPlane;
        uint32_t         environmentSpecular;       // bindless cube; INVALID_TEXTURE_INDEX: no environment, a constant ambient
        uint32_t         environmentSpecularMips;
        uint32_t         environmentIrradiance;     // bindless cube
        uint32_t         environmentSky;            // bindless cube drawn behind the scene; INVALID_TEXTURE_INDEX: no sky
        uint32_t         brdfLut;                   // bindless 2D
        float            environmentIntensity;
        glm::vec2        environmentRotation;       // cos and sin of the environment's turn around +Y
    };

    struct PushConstants
    {
        uint64_t         scene;
        uint32_t         firstDraw;
        uint32_t         argument;
    };

    // The environment bake's push constants (shaders/passes/environment.slang). source is a bindless texture or a
    // storage slot, target a storage slot, size the target mip's width.
    struct EnvironmentPushConstants
    {
        uint32_t         source;
        uint32_t         target;
        float            roughness;
        uint32_t         size;
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
    static_assert(sizeof(MetallicRoughnessMaterial) == 40, "MetallicRoughnessMaterial must match the shader's scalar layout");
    static_assert(sizeof(TextureTransform) == 24, "TextureTransform must match the shader's scalar layout");
    static_assert(sizeof(Material) == 84, "Material must match the shader's scalar layout");
    static_assert(sizeof(MaterialData) == 220, "MaterialData must match the shader's scalar layout");
    static_assert(offsetof(MaterialData, materialId) == 84, "MaterialData must match the shader's scalar layout");
    static_assert(offsetof(MaterialData, alphaMode) == 88, "MaterialData must match the shader's scalar layout");
    static_assert(offsetof(MaterialData, flags) == 96, "MaterialData must match the shader's scalar layout");
    static_assert(offsetof(MaterialData, textureTransforms) == 100, "MaterialData must match the shader's scalar layout");
    static_assert(sizeof(InstanceData) == 108, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, normalMatrix) == 64, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, geometryIndex) == 100, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, materialIndex) == 104, "InstanceData must match the shader's scalar layout");
    static_assert(sizeof(GeometryLod) == 12, "GeometryLod must match the shader's scalar layout");
    static_assert(offsetof(GeometryLod, error) == 8, "GeometryLod must match the shader's scalar layout");
    static_assert(sizeof(GeometryData) == 28 + 12 * MAX_MESH_LODS, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, colorOffset) == 24 + 12 * MAX_MESH_LODS, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, boundsCenter) == 4, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, boundsRadius) == 16, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, lodCount) == 20, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, lods) == 24, "GeometryData must match the shader's scalar layout");
    static_assert(sizeof(SceneData) == 2144, "SceneData must match the shader's scalar layout");
    static_assert(sizeof(SceneData) % 8 == 0, "views' SceneData sit one after another, so their pointers must stay 8-byte aligned");
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
    static_assert(offsetof(SceneData, colorsPtr) == 752, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, sortKeysPtr) == 760, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, sortDrawsPtr) == 768, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, blendRun) == 908, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, frustumPlanes) == 776, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, drawCount) == 872, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runCount) == 876, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runFirst) == 880, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, hizLevelCount) == 916, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, depthTexture) == 920, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, viewportWidth) == 924, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, viewportHeight) == 928, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodErrorPixels) == 932, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, forcedLod) == 936, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, shadowMaps) == 940, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodPixelsPerUnit) == 2092, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodDepthPlane) == 2096, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, environmentSpecular) == 2112, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, brdfLut) == 2128, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, environmentRotation) == 2136, "SceneData must match the shader's scalar layout");
    static_assert(sizeof(PushConstants) == PUSH_CONSTANTS_SIZE, "PushConstants must fill the pipelines' push-constant range");
    static_assert(sizeof(ToneMapPushConstants) == 8, "ToneMapPushConstants must match the shader's scalar layout");
    static_assert(sizeof(EnvironmentPushConstants) == PUSH_CONSTANTS_SIZE, "EnvironmentPushConstants must fill the pipelines' push-constant range");
    static_assert(offsetof(EnvironmentPushConstants, size) == 12, "EnvironmentPushConstants must match the shader's scalar layout");
    static_assert(offsetof(ToneMapPushConstants, exposure) == 4, "ToneMapPushConstants must match the shader's scalar layout");


} // namespace rasm::gfx
// clang-format on