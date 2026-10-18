// clang-format off
#include "pbr.slang"
#include "core.slang"

struct VSInput
{
    // SV_VertexID automatically includes the baseVertex offset from the indirect call
    uint vertexID : SV_VertexID;
};

struct VSOutput
{
    float4 clipPosition  : SV_POSITION;
    float3 worldPosition : POSITION;
    float3 worldNormal   : NORMAL;
    float3 Tangent       : TANGENT;
    // glTF materials pick a UV set per texture (TEXCOORD_0..2); carry them all and let the
    // fragment shader select using the material's per-texture texCoord.
    float2 uv0            : TEXCOORD0;
    float2 uv1            : TEXCOORD1;
    float2 uv2            : TEXCOORD2;
    float3 worldCameraPosition : TEXCOORD3;
    uint   drawID;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    uint drawID = getDrawIndex();

    float4x4 modelMat       = pc.scene->modeldataPtr[drawID].worldMatrix;
    float4x4 viewMat        = pc.scene->view;
    float4x4 projectionMat  = pc.scene->projection;
    float3x3 normalMat      = pc.scene->modeldataPtr[drawID].normalMatrix;

    // Slang lowers SV_VertexID to (VertexIndex - BaseVertex) to match D3D semantics, so it is the raw
    // index-buffer value and the per-draw vertexOffset must be added back to address the mega buffer.
    VertexFull vert = pc.scene->vertsPtr[input.vertexID + pc.scene->drawInfoPtr[drawID].vertexOffset];

    VSOutput output = {};
    output.clipPosition         = mul(projectionMat, mul(viewMat, mul(modelMat, float4(vert.pos, 1.0))));
    output.worldPosition        = mul(modelMat, float4(vert.pos, 1.0)).xyz;
    output.worldNormal          = mul(normalMat, vert.normal);
    if (dot(output.worldNormal, output.worldNormal) < 1e-12) output.worldNormal = float3(0.0, 0.0, 1.0);
    output.worldNormal          = normalize(output.worldNormal);
    output.worldCameraPosition  = pc.scene->cameraWorldPosition;
    output.Tangent              = output.worldNormal; // TODO: use the actual tangent from the vertex data
    output.uv0                  = vert.uv[0];
    output.uv1                  = vert.uv[1];
    output.uv2                  = vert.uv[2];
    output.drawID               = drawID;

    return output;
}

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    Material material = pc.scene->modeldataPtr[input.drawID].material;

    PBRInfo info = {};
    info.vPosition              = input.worldPosition;
    info.vNormal                = input.worldNormal;
    info.vTangent               = input.Tangent;
    info.cameraPosition         = input.worldCameraPosition;
    // Each texture samples the UV set the glTF material asked for.
    info.baseColorUV            = selectUV(material.metallicRoughness.baseColorTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.metallicRoughnessUV    = selectUV(material.metallicRoughness.metallicRoughtnessTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.normalUV               = selectUV(material.normal.normalTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.occlusionUV            = selectUV(material.occlusion.occlusionTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.emissiveUV             = selectUV(material.emissive.emissiveTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.numLights              = pc.scene->numLights;
    info.lights                 = pc.scene->lights;
    info.material               = material;

    return PBR(gTextures, info);
}

// clang-format on