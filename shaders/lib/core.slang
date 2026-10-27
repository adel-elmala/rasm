// clang-format off
#pragma once

#include "pbr.slang"

enum VertexFormat : uint32_t { Full, PosOnly };

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

struct Modeldata
{
    float4x4        worldMatrix;
    float3x3        normalMatrix;
    Material        material;
    VertexFormat    vertFormat;
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
    Modeldata*          modeldataPtr;
};

struct PushConstants
{
    SceneData* scene;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

// Define a function to pull the native Vulkan DrawIndex built-in
uint getDrawIndex()
{
    return spirv_asm
    {
        OpExtension "SPV_KHR_shader_draw_parameters";
        OpCapability DrawParameters;
        result:$$uint = OpLoad builtin(DrawIndex:uint);
    };
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