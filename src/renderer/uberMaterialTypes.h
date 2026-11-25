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
        glm::vec3   positionLow;    // position is the high part: the light is at position + positionLow (splitPosition)
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
        glm::mat4   matrix;         // render space to clip: a cascade's or a spot's projection * view; a point light's face projection
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
        glm::vec4       world[3];                   // the rows of the affine world matrix; w: the translation's high part
        glm::vec3       translationLow;             // the translation is w + translationLow (splitPosition)
        glm::mat3       normalMatrix;
        uint32_t        geometryIndex;
        uint32_t        materialIndex;
        glm::vec4       previousWorld[3];           // the rows the frame before drew, for motion; w: its translation minus this one
        int32_t         animationRow;               // its InstanceTransform in SceneData::animatedTransformsPtr, -1: the matrices above
        int32_t         meshletVisibility;          // its first word in each view's meshlet visibility bits, -1: none
    };

    struct InstanceTransform
    {
        glm::vec4       world[3];
        glm::vec3       translationLow;
        glm::mat3       normalMatrix;
        glm::vec4       previousWorld[3];
        glm::vec3       boundsCenter;               // a skinned sub-mesh's sphere around its posed vertices, object space
        float           boundsRadius;               // < 0: the geometry's bounds
        int32_t         skinnedVertex;              // its first vertex in SceneData::skinnedVertsPtr, -1: the geometry pool's
        uint32_t        skinnedMotion;              // 1: SceneData::previousSkinnedVertsPtr holds the frame before's
    };

    // Must match rasm::SkinVertex.
    struct SkinVertex
    {
        uint16_t        joints[4];
        uint16_t        weights[4];
    };

    // Must match rasm::MorphDelta.
    struct MorphDelta
    {
        glm::vec3       position;
        uint32_t        normalTangent[3];           // half floats: normal x y, normal z tangent x, tangent y z
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

    // An animated sub-mesh: a rigid one follows a node, a skinned one (jointCount > 0) is moved by joints. A skinned or
    // morphed one is posed by the Skin pass.
    struct AnimationSubMeshData
    {
        uint32_t        node;                       // the node a rigid one follows
        uint32_t        jointCount;                 // 0: rigid
        uint32_t        firstJoint;                 // in AnimationMeshData::joints
        uint32_t        firstSphere;                // in AnimationMeshData::jointSpheres, jointCount of them
        uint32_t        geometryIndex;
        uint32_t        vertexCount;                // 0: not posed by the Skin pass
        uint32_t        firstSkinnedVertex;         // among the entity's skinned vertices, a multiple of 64
        uint32_t        firstWeight;                // among the entity's morph weights
        uint32_t        firstMorphRadius;           // in AnimationMeshData::morphRadii, one per morph target
    };

    struct AnimationJointData
    {
        glm::vec4       inverseBind[3];             // the rows of the inverse bind matrix
        uint32_t        node;
    };

    struct AnimationMeshData
    {
        uint64_t        nodes;                      // AnimationNodeData[nodeCount]
        uint64_t        samplers;                   // AnimationSamplerData[]
        uint64_t        channels;                   // int32_t per clip: per node the sampler of translation, rotation, scale, then per weight its sampler; -1: rest
        uint64_t        subMeshes;                  // AnimationSubMeshData[subMeshCount], per animated sub-mesh
        uint64_t        keys;                       // float
        uint64_t        joints;                     // AnimationJointData[jointCount]
        uint64_t        jointSpheres;               // glm::vec4: center and radius, object space; radius < 0: the joint moves no vertex
        uint64_t        weights;                    // float[weightCount]: the morph weights at rest
        uint64_t        morphRadii;                 // float per morph target of each sub-mesh: its largest position change
        uint32_t        nodeCount;
        uint32_t        subMeshCount;
        uint32_t        jointCount;
        uint32_t        skinnedVertexCount;         // per entity
        uint32_t        weightCount;                // per entity
        uint32_t        padding;
    };

    struct AnimatedEntity
    {
        glm::vec4       entity[3];                  // the rows of the entity's matrix; w: the translation's high part
        glm::vec3       translationLow;
        uint32_t        rowPadding;
        uint64_t        mesh;                       // AnimationMeshData in the animation pool
        float           phase;                      // the clip's time at the clock's base
        float           speed;
        float           duration;
        int32_t         clip;                       // -1: no clip plays
        uint32_t        loop;
        uint32_t        firstTransform;             // its first row in AnimationFrame::transforms
        uint32_t        firstNode;                  // its first node in AnimationFrame::pose
        uint32_t        resetSync;                  // the sync whose frame has no motion from the frame before
        uint32_t        firstJoint;                 // its first joint matrix in AnimationFrame::joints
        uint32_t        firstSkinnedVertex;         // its first vertex in AnimationFrame::skinnedVertices
        uint32_t        firstWeight;                // its first weight in AnimationFrame::weights and weightOverrides
        uint32_t        padding;
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
        uint32_t        previousSync;               // the sync of the last frame that ran the pass
        uint64_t        joints;                     // glm::vec4, 3 rows per joint matrix, this slot's
        uint64_t        vertices;                   // VertexFull: the geometry pool
        uint64_t        skins;                      // SkinVertex: the geometry pool's skin stream
        uint64_t        geometry;                   // GeometryData: the geometry table
        uint64_t        skinnedVertices;            // VertexFull, this slot's: what the Skin pass writes
        uint64_t        weights;                    // float, this slot's: the morph weights the Animate pass writes
        uint64_t        weightOverrides;            // float, this slot's: setMorphWeights's, NaN where the clip's count
        uint64_t        morphs;                     // MorphDelta: the geometry pool's morph stream
    };

    struct AnimatePushConstants
    {
        uint64_t        frame;                      // AnimationFrame
        uint32_t        zero;                       // always 0: the shaders' render-space sums keep their order (renderSpace.slang)
        uint32_t        padding;
    };

    struct GeometryLod
    {
        uint32_t        firstIndex;
        uint32_t        indexCount;
        float           error;
        uint32_t        firstMeshlet;               // in the meshlet pool
        uint32_t        meshletCount;               // 0: the LOD is culled and drawn whole
    };

    // A meshlet in the geometry pool's meshlet stream: a range of its LOD's indices and its bounds, in object space.
    struct GeometryMeshlet
    {
        glm::vec3       center;
        float           radius;
        glm::vec3       coneAxis;                   // every triangle faces away when the camera is inside the cone behind it
        float           coneCutoff;
        uint32_t        firstIndex;                 // in the index pool
        uint32_t        indexCount;
        uint32_t        firstData;                  // in the meshlet data pool: vertexCount vertices, then a word per triangle
        uint32_t        vertexCount;
    };

    struct GeometryData
    {
        int32_t         vertexOffset;
        glm::vec3       boundsCenter;
        float           boundsRadius;
        uint32_t        lodCount;
        GeometryLod     lods[MAX_MESH_LODS];
        int32_t         colorOffset;        // the sub-mesh's first vertex color in the color pool, -1 without colors
        int32_t         skinOffset;         // the sub-mesh's first SkinVertex in the skin stream, -1 when not skinned
        int32_t         morphOffset;        // the sub-mesh's first MorphDelta in the morph stream, -1 without morph targets
        uint32_t        morphTargetCount;
    };

    struct SceneData
    {
        glm::mat4        projection;
        glm::mat4        view;
        glm::vec3        originHigh;                // the view's origin in the world, its camera: render space = world - origin
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
        uint32_t         debugView;                 // rasm::DebugView: what a RASM_DEBUG_VIEW pipeline shows; also keeps animatedTransformsPtr 8-byte aligned
        uint64_t         animatedTransformsPtr;     // this slot's InstanceTransform rows, written by the Animate pass
        uint64_t         skinnedVertsPtr;           // this slot's skinned vertices, written by the Skin pass
        uint64_t         previousSkinnedVertsPtr;   // the other slot's, the frame before's
        uint64_t         meshletsPtr;               // GeometryMeshlet: the geometry pool's meshlet stream
        uint64_t         meshletDrawsPtr;           // DrawInfo: the view's visible meshlet draws, each run's at runMeshletFirst; 0: no meshlets
        uint64_t         meshletChunksPtr;          // uint4 per chunk of up to 64 meshlets of a draw: draw, lod and flags, first meshlet, count
        uint64_t         chunkMasksPtr;             // uint2 per chunk: its visible meshlets
        uint64_t         chunkSumsPtr;              // uint per 64 chunks: their visible meshlets
        uint64_t         meshletArgsPtr;            // uint4 per run: the meshlet dispatches' group counts, then the run's chunk count
        uint64_t         meshletDrawCountsPtr;      // uint per run: its visible meshlet draws
        uint64_t         groupMeshletsPtr;          // uint2 per cull group: its chunks and its meshlets
        uint64_t         meshletVisibilityPtr;      // the view's bits: 1 where a meshlet passed its last occlusion test
        glm::vec4        cullViewPoint;             // meshlet cone test: the camera's position (w = 1), or an orthographic view's direction (w = 0)
        uint32_t         runChunkFirst[MAX_DRAW_RUNS];
        uint32_t         runMeshletFirst[MAX_DRAW_RUNS];
        uint32_t         runMeshletBudget[MAX_DRAW_RUNS];
        uint32_t         meshletRuns;               // a bit per run whose big draws are drawn by meshlets
        uint32_t         coneRuns;                  // a bit per run culled by face, where the cone test holds
        uint32_t         padding;
        uint64_t         meshletDataPtr;            // the meshlets' vertices and triangles, for the mesh shaders; 0 without them
        glm::vec3        originLow;                 // the origin is originHigh + originLow (splitPosition)
        uint32_t         zero;                      // always 0: the shaders' render-space sums keep their order (renderSpace.slang)
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

    // The UI pass's push constants (shaders/passes/ui.slang): the draw's first ImDrawVert, its texture's bindless index
    // and the display's size in points, width | height << 16.
    struct UiPushConstants
    {
        uint64_t         vertices;
        uint32_t         texture;
        uint32_t         displaySize;
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

    // The Inspect pass's modes and flags (shaders/passes/inspect.slang).
    constexpr uint32_t   INSPECT_MODE_COLOR         = 0;
    constexpr uint32_t   INSPECT_MODE_MOTION        = 1;
    constexpr uint32_t   INSPECT_MODE_DEPTH         = 2;
    constexpr uint32_t   INSPECT_MODE_HIZ           = 3;
    constexpr uint32_t   INSPECT_MODE_CLUSTERS      = 4;
    constexpr uint32_t   INSPECT_FLAG_MULTISAMPLED  = 1u << 0;
    constexpr uint32_t   INSPECT_FLAG_CUBE          = 1u << 1;

    // The Inspect pass's parameters, in a host-visible buffer per frame slot: what to read, and how to show it in the
    // capture target's top-left width x height texels.
    struct InspectParams
    {
        uint64_t         scene;             // the view's SceneData for DEPTH, HIZ and CLUSTERS, 0 otherwise
        uint32_t         mode;              // INSPECT_MODE_*
        uint32_t         source;            // bindless index of the texture; a buffer is reached through scene
        uint32_t         target;            // storage slot of the capture target
        uint32_t         flags;             // INSPECT_FLAG_*
        uint32_t         channel;           // 0 all, 1 to 4 one as gray
        uint32_t         face;
        uint32_t         level;             // HIZ: the level; CLUSTERS: the slice, or rasm::INSPECT_TILES
        uint32_t         sample;            // rasm::INSPECT_ALL_SAMPLES: their average
        uint32_t         sourceWidth;       // of the level, in texels
        uint32_t         sourceHeight;
        uint32_t         width;
        uint32_t         height;
        float            exposure;
        float            depthRange;
    };

    struct InspectPushConstants
    {
        uint64_t         params;
        uint32_t         padding[2];
    };

    // These structs are read by shaders/passes/rasm.slang through buffer-device-address pointers,
    // compiled with GLSL scalar layout. Any drift here silently corrupts the GPU reads (a wrong
    // vertsPtr/instancesPtr turns into a device lost on the first draw).
    static_assert(sizeof(Light) == 68, "Light must match the shader's scalar layout");
    static_assert(offsetof(Light, type) == 44, "Light must match the shader's scalar layout");
    static_assert(offsetof(Light, spotOffset) == 52, "Light must match the shader's scalar layout");
    static_assert(offsetof(Light, positionLow) == 56, "Light must match the shader's scalar layout");
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
    static_assert(offsetof(InstanceData, translationLow) == 48, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, normalMatrix) == 60, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, geometryIndex) == 96, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, materialIndex) == 100, "InstanceData must match the shader's scalar layout");
    static_assert(sizeof(GeometryLod) == 20, "GeometryLod must match the shader's scalar layout");
    static_assert(offsetof(GeometryLod, error) == 8, "GeometryLod must match the shader's scalar layout");
    static_assert(offsetof(GeometryLod, meshletCount) == 16, "GeometryLod must match the shader's scalar layout");
    static_assert(sizeof(GeometryMeshlet) == 48, "GeometryMeshlet must match the shader's scalar layout");
    static_assert(offsetof(GeometryMeshlet, vertexCount) == 44, "GeometryMeshlet must match the shader's scalar layout");
    static_assert(offsetof(GeometryMeshlet, coneCutoff) == 28, "GeometryMeshlet must match the shader's scalar layout");
    static_assert(offsetof(GeometryMeshlet, indexCount) == 36, "GeometryMeshlet must match the shader's scalar layout");
    static_assert(sizeof(GeometryData) == 40 + 20 * MAX_MESH_LODS, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, morphOffset) == 32 + 20 * MAX_MESH_LODS, "GeometryData must match the shader's scalar layout");
    static_assert(sizeof(MorphDelta) == 24, "MorphDelta must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, skinOffset) == 28 + 20 * MAX_MESH_LODS, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, colorOffset) == 24 + 20 * MAX_MESH_LODS, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, boundsCenter) == 4, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, boundsRadius) == 16, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, lodCount) == 20, "GeometryData must match the shader's scalar layout");
    static_assert(offsetof(GeometryData, lods) == 24, "GeometryData must match the shader's scalar layout");
    static_assert(sizeof(SceneData) == 2032, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, meshletDataPtr) == 2008, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, originLow) == 2016, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, previousSkinnedVertsPtr) == 1816, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, animatedTransformsPtr) == 1800, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, meshletsPtr) == 1824, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, meshletVisibilityPtr) == 1888, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, cullViewPoint) == 1896, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runChunkFirst) == 1912, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, runMeshletBudget) == 1968, "SceneData must match the shader's scalar layout");
    static_assert(offsetof(SceneData, coneRuns) == 2000, "SceneData must match the shader's scalar layout");
    static_assert(sizeof(InstanceData) == 160, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, previousWorld) == 104, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, animationRow) == 152, "InstanceData must match the shader's scalar layout");
    static_assert(offsetof(InstanceData, meshletVisibility) == 156, "InstanceData must match the shader's scalar layout");
    static_assert(sizeof(InstanceTransform) == 168, "InstanceTransform must match the shader's scalar layout");
    static_assert(offsetof(InstanceTransform, boundsCenter) == 144, "InstanceTransform must match the shader's scalar layout");
    static_assert(offsetof(InstanceTransform, skinnedMotion) == 164, "InstanceTransform must match the shader's scalar layout");
    static_assert(sizeof(SkinVertex) == 16, "SkinVertex must match the shader's scalar layout");
    static_assert(sizeof(AnimationSubMeshData) == 36, "AnimationSubMeshData must match the shader's scalar layout");
    static_assert(sizeof(AnimationJointData) == 52, "AnimationJointData must match the shader's scalar layout");
    static_assert(offsetof(InstanceTransform, previousWorld) == 96, "InstanceTransform must match the shader's scalar layout");
    static_assert(sizeof(AnimationNodeData) == 96, "AnimationNodeData must match the shader's scalar layout");
    static_assert(offsetof(AnimationNodeData, parent) == 60, "AnimationNodeData must match the shader's scalar layout");
    static_assert(offsetof(AnimationNodeData, scale) == 80, "AnimationNodeData must match the shader's scalar layout");
    static_assert(sizeof(AnimationSamplerData) == 16, "AnimationSamplerData must match the shader's scalar layout");
    static_assert(sizeof(AnimationMeshData) == 96, "AnimationMeshData must match the shader's scalar layout");
    static_assert(offsetof(AnimationMeshData, weightCount) == 88, "AnimationMeshData must match the shader's scalar layout");
    static_assert(offsetof(AnimationMeshData, nodeCount) == 72, "AnimationMeshData must match the shader's scalar layout");
    static_assert(sizeof(AnimatedEntity) == 120, "AnimatedEntity must match the shader's scalar layout");
    static_assert(offsetof(AnimatedEntity, firstWeight) == 112, "AnimatedEntity must match the shader's scalar layout");
    static_assert(offsetof(AnimatedEntity, firstSkinnedVertex) == 108, "AnimatedEntity must match the shader's scalar layout");
    static_assert(offsetof(AnimatedEntity, mesh) == 64, "AnimatedEntity must match the shader's scalar layout");
    static_assert(offsetof(AnimatedEntity, translationLow) == 48, "AnimatedEntity must match the shader's scalar layout");
    static_assert(offsetof(InstanceTransform, translationLow) == 48, "InstanceTransform must match the shader's scalar layout");
    static_assert(offsetof(AnimatedEntity, resetSync) == 100, "AnimatedEntity must match the shader's scalar layout");
    static_assert(sizeof(AnimationFrame) == 112, "AnimationFrame must match the shader's scalar layout");
    static_assert(offsetof(AnimationFrame, morphs) == 104, "AnimationFrame must match the shader's scalar layout");
    static_assert(offsetof(AnimationFrame, joints) == 48, "AnimationFrame must match the shader's scalar layout");
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
    static_assert(sizeof(UiPushConstants) == PUSH_CONSTANTS_SIZE, "UiPushConstants must fill the push-constant range");
    static_assert(offsetof(UiPushConstants, displaySize) == 12, "UiPushConstants must match the shader's scalar layout");
    static_assert(offsetof(ToneMapPushConstants, exposure) == 4, "ToneMapPushConstants must match the shader's scalar layout");
    static_assert(sizeof(InspectPushConstants) == PUSH_CONSTANTS_SIZE, "InspectPushConstants must fill the push-constant range");
    static_assert(sizeof(InspectParams) == 64, "InspectParams must match the shader's scalar layout");
    static_assert(offsetof(InspectParams, depthRange) == 60, "InspectParams must match the shader's scalar layout");


} // namespace rasm::gfx
// clang-format on