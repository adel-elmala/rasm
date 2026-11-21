// clang-format off
#pragma once

// The geometry pool's structs. Each must match its rasm::gfx struct in src/renderer/uberMaterialTypes.h; core.slang
// includes this file, and so does animationTypes.slang, for the Skin pass.

// Must match rasm::Vertex / rasm::gfx::VertexFull (scalar layout). Unused UV sets are zero.
// tangent.w is the bitangent sign (±1), or 0 when the mesh has no tangents.
static const uint MAX_VERTEX_UV_SETS = 3;
// Must match rasm::MAX_MESH_LODS.
static const uint MAX_MESH_LODS = 8;
// Must match rasm::MAX_MORPH_TARGETS: a primitive's most morph targets.
static const uint MAX_MORPH_TARGETS = 256;

struct VertexFull       { float3 pos; float3 normal; float2 uv[MAX_VERTEX_UV_SETS]; float4 tangent; };

// Must match rasm::SkinVertex: 4 joints and 4 UNORM16 weights that add up to 65535, two per uint, the first in the low
// half.
struct SkinVertex
{
    uint2           joints;
    uint2           weights;
};

// Must match rasm::MorphDelta: a morph target's change to one vertex. The normal and the tangent's xyz are half floats,
// two per uint, the first in the low half: normal x y, normal z tangent x, tangent y z.
struct MorphDelta
{
    float3          position;
    uint32_t        normalTangent[3];
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
    int32_t         skinOffset;         // the sub-mesh's first SkinVertex in the skin stream, -1 when not skinned
    int32_t         morphOffset;        // the sub-mesh's first MorphDelta in the morph stream, -1 without morph targets
    uint32_t        morphTargetCount;   // target t's delta of vertex v is at morphOffset + t * vertex count + v
};
// clang-format on
