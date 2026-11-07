// clang-format off
#pragma once

// Constants
static const uint   MAX_LIGHTS          = 8;
static const uint   INVALID_TEXTURE_INDEX = 0xFFFFFFFF;

// Selects how fragMain shades a model. Must match rasm::gfx::MaterialId.
enum class MaterialId : uint32_t { PBR, UNLIT };

// Must match rasm::Vertex / rasm::gfx::VertexFull (scalar layout). Unused UV sets are zero.
// tangent.w is the bitangent sign (±1), or 0 when the mesh has no tangents.
static const uint MAX_VERTEX_UV_SETS = 3;

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

enum class LightType : uint32_t { DIRECTIONAL, POINT };

// Must match rasm::gfx::Light exactly (scalar layout): the lights array sits between numLights and
// the buffer device addresses in SceneData, so any size drift shifts vertsPtr/drawInfoPtr/
// instancesPtr and the shader dereferences a garbage address (device lost).
struct Light
{
    float3      position;
    float       intensity;
    float3      color;
    float       range;      // Only used for point lights
    float3      direction;  // Only used for directional lights
    LightType   type;
};

struct MetallicRoughnessMaterial
{
    float3      baseColorFactor;
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

struct Material
{
    MetallicRoughnessMaterial   metallicRoughness;
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
    float4x4        worldMatrix;
    float3x3        normalMatrix;
    uint32_t        geometryIndex;
    uint32_t        materialIndex;
};

struct GeometryData
{
    uint32_t        firstIndex;
    uint32_t        indexCount;
    int32_t         vertexOffset;
    float3          boundsCenter;
    float           boundsRadius;
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
};

// Each indirect draw restarts DrawIndex at 0, so firstDraw is added back to find the draw's DrawInfo.
struct PushConstants
{
    SceneData* scene;
    uint32_t   firstDraw;
    uint32_t   padding;
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

// glTF base color: factor * texture, or the factor alone when the material has no texture.
float4 sampleBaseColor(Sampler2D gTextures[], MetallicRoughnessMaterial material, float2 uv)
{
    float4 color = float4(material.baseColorFactor, 1.0);
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
// clang-format on