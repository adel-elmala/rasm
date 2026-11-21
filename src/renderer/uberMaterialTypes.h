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
    constexpr uint32_t      MATERIAL_NORMAL_XY          = 1u << 2;  // the normal map has x and y only (BC5, EAC RG11): z is rebuilt

    // Selects how the uber shader's fragMain shades a model. Must match shaders/lib/core.slang.
    enum class MaterialId : uint32_t { PBR, UNLIT };

    // Must match rasm::Vertex (static asserted in scene.cpp) and shaders/lib/core.slang.
    struct VertexFull       { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv[MAX_VERTEX_UV_SETS]; glm::vec4 tangent; };
    struct VertexPosOnly    { glm::vec3 pos; };

    // Each shadowed light of a view has up to MAX_SHADOW_CASCADES maps. Must match shaders/lib/core.slang.
    constexpr uint32_t      MAX_SHADOW_MAPS     = MAX_VIEW_SHADOWS * MAX_SHADOW_CASCADES;

    enum class LightType : uint32_t { DIRECTIONAL, POINT, SPOT };

    // A row of the light table, at the light's handle index.
    struct Light
    {
        glm::vec3   position;       // point and spot lights
        float       intensity;
        glm::vec3   color;
        float       range;          // point and spot lights; 0: unbounded, a global light
        glm::vec3   direction;      // directional and spot lights
        LightType   type;
        float       spotScale;      // the cone's falloff: saturate(dot(direction, -l) * spotScale + spotOffset)
        float       spotOffset;
    };

    // A light of the view that has a shadow, and its maps in SceneData::shadowMaps.
    struct ShadowedLight
    {
        uint32_t    light;          // its row in the light table
        uint32_t    firstMap;
        uint32_t    mapCount;       // its cascades, or 1 for a spot or a point light (a cube)
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
        glm::vec4       previousWorld[3];           // the rows of the affine world matrix the frame before drew, for motion
        int32_t         animationRow;               // its InstanceTransform in SceneData::animatedTransformsPtr, -1: the matrices above
    };

    struct InstanceTransform
    {
        glm::mat4       worldMatrix;
        glm::mat3       normalMatrix;
        glm::vec4       previousWorld[3];
    };

    struct AnimationNodeData
    {
        glm::vec4       rest[3];                    // the rows of the local matrix at rest
        glm::vec3       translation;
        int32_t         parent;                     // an earlier node, -1 for a root
        glm::vec4       rotation;                   // quaternion x, y, z, w
        glm::vec3       scale;
        uint32_t        padding;
    };

    struct AnimationSamplerData
    {
        uint32_t        firstTime;                  // in AnimationMeshData::keys
        uint32_t        firstValue;                 // in keys, 4 floats per value
        uint32_t        keyCount;
        uint32_t        interpolation;              // rasm::AnimationInterpolation
    };

    struct AnimationMeshData
    {
        uint64_t        nodes;                      // AnimationNodeData[nodeCount]
        uint64_t        samplers;                   // AnimationSamplerData[]
        uint64_t        channels;                   // int32_t per clip, node and path: the sampler of translation, rotation, scale; -1: rest
        uint64_t        subMeshNodes;               // uint32_t per animated sub-mesh: the node it follows
        uint64_t        keys;                       // float
        uint32_t        nodeCount;
        uint32_t        subMeshCount;
    };

    struct AnimatedEntity
    {
        glm::mat4       entityMatrix;
        uint64_t        mesh;                       // AnimationMeshData in the animation pool
        float           phase;                      // the clip's time at the clock's base
        float           speed;
        float           duration;
        int32_t         clip;                       // -1: no clip plays
        uint32_t        loop;
        uint32_t        firstTransform;             // its first row in AnimationFrame::transforms
        uint32_t        firstNode;                  // its first node in AnimationFrame::pose
        uint32_t        resetSync;                  // the sync whose frame has no motion from the frame before
    };

    struct AnimationFrame
    {
        uint64_t        entities;                   // AnimatedEntity[entityCount], this slot's table
        uint64_t        transforms;                 // InstanceTransform, this slot's
        uint64_t        previousTransforms;         // the other slot's, written by the frame before
        uint64_t        pose;                       // glm::vec4, 3 per local matrix, then 3 per world matrix of each node
        float           clock;                      // seconds since the clock's base
        uint32_t        sync;
        uint32_t        entityCount;
        uint32_t        padding;
    };

    struct AnimatePushConstants
    {
        uint64_t        frame;                      // AnimationFrame
        uint32_t        padding[2];
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
        uint32_t         shadowedLightCount;
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
        uint64_t         lightsPtr;                 // the light table
        uint64_t         sceneLightsPtr;            // the scene's light rows: the global lights, then the local ones
        uint64_t         clustersPtr;               // the view's light clusters; 0: every pixel shades every local light
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
        ShadowedLight    shadowedLights[MAX_VIEW_SHADOWS];
        uint32_t         globalLightCount;          // directional lights and lights without a range, at every pixel
        uint32_t         localLightCount;           // lights with a range, sorted into clusters
        float            nearZ;                     // the camera's near plane, where the clusters start
        uint32_t         depthSamples;              // samples per pixel of depthTexture; the Hi-Z build reads each one
        glm::mat4        previousViewProjection;    // the frame before's, without jitter, for motion
        glm::vec2        jitter;                    // NDC offset added to clip x and y (times w): TAA's sub-pixel jitter
        float            textureLodBias;            // added to the material textures' mip level: -1 while jittered, sharper
        uint32_t         padding;                   // keeps animatedTransformsPtr 8-byte aligned
        uint64_t         animatedTransformsPtr;     // this slot's InstanceTransform rows, written by the Animate pass
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

    // The tone-map pass's own push constants (shaders/passes/toneMap.slang). bloom is INVALID_TEXTURE_INDEX without one.
    struct ToneMapPushConstants
    {
        uint32_t         source;
        float            exposure;
        uint32_t         bloom;
        float            bloomStrength;
    };

    // The MSAA resolve's push constants (shaders/passes/msaaResolve.slang): source is the multisampled color's bindless
    // index, target the resolved color's storage slot.
    struct ResolvePushConstants
    {
        uint32_t         source;
        uint32_t         target;
        uint32_t         samples;
        float            exposure;
    };

    // The TAA pass's push constants (shaders/passes/taa.slang): two 16-bit bindless indices or slots per word, the
    // history's reset in bit 16 of targetAndReset, "the view has no sky" in bit 17.
    struct TaaPushConstants
    {
        uint32_t         colorAndMotion;
        uint32_t         depthAndHistory;
        uint32_t         targetAndReset;
        float            exposure;
    };

    // The bloom passes' push constants (shaders/passes/bloom.slang). source and base are bindless textures, target a
    // storage slot; an up pass writes (base + tent(source)) * scale, without source (INVALID_TEXTURE_INDEX) base * scale.
    struct BloomPushConstants
    {
        uint32_t         source;
        uint32_t         target;
        uint32_t         base;
        float            scale;
    };

    // These structs are read by shaders/passes/rasm.slang through buffer-device-address pointers,
    // compiled with GLSL scalar layout. Any drift here silently corrupts the GPU reads (a wrong
    // vertsPtr/instancesPtr turns into a device lost on the first draw).
    static_assert(sizeof(Light) == 56, "Light must match the shader's scalar layout");
    static_assert(offsetof(Light, type) == 44, "Light must match the shader's scalar layout");
    static_assert(offsetof(Light, spotOffset) == 52, "Light must match the shader's scalar layout");
    static_assert(sizeof(ShadowedLight) == 12, "ShadowedLight must match the shader's scalar layout");
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
    static_assert(sizeof(SceneData) == 1808, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, animatedTransformsPtr) == 1800, "SceneData must match the shader's scalar layout");
    static_assert(sizeof(InstanceData) == 160, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, previousWorld) == 108, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, animationRow) == 156, "InstanceData must match the shader's scalar layout");
    static_assert(sizeof(InstanceTransform) == 148, "InstanceTransform must match the shader's scalar layout");
    static_assert(offsetof(InstanceTransform, previousWorld) == 100, "InstanceTransform must match the shader's scalar layout");
    static_assert(sizeof(AnimationNodeData) == 96, "AnimationNodeData must match the shader's scalar layout");
    static_assert(offsetof(AnimationNodeData, parent) == 60, "AnimationNodeData must match the shader's scalar layout");
    static_assert(offsetof(AnimationNodeData, scale) == 80, "AnimationNodeData must match the shader's scalar layout");
    static_assert(sizeof(AnimationSamplerData) == 16, "AnimationSamplerData must match the shader's scalar layout");
    static_assert(sizeof(AnimationMeshData) == 48, "AnimationMeshData must match the shader's scalar layout");
    static_assert(offsetof(AnimationMeshData, nodeCount) == 40, "AnimationMeshData must match the shader's scalar layout");
    static_assert(sizeof(AnimatedEntity) == 104, "AnimatedEntity must match the shader's scalar layout");
    static_assert(offsetof(AnimatedEntity, mesh) == 64, "AnimatedEntity must match the shader's scalar layout");
    static_assert(offsetof(AnimatedEntity, resetSync) == 100, "AnimatedEntity must match the shader's scalar layout");
    static_assert(sizeof(AnimationFrame) == 48, "AnimationFrame must match the shader's scalar layout");
    static_assert(offsetof(AnimationFrame, clock) == 32, "AnimationFrame must match the shader's scalar layout");
    static_assert(sizeof(AnimatePushConstants) == PUSH_CONSTANTS_SIZE, "AnimatePushConstants must fill the push-constant range");
    static_assert(offsetof(SceneData, previousViewProjection) == 1720, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, textureLodBias) == 1792, "SceneData must match the shader's scalar layout");
    static_assert(sizeof(SceneData) % 8 == 0, "views' SceneData sit one after another, so their pointers must stay 8-byte aligned");
    static_assert(offsetof(SceneData, shadowedLightCount) == 140, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, vertsPtr) == 144, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, instancesPtr) == 160, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, geometryPtr) == 176, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, allDrawsPtr) == 184, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, drawCountsPtr) == 192, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, cullSlotsPtr) == 200, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, groupCountsPtr) == 208, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, cullStatsPtr) == 216, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, visibilityPtr) == 224, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, hizPtr) == 232, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, colorsPtr) == 240, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, sortKeysPtr) == 248, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, sortDrawsPtr) == 256, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lightsPtr) == 264, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, sceneLightsPtr) == 272, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, clustersPtr) == 280, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, frustumPlanes) == 288, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, drawCount) == 384, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runCount) == 388, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runFirst) == 392, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, blendRun) == 420, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, hizLevelCount) == 428, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, depthTexture) == 432, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, viewportWidth) == 436, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, viewportHeight) == 440, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodErrorPixels) == 444, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, forcedLod) == 448, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, shadowMaps) == 452, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodPixelsPerUnit) == 1604, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, lodDepthPlane) == 1608, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, environmentSpecular) == 1624, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, brdfLut) == 1640, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, shadowedLights) == 1656, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, globalLightCount) == 1704, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, localLightCount) == 1708, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, nearZ) == 1712, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, environmentRotation) == 1648, "SceneData must match the shader's scalar layout");
    static_assert(sizeof(PushConstants) == PUSH_CONSTANTS_SIZE, "PushConstants must fill the pipelines' push-constant range");
    static_assert(sizeof(ToneMapPushConstants) == PUSH_CONSTANTS_SIZE, "ToneMapPushConstants must fill the push-constant range");
    static_assert(offsetof(ToneMapPushConstants, bloomStrength) == 12, "ToneMapPushConstants must match the shader's scalar layout");
    static_assert(sizeof(ResolvePushConstants) == PUSH_CONSTANTS_SIZE, "ResolvePushConstants must fill the push-constant range");
    static_assert(offsetof(ResolvePushConstants, exposure) == 12, "ResolvePushConstants must match the shader's scalar layout");
    static_assert(sizeof(TaaPushConstants) == PUSH_CONSTANTS_SIZE, "TaaPushConstants must fill the push-constant range");
    static_assert(offsetof(TaaPushConstants, exposure) == 12, "TaaPushConstants must match the shader's scalar layout");
    static_assert(sizeof(BloomPushConstants) == PUSH_CONSTANTS_SIZE, "BloomPushConstants must fill the push-constant range");
    static_assert(offsetof(BloomPushConstants, scale) == 12, "BloomPushConstants must match the shader's scalar layout");
    static_assert(sizeof(EnvironmentPushConstants) == PUSH_CONSTANTS_SIZE, "EnvironmentPushConstants must fill the pipelines' push-constant range");
    static_assert(offsetof(EnvironmentPushConstants, size) == 12, "EnvironmentPushConstants must match the shader's scalar layout");
    static_assert(offsetof(ToneMapPushConstants, exposure) == 4, "ToneMapPushConstants must match the shader's scalar layout");


} // namespace rasm::gfx
// clang-format on