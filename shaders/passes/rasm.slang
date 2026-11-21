// clang-format off
#include "core.slang"
#include "pbr.slang"
#include "unlit.slang"
#include "shadow.slang"

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
    float4 color         : COLOR;       // COLOR_0, linear, interpolated in linear space as glTF asks; white without one
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
    DrawInfo *draw          = pc.scene->drawInfoPtr + getDrawIndex();
    InstanceData *instance  = pc.scene->instancesPtr + draw->firstInstance;

    float4x4 modelMat       = instance->worldMatrix;
    float4x4 viewMat        = pc.scene->view;
    float4x4 projectionMat  = pc.scene->projection;
    float3x3 normalMat      = instance->normalMatrix;

    // Slang lowers SV_VertexID to (VertexIndex - BaseVertex) to match D3D semantics, so it is the raw
    // index-buffer value and the draw's vertexOffset must be added back to address the geometry pool.
    VertexFull vert = pc.scene->vertsPtr[input.vertexID + draw->vertexOffset];

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
    output.color                = getVertexColor(pc.scene, instance->geometryIndex, input.vertexID);
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

// The same bindless array, for the slots that hold a cube (a point light's shadow). A slot is read only through the
// declaration that matches its texture.
[[vk::binding(0, 0)]]
SamplerCube gCubeTextures[];

// Only double-sided draws show back faces (D8). A mirroring transform turns the authored front faces clockwise, so it
// swaps which side the rasterizer calls front.
bool isBackFace(float handedness, bool isFrontFace)
{
    return isFrontFace == (handedness < 0.0);
}

// The color the pipeline writes. A BLEND pipeline (RASM_ALPHA_BLEND) premultiplies it by alpha: the target keeps
// (1 - alpha) of what is behind. The others write alpha too, which the target ignores.
float4 toTarget(float4 color)
{
#if defined(RASM_ALPHA_BLEND)
    float alpha = saturate(color.a);
    return float4(color.rgb * alpha, alpha);
#else
    return color;
#endif
}

// One entry point for the three alpha modes, compiled once per mode (gpuScene.cpp): the opaque pipeline has no
// discard, so it keeps the early depth test. RASM_ALPHA_MASK (glTF MASK) draws nothing where alpha < alphaCutoff;
// with 1 sample per pixel that is a plain cut-out, and alpha-to-coverage waits for MSAA. RASM_ALPHA_BLEND (glTF
// BLEND) is drawn back to front by the Transparent pass; its run is not culled by face, so that it stays in one
// sorted order, and a single-sided material drops its back faces here instead. The shading stays in the entry point
// rather than in a function that takes the varyings: such a call reads every varying up front, which costs the opaque
// draws about 5% (05_city).
[shader("fragment")]
float4 fragMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    // A pointer, so the unlit path loads only the fields it reads.
    MaterialData *model = pc.scene->materialsPtr + input.materialIndex;
#if defined(RASM_ALPHA_MASK)
    if (getBaseAlpha(gTextures, model, input.uv0, input.uv1, input.uv2, input.color.a) < model->alphaCutoff)
        discard;
#endif
#if defined(RASM_ALPHA_BLEND)
    // handedness covers a mirroring model matrix only. A shaded view's view matrix never mirrors (a camera has no
    // scale); the mirrored views are a point light's cube faces, which are depth-only and draw no face test.
    if ((model->flags & MATERIAL_DOUBLE_SIDED) == 0 && isBackFace(input.handedness, isFrontFace))
        discard;
#endif

    if (model->materialId == MaterialId.UNLIT)
    {
        MetallicRoughnessMaterial metallicRoughness = model->material.metallicRoughness;
        float2 uv = getTextureUV(model, TRANSFORM_BASE_COLOR, metallicRoughness.baseColorTextureCoordinate, input.uv0, input.uv1, input.uv2);
        return toTarget(shadeUnlit(gTextures, metallicRoughness, uv, input.color));
    }

    Material material = model->material;

    PBRInfo info = {};
    info.vPosition              = input.worldPosition;
    info.vNormal                = input.worldNormal;
    info.vTangent               = input.worldTangent;
    info.cameraPosition         = input.worldCameraPosition;
    info.backFace               = isBackFace(input.handedness, isFrontFace);
    // Each texture samples the UV set the glTF material asked for, through its KHR_texture_transform: one branch for
    // all five, uniform over the draw.
    info.baseColorUV            = selectUV(material.metallicRoughness.baseColorTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.metallicRoughnessUV    = selectUV(material.metallicRoughness.metallicRoughnessTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.normalUV               = selectUV(material.normal.normalTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.occlusionUV            = selectUV(material.occlusion.occlusionTextureCoordinate, input.uv0, input.uv1, input.uv2);
    info.emissiveUV             = selectUV(material.emissive.emissiveTextureCoordinate, input.uv0, input.uv1, input.uv2);
    if ((model->flags & MATERIAL_TEXTURE_TRANSFORM) != 0)
    {
        info.baseColorUV            = applyTextureTransform(model->textureTransforms[TRANSFORM_BASE_COLOR], info.baseColorUV);
        info.metallicRoughnessUV    = applyTextureTransform(model->textureTransforms[TRANSFORM_METALLIC_ROUGHNESS], info.metallicRoughnessUV);
        info.normalUV               = applyTextureTransform(model->textureTransforms[TRANSFORM_NORMAL], info.normalUV);
        info.occlusionUV            = applyTextureTransform(model->textureTransforms[TRANSFORM_OCCLUSION], info.occlusionUV);
        info.emissiveUV             = applyTextureTransform(model->textureTransforms[TRANSFORM_EMISSIVE], info.emissiveUV);
    }
    info.vertexColor            = input.color;
    info.numLights              = pc.scene->numLights;
    info.lights                 = pc.scene->lights;
    float3 shadowNormal         = info.backFace ? -input.worldNormal : input.worldNormal;
    for (uint i = 0; i < MAX_LIGHTS; ++i)
    {
        info.shadows[i]         = 1.0;
        if (i < info.numLights)
            info.shadows[i]     = sampleShadow(gTextures, gCubeTextures, pc.scene, info.lights[i], input.worldPosition, shadowNormal);
    }
    info.material               = material;
    info.environment.specular       = pc.scene->environmentSpecular;
    info.environment.specularMips   = pc.scene->environmentSpecularMips;
    info.environment.irradiance     = pc.scene->environmentIrradiance;
    info.environment.brdfLut        = pc.scene->brdfLut;
    info.environment.intensity      = pc.scene->environmentIntensity;
    info.environment.rotation       = pc.scene->environmentRotation;

    return toTarget(shadePBR(gTextures, gCubeTextures, info));
}

// clang-format on
