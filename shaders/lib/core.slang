// clang-format off
#pragma once

// Constants
static const uint   MAX_LIGHTS          = 8;
static const uint   INVALID_TEXTURE_INDEX = 0xFFFFFFFF;
// A run per alpha mode and cull mode, and one blend run. Must match rasm::gfx::MAX_DRAW_RUNS.
static const uint   MAX_DRAW_RUNS       = 7;

// Selects how fragMain shades a model. Must match rasm::gfx::MaterialId.
enum class MaterialId : uint32_t { PBR, UNLIT };

// Must match rasm::AlphaMode.
enum class AlphaMode : uint32_t { NONE, MASK, BLEND };

// The textures with a KHR_texture_transform each, in Material::textureTransforms. Must match rasm::gfx::TextureTransformSlot.
static const uint   TEXTURE_TRANSFORMS              = 5;
static const uint   TRANSFORM_BASE_COLOR            = 0;
static const uint   TRANSFORM_METALLIC_ROUGHNESS    = 1;
static const uint   TRANSFORM_NORMAL                = 2;
static const uint   TRANSFORM_OCCLUSION             = 3;
static const uint   TRANSFORM_EMISSIVE              = 4;

// MaterialData::flags. Must match rasm::gfx::MATERIAL_*.
static const uint   MATERIAL_DOUBLE_SIDED           = 1 << 0;
static const uint   MATERIAL_TEXTURE_TRANSFORM      = 1 << 1;   // a texture has a KHR_texture_transform that is not the identity
static const uint   MATERIAL_NORMAL_XY              = 1 << 2;   // the normal map has x and y only (BC5, EAC RG11): z is rebuilt

// Must match rasm::Vertex / rasm::gfx::VertexFull (scalar layout). Unused UV sets are zero.
// tangent.w is the bitangent sign (±1), or 0 when the mesh has no tangents.
static const uint MAX_VERTEX_UV_SETS = 3;
// Must match rasm::MAX_MESH_LODS.
static const uint MAX_MESH_LODS = 8;

// Must match rasm::MAX_SHADOW_CASCADES.
static const uint MAX_SHADOW_CASCADES = 4;
// Must match rasm::gfx::MAX_SHADOW_MAPS: rasm::MAX_VIEW_SHADOWS lights of up to MAX_SHADOW_CASCADES maps.
static const uint MAX_SHADOW_MAPS = 16;

struct VertexFull       { float3 pos; float3 normal; float2 uv[MAX_VERTEX_UV_SETS]; float4 tangent; };
struct VertexPosOnly    { float3 pos; };

struct DrawInfo
{
    uint32_t        indexCount;
    uint32_t        instanceCount;
    uint32_t        firstIndex;
    int32_t         vertexOffset;
    uint32_t        firstInstance;
};

enum class LightType : uint32_t { DIRECTIONAL, POINT, SPOT };

// Must match rasm::gfx::Light exactly (scalar layout): the lights array sits between numLights and
// the buffer device addresses in SceneData, so any size drift shifts vertsPtr/drawInfoPtr/
// instancesPtr and the shader dereferences a garbage address (device lost).
struct Light
{
    float3      position;       // point and spot lights
    float       intensity;
    float3      color;
    float       range;          // point and spot lights
    float3      direction;      // directional and spot lights
    LightType   type;
    float       spotScale;      // the cone's falloff: saturate(dot(direction, -l) * spotScale + spotOffset)
    float       spotOffset;
    int32_t     shadowMap;      // the light's first map in SceneData::shadowMaps, -1 without a shadow
    uint32_t    shadowMapCount; // its cascades, or 1 for a spot or a point light (a cube)
};

// Must match rasm::gfx::ShadowMap.
struct ShadowMap
{
    float4x4    matrix;         // world to clip: a cascade's or a spot's projection * view; a point light's face projection
    uint32_t    texture;        // bindless index of the depth; a point light's is a cube (gCubeTextures)
    float       split;          // a cascade's: the view depth where it ends
};

struct MetallicRoughnessMaterial
{
    float4      baseColorFactor;
    uint32_t    baseColorTextureIndex;
    uint32_t    baseColorTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    uint32_t    metallicRoughnessTextureIndex;
    uint32_t    metallicRoughnessTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       metallicFactor;
    float       roughnessFactor;
};

struct NormalMaterial
{
    uint32_t    normalTextureIndex;
    uint32_t    normalTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       scale;
};

struct OcclusionMaterial
{
    uint32_t    occlusionTextureIndex;
    uint32_t    occlusionTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       strength;
};

struct EmissiveMaterial
{
    uint32_t    emissiveTextureIndex;
    uint32_t    emissiveTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float3      emissiveFactor;
};

// KHR_texture_transform: uv' = (dot(matrix.xy, uv), dot(matrix.zw, uv)) + offset. Identity without the extension.
struct TextureTransform
{
    float4      matrix;
    float2      offset;
};

struct Material
{
    MetallicRoughnessMaterial   metallicRoughness;
    NormalMaterial              normal;
    OcclusionMaterial           occlusion;
    EmissiveMaterial            emissive;           // emissiveFactor holds KHR_materials_emissive_strength too
};

// The texture transforms come after the rest, so a shader copies Material without them and reads them only for a
// material whose flags have MATERIAL_TEXTURE_TRANSFORM.
struct MaterialData
{
    Material            material;
    MaterialId          materialId;
    AlphaMode           alphaMode;
    float               alphaCutoff;
    uint32_t            flags;
    TextureTransform    textureTransforms[TEXTURE_TRANSFORMS];
};

struct InstanceData
{
    float4x4        worldMatrix;
    float3x3        normalMatrix;
    uint32_t        geometryIndex;
    uint32_t        materialIndex;
};

struct GeometryLod
{
    uint32_t        firstIndex;
    uint32_t        indexCount;
    float           error;              // object space; 0 for LOD 0
};

// The LODs share the sub-mesh's vertices, so vertexOffset is the same for all of them.
struct GeometryData
{
    int32_t         vertexOffset;
    float3          boundsCenter;
    float           boundsRadius;
    uint32_t        lodCount;
    GeometryLod     lods[MAX_MESH_LODS];
    int32_t         colorOffset;        // the sub-mesh's first color in SceneData::colorsPtr, -1 without vertex colors
};

struct SceneData
{
    float4x4            projection;
    float4x4            view;
    float3              cameraWorldPosition;
    uint32_t            numLights;
    Light[MAX_LIGHTS]   lights;
    VertexFull*         vertsPtr;
    DrawInfo*           drawInfoPtr;
    InstanceData*       instancesPtr;
    MaterialData*       materialsPtr;
    GeometryData*       geometryPtr;
    DrawInfo*           allDrawsPtr;        // the scene's whole draw list; cull.slang packs the visible ones into drawInfoPtr
    uint32_t*           drawCountsPtr;      // visible draws per run, written by cull.slang
    uint32_t*           cullSlotsPtr;       // cull.slang's scratch: a draw's slot among its group's visible draws, and its LOD
    uint32_t*           groupCountsPtr;     // cull.slang's scratch: visible draws per group of 64
    uint32_t*           cullStatsPtr;       // the frame's visible draws, triangles and LODs, added up by cull.slang (STATS_* there)
    uint32_t*           visibilityPtr;      // per instance row: 1 if the last occlusion test saw it, read by the early cull
    float*              hizPtr;             // the call's Hi-Z: farthest depth per texel, levels packed one after another
    uint32_t*           colorsPtr;          // the vertex color pool: RGBA8, RGB sRGB-encoded (GeometryData::colorOffset)
    uint2*              sortKeysPtr;        // the blend sort's keys: (~view depth, draw), sortCapacity of them (sort.slang)
    DrawInfo*           sortDrawsPtr;       // the blend sort's copy of the blend run's visible draws
    float4              frustumPlanes[6];   // world space, inside where dot(xyz, p) + w >= 0
    uint32_t            drawCount;
    uint32_t            runCount;
    uint32_t            runFirst[MAX_DRAW_RUNS];
    uint32_t            blendRun;           // the run the Transparent pass draws, sorted back to front; MAX_DRAW_RUNS without one
    uint32_t            sortCapacity;
    uint32_t            hizLevelCount;      // 0: no occlusion culling for this call
    uint32_t            depthTexture;       // bindless index of the pass's depth texture
    uint32_t            viewportWidth;      // the pass's size in pixels
    uint32_t            viewportHeight;
    float               lodErrorPixels;     // culling picks the coarsest LOD whose error stays under this on screen
    int32_t             forcedLod;          // >= 0: every draw uses this LOD (or its mesh's coarsest), for debugging
    ShadowMap           shadowMaps[MAX_SHADOW_MAPS]; // the maps of the shadowed lights, found through Light::shadowMap
    float               lodPixelsPerUnit;   // the LOD camera's pixels per world unit at view depth 1 (lodSelection.slang)
    float4              lodDepthPlane;      // dot(lodDepthPlane, float4(p, 1)) is p's view depth from the LOD camera; 0 if orthographic
    uint32_t            environmentSpecular;        // bindless cube of prefiltered light; INVALID_TEXTURE_INDEX: no environment
    uint32_t            environmentSpecularMips;
    uint32_t            environmentIrradiance;      // bindless cube
    uint32_t            environmentSky;             // bindless cube drawn behind the scene; INVALID_TEXTURE_INDEX: no sky
    uint32_t            brdfLut;                    // bindless 2D: the split sum's scale and bias of f0
    float               environmentIntensity;
    float2              environmentRotation;        // cos and sin of the environment's turn around +Y
};

// Each indirect draw restarts DrawIndex at 0, so firstDraw is added back to find the draw's DrawInfo.
// argument is for compute passes: the cull phase or the Hi-Z level (cull.slang).
struct PushConstants
{
    SceneData* scene;
    uint32_t   firstDraw;
    uint32_t   argument;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

// The draw's index in the scene: the native Vulkan DrawIndex built-in plus the indirect call's first draw.
uint getDrawIndex()
{
    uint drawIndex = spirv_asm
    {
        OpExtension "SPV_KHR_shader_draw_parameters";
        OpCapability DrawParameters;
        result:$$uint = OpLoad builtin(DrawIndex:uint);
    };
    return drawIndex + pc.firstDraw;
}

bool hasTexture(uint textureIndex)
{
    return textureIndex != INVALID_TEXTURE_INDEX;
}

// glTF base color: factor * texture * COLOR_0, or without the texture when the material has none.
float4 sampleBaseColor(Sampler2D gTextures[], MetallicRoughnessMaterial material, float2 uv, float4 vertexColor)
{
    float4 color = material.baseColorFactor * vertexColor;
    if (hasTexture(material.baseColorTextureIndex))
        color *= gTextures[material.baseColorTextureIndex].Sample(uv);
    return color;
}

// Pick the interpolated UV set a material texture asked for (glTF TEXCOORD_n). Sets we do not
// carry yet fall back to TEXCOORD_0, mirroring the CPU-side fallback in loadMaterial.
float2 selectUV(uint texCoordSet, float2 uv0, float2 uv1, float2 uv2)
{
    if (texCoordSet == 1) return uv1;
    if (texCoordSet == 2) return uv2;
    return uv0;
}

float2 applyTextureTransform(TextureTransform transform, float2 uv)
{
    return float2(dot(transform.matrix.xy, uv), dot(transform.matrix.zw, uv)) + transform.offset;
}

// The UV set one texture asked for, through its KHR_texture_transform. The flag is the same for the whole draw, so the
// branch is uniform, and a material without transforms does not load them. A shader that reads several textures
// branches once for all of them instead (fragMain): a branch per texture costs the opaque draws about 6% (05_city).
float2 getTextureUV(MaterialData *material, uint transform, uint texCoordSet, float2 uv0, float2 uv1, float2 uv2)
{
    float2 uv = selectUV(texCoordSet, uv0, uv1, uv2);
    if ((material->flags & MATERIAL_TEXTURE_TRANSFORM) == 0)
        return uv;
    return applyTextureTransform(material->textureTransforms[transform], uv);
}

float decodeSrgb(float encoded)
{
    return encoded <= 0.04045 ? encoded / 12.92 : pow((encoded + 0.055) / 1.055, 2.4);
}

// A vertex's COLOR_0 as linear RGBA, white for a sub-mesh without colors (the color stream, geometryPool.cpp). The
// stream is indexed like the vertices, by the sub-mesh's own index.
float4 getVertexColor(SceneData *scene, uint geometryIndex, uint vertexIndex)
{
    int colorOffset = scene->geometryPtr[geometryIndex].colorOffset;
    if (colorOffset < 0)
        return float4(1.0);
    uint packed = scene->colorsPtr[uint(colorOffset) + vertexIndex];
    float4 color = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    return float4(decodeSrgb(color.r), decodeSrgb(color.g), decodeSrgb(color.b), color.a);
}

// The base color's alpha, all the alpha modes test: factor * texture * COLOR_0.
float getBaseAlpha(Sampler2D gTextures[], MaterialData *material, float2 uv0, float2 uv1, float2 uv2, float vertexAlpha)
{
    float alpha = material->material.metallicRoughness.baseColorFactor.a * vertexAlpha;
    uint texture = material->material.metallicRoughness.baseColorTextureIndex;
    if (hasTexture(texture))
    {
        float2 uv = getTextureUV(material, TRANSFORM_BASE_COLOR, material->material.metallicRoughness.baseColorTextureCoordinate, uv0, uv1, uv2);
        alpha *= gTextures[texture].Sample(uv).a;
    }
    return alpha;
}
// clang-format on