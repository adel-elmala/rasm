// clang-format off
#include "core.slang"
#include "pbr.slang"
#include "unlit.slang"

struct VSInput
{
    // Slang exposes the raw index here; vertMain restores the indirect draw baseVertex.
    uint vertexID : SV_VertexID;
};

struct VSOutput
{
    float4 clipPosition  : SV_POSITION;
    float3 worldPosition : POSITION;
    float3 worldNormal   : NORMAL;
    float4 worldTangent  : TANGENT;     // w = bitangent sign, 0 when the mesh has no tangents
    // glTF materials pick a UV set per texture (TEXCOORD_0..2); carry them all and let the
    // fragment shader select using the material's per-texture texCoord.
    float2 uv0            : TEXCOORD0;
    float2 uv1            : TEXCOORD1;
    float2 uv2            : TEXCOORD2;
    float3 worldCameraPosition : TEXCOORD3;
    nointerpolation uint materialIndex;
    nointerpolation float handedness;   // -1 when the model matrix mirrors the mesh
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    // Each draw names its instance row in firstInstance (D6).
    InstanceData *instance  = pc.scene->instancesPtr + pc.scene->drawInfoPtr[getDrawIndex()].firstInstance;

    float4x4 modelMat       = instance->worldMatrix;
    float4x4 viewMat        = pc.scene->view;
    float4x4 projectionMat  = pc.scene->projection;
    float3x3 normalMat      = instance->normalMatrix;

    // Slang lowers SV_VertexID to (VertexIndex - BaseVertex) to match D3D semantics, so it is the raw
    // index-buffer value and the sub-mesh's vertexOffset must be added back to address the geometry pool.
    VertexFull vert = pc.scene->vertsPtr[input.vertexID + pc.scene->geometryPtr[instance->geometryIndex].vertexOffset];

    VSOutput output = {};
    output.clipPosition         = mul(projectionMat, mul(viewMat, mul(modelMat, float4(vert.pos, 1.0))));
    output.worldPosition        = mul(modelMat, float4(vert.pos, 1.0)).xyz;
    output.worldNormal          = mul(normalMat, vert.normal);
    if (dot(output.worldNormal, output.worldNormal) < 1e-12) output.worldNormal = float3(0.0, 0.0, 1.0);
    output.worldNormal          = normalize(output.worldNormal);
    // Tangents lie in the surface, so they transform with the model matrix, not the normal matrix. A mirroring
    // transform (negative determinant, e.g. a negative glTF node scale) reverses cross(N, T), so fold its sign into w.
    float3x3 modelMat3          = (float3x3)modelMat;
    float handedness            = determinant(modelMat3) < 0.0 ? -1.0 : 1.0;
    output.worldTangent         = float4(mul(modelMat3, vert.tangent.xyz), vert.tangent.w * handedness);
    output.worldCameraPosition  = pc.scene->cameraWorldPosition;
    output.uv0                  = vert.uv[0];
    output.uv1                  = vert.uv[1];
    output.uv2                  = vert.uv[2];
    output.materialIndex        = instance->materialIndex;
    output.handedness           = handedness;

    return output;
}

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

[shader("fragment")]
float4 fragMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    // A pointer, so the unlit path loads only the fields it reads.
    MaterialData *model = pc.scene->materialsPtr + input.materialIndex;

    if (model->materialId == MaterialId.UNLIT)
    {
        MetallicRoughnessMaterial metallicRoughness = model->material.metallicRoughness;
        return shadeUnlit(gTextures, metallicRoughness, selectUV(metallicRoughness.baseColorTextureCoordinate, input.uv0, input.uv1, input.uv2));
    }

    Material material = model->material;

    PBRInfo info = {};
    info.vPosition              = input.worldPosition;
    info.vNormal                = input.worldNormal;
    info.vTangent               = input.worldTangent;
    info.cameraPosition         = input.worldCameraPosition;
    // Only double-sided draws show back faces (D8). A mirroring transform turns the authored front faces
    // clockwise, so it swaps which side the rasterizer calls front.
    info.backFace               = isFrontFace == (input.handedness < 0.0);
    // Each texture samples the UV set the glTF material asked for.
    info.baseColorUV            = selectUV(material.metallicRoughness.baseColorTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.metallicRoughnessUV    = selectUV(material.metallicRoughness.metallicRoughnessTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.normalUV               = selectUV(material.normal.normalTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.occlusionUV            = selectUV(material.occlusion.occlusionTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.emissiveUV             = selectUV(material.emissive.emissiveTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.numLights              = pc.scene->numLights;
    info.lights                 = pc.scene->lights;
    info.material               = material;

    return shadePBR(gTextures, info);
}

// clang-format on