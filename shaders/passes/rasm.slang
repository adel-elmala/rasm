// clang-format off
#include "core.slang"
#include "pbr.slang"
#include "unlit.slang"
#include "shadow.slang"
#include "meshletTask.slang"
#if defined(RASM_DEBUG_VIEW)
#include "debugViews.slang"
#endif

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
#if defined(RASM_MOTION_VECTORS)
    float4 currentClip   : TEXCOORD4;   // without jitter
    float4 previousClip  : TEXCOORD5;   // where the frame before drew the same point
#endif
#if defined(RASM_DEBUG_VIEW)
    nointerpolation uint lod;           // the LOD the draw draws, for DEBUG_VIEW_LODS
    nointerpolation uint meshlet;       // the draw's key, for DEBUG_VIEW_MESHLETS
#endif
};

// One vertex of a draw, for vertMain and meshMain alike: vertexID is the sub-mesh's own (the raw index-buffer value),
// lod and meshletKey only feed the debug views.
VSOutput getVertexOutput(InstanceData *instance, int vertexOffset, uint vertexID, uint lod, uint meshletKey)
{
    float4x4 modelMat       = getWorldMatrix(pc.scene, instance);
    float4x4 viewMat        = pc.scene->view;
    float4x4 projectionMat  = pc.scene->projection;
    float3x3 normalMat      = getNormalMatrix(pc.scene, instance);
    VertexFull vert         = getVertex(pc.scene, instance, vertexOffset, vertexID);

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
    output.color                = getVertexColor(pc.scene, instance->geometryIndex, vertexID);
    output.worldCameraPosition  = pc.scene->cameraWorldPosition;
    output.uv0                  = vert.uv[0];
    output.uv1                  = vert.uv[1];
    output.uv2                  = vert.uv[2];
    output.materialIndex        = instance->materialIndex;
    output.handedness           = handedness;
#if defined(RASM_MOTION_VECTORS)
    // The point where the frame before drew it: its last position, world matrix and view.
    float3 previousPosition     = getPreviousObjectPosition(pc.scene, instance, vertexID, vert.pos);
    float3 previousWorld        = getPreviousWorldPosition(pc.scene, instance, float4(previousPosition, 1.0));
    output.currentClip          = removeJitter(pc.scene, output.clipPosition);
    output.previousClip         = mul(pc.scene->previousViewProjection, float4(previousWorld, 1.0));
#endif
#if defined(RASM_DEBUG_VIEW)
    output.lod                  = lod;
    output.meshlet              = meshletKey;
#endif
    return output;
}

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    // Each draw names its instance row in firstInstance (D6). Slang lowers SV_VertexID to (VertexIndex - BaseVertex)
    // to match D3D semantics, so it is the raw index-buffer value and the draw's vertexOffset is added back to address
    // the geometry pool.
    DrawInfo *draw          = pc.scene->drawInfoPtr + getDrawIndex();
    InstanceData *instance  = pc.scene->instancesPtr + draw->firstInstance;
    uint lod                = 0;
    uint meshletKey         = 0;
#if defined(RASM_DEBUG_VIEW)
    lod                     = getDrawLod(pc.scene, instance, draw->firstIndex);
    meshletKey              = getMeshletKey(draw->firstIndex, draw->firstInstance);
#endif
    return getVertexOutput(instance, draw->vertexOffset, input.vertexID, lod, meshletKey);
}

// A visible meshlet (lib/meshletTask.slang): its vertices through getVertexOutput, as vertMain's, and its triangles.
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MESHLET_CHUNK_SIZE, 1, 1)]
void meshMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, in payload MeshletPayload meshletPayload,
              out indices uint3 triangles[MESHLET_MAX_TRIANGLES], out vertices VSOutput vertices[MESHLET_MAX_VERTICES])
{
    MeshletDraw meshlet = getMeshletDraw(pc.scene, meshletPayload, groupId.x);
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);
    uint lod            = 0;
    uint meshletKey     = 0;
#if defined(RASM_DEBUG_VIEW)
    lod                 = meshlet.lod;
    meshletKey          = getMeshletKey(meshlet.firstIndex, meshlet.draw.firstInstance);
#endif
    if (threadId.x < meshlet.vertexCount)
        vertices[threadId.x] = getVertexOutput(meshlet.instance, meshlet.draw.vertexOffset, getMeshletVertex(pc.scene, meshlet, threadId.x), lod, meshletKey);
    for (uint t = threadId.x; t < meshlet.triangleCount; t += MESHLET_CHUNK_SIZE)
        triangles[t] = getMeshletTriangle(pc.scene, meshlet, t);
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
// (1 - alpha) of what is behind. An alpha-to-coverage pipeline (RASM_ALPHA_TO_COVERAGE) writes the coverage as alpha,
// and the rasterizer keeps that share of the pixel's samples. The others write alpha too, which the target ignores.
float4 toTarget(float4 color, float coverage)
{
#if defined(RASM_ALPHA_BLEND)
    float alpha = saturate(color.a);
    return float4(color.rgb * alpha, alpha);
#elif defined(RASM_ALPHA_TO_COVERAGE)
    return float4(color.rgb, coverage);
#else
    return color;
#endif
}

#if defined(RASM_MOTION_VECTORS)
// A target with motion (RASM_MOTION_VECTORS, a MOTION_FORMAT second color) gets the pixel's screen motion as well. A
// BLEND pipeline writes zero motion with alpha 0, which its premultiplied blend turns into "keep what is behind": a
// see-through surface leaves the motion of the surface behind it.
struct FragmentOutput
{
    float4 color  : SV_TARGET0;
    float4 motion : SV_TARGET1;
};

// It takes the two clip positions, not the varyings (see fragMain), and without motion TO_OUTPUT is the color alone.
FragmentOutput toOutput(float4 color, float4 currentClip, float4 previousClip)
{
    FragmentOutput output;
    output.color = color;
#if defined(RASM_ALPHA_BLEND)
    output.motion = float4(0.0);
#else
    output.motion = float4(getMotion(currentClip, previousClip), 0.0, 0.0);
#endif
#if defined(RASM_DEBUG_VIEW)
    // OVERDRAW's pipelines add up what they write, so its fragments leave the motion as it is.
    if (pc.scene->debugView == DEBUG_VIEW_OVERDRAW)
        output.motion = float4(0.0);
#endif
    return output;
}
#define TO_OUTPUT(color) toOutput(color, input.currentClip, input.previousClip)
#else
#define TO_OUTPUT(color) (color)
#endif

// A cut-out's alpha made sharp around the cutoff: 0.5 exactly at alphaCutoff, and from 0 to 1 over one pixel's change
// of alpha (fwidth). Alpha-to-coverage then covers samples by where the edge crosses the pixel, at any distance, while
// the edge stays where the cutoff puts it. Where alpha does not change, there is no edge to place, and the cutoff
// decides as discard does: alpha at the cutoff (a cutoff of 1 on opaque texels) is covered.
float getCoverage(float alpha, float cutoff)
{
    float change = fwidth(alpha);
    if (change < 1e-4)
        return alpha >= cutoff ? 1.0 : 0.0;
    return saturate((alpha - cutoff) / change + 0.5);
}

// One entry point for the three alpha modes, compiled once per mode (gpuScene.cpp): the opaque pipeline has no
// discard, so it keeps the early depth test. RASM_ALPHA_MASK (glTF MASK) draws nothing where alpha < alphaCutoff:
// with 1 sample per pixel that is a plain cut-out; with several (RASM_ALPHA_TO_COVERAGE too) it covers the share of
// the samples its sharpened alpha says, instead of discarding, so the edge is smooth. RASM_ALPHA_BLEND (glTF
// BLEND) is drawn back to front by the Transparent pass; its run is not culled by face, so that it stays in one
// sorted order, and a single-sided material drops its back faces here instead. The shading stays in the entry point
// rather than in a function that takes the varyings: such a call reads every varying up front, which costs the opaque
// draws about 5% (05_city).
[shader("fragment")]
#if defined(RASM_MOTION_VECTORS)
FragmentOutput fragMain(VSOutput input, bool isFrontFace : SV_IsFrontFace)
#else
float4 fragMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
#endif
{
    // A pointer, so the unlit path loads only the fields it reads.
    MaterialData *model = pc.scene->materialsPtr + input.materialIndex;
    float coverage = 1.0;
#if defined(RASM_ALPHA_MASK)
    float alpha = getBaseAlpha(gTextures, model, input.uv0, input.uv1, input.uv2, input.color.a);
#if defined(RASM_ALPHA_TO_COVERAGE)
    coverage = getCoverage(alpha, model->alphaCutoff);
#else
    if (alpha < model->alphaCutoff)
        discard;
#endif
#endif
#if defined(RASM_ALPHA_BLEND)
    // handedness covers a mirroring model matrix only. A shaded view's view matrix never mirrors (a camera has no
    // scale); the mirrored views are a point light's cube faces, which are depth-only and draw no face test.
    if ((model->flags & MATERIAL_DOUBLE_SIDED) == 0 && isBackFace(input.handedness, isFrontFace))
        discard;
#endif
#if defined(RASM_DEBUG_VIEW)
    // A debug view (Engine::setDebugView) shows a value of the fragment instead of its shading; cut-outs still cut.
    return TO_OUTPUT(getDebugColor(gTextures, pc.scene, model, input.worldPosition, input.worldNormal, input.worldTangent, input.uv0, input.uv1, input.uv2,
                                   input.clipPosition.xy, isBackFace(input.handedness, isFrontFace), input.lod, input.meshlet));
#else
    if (model->materialId == MaterialId.UNLIT)
    {
        MetallicRoughnessMaterial metallicRoughness = model->material.metallicRoughness;
        float2 uv = getTextureUV(model, TRANSFORM_BASE_COLOR, metallicRoughness.baseColorTextureCoordinate, input.uv0, input.uv1, input.uv2);
        return TO_OUTPUT(toTarget(shadeUnlit(gTextures, metallicRoughness, uv, input.color), coverage));
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
    info.pixel                  = input.clipPosition.xy;
    info.material               = material;
    info.normalXY               = (model->flags & MATERIAL_NORMAL_XY) != 0;
    info.environment.specular       = pc.scene->environmentSpecular;
    info.environment.specularMips   = pc.scene->environmentSpecularMips;
    info.environment.irradiance     = pc.scene->environmentIrradiance;
    info.environment.brdfLut        = pc.scene->brdfLut;
    info.environment.intensity      = pc.scene->environmentIntensity;
    info.environment.rotation       = pc.scene->environmentRotation;

    return TO_OUTPUT(toTarget(shadePBR(gTextures, gCubeTextures, info), coverage));
#endif
}

// clang-format on
