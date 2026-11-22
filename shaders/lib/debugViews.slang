// clang-format off
#pragma once

#include "core.slang"
#include "lightClusters.slang"
#include "pbr.slang"

// The scene's debug views (Engine::setDebugView): rasm.slang's fragMain compiled with RASM_DEBUG_VIEW returns
// getDebugColor instead of the shading, for the view in SceneData::debugView. The Tone Map pass then shows the
// target's values as they are, or the counts of OVERDRAW and LIGHT_COUNT as a heat color (toneMap.slang). Must match
// rasm::DebugView.
static const uint   DEBUG_VIEW_NORMALS      = 1;
static const uint   DEBUG_VIEW_UVS          = 2;
static const uint   DEBUG_VIEW_OVERDRAW     = 3;
static const uint   DEBUG_VIEW_LODS         = 4;
static const uint   DEBUG_VIEW_CULLING      = 5;
static const uint   DEBUG_VIEW_LIGHT_COUNT  = 6;
// The draw passes' PushConstants::argument: the late pass (CULL_LATE in cull.slang) draws what the Hi-Z found.
static const uint   DEBUG_LATE_PASS         = 1;

// Display values: the Tone Map pass shows them without a curve.
static const float3 DEBUG_LOD_COLORS[MAX_MESH_LODS] = {
    float3(0.20, 0.80, 0.25), float3(0.65, 0.85, 0.15), float3(0.95, 0.85, 0.15), float3(1.00, 0.55, 0.10),
    float3(0.95, 0.20, 0.15), float3(0.90, 0.25, 0.70), float3(0.55, 0.30, 0.95), float3(0.20, 0.45, 1.00)};
static const float3 DEBUG_EARLY_COLOR       = float3(0.20, 0.80, 0.25);
static const float3 DEBUG_LATE_COLOR        = float3(0.95, 0.85, 0.15);

// The LOD a draw draws: the one whose index range culling gave it (getLodDraw).
uint getDrawLod(SceneData *scene, InstanceData *instance, DrawInfo *draw)
{
    GeometryData *geometry = scene->geometryPtr + instance->geometryIndex;
    for (uint i = 1; i < min(geometry->lodCount, MAX_MESH_LODS); ++i)
    {
        if (geometry->lods[i].firstIndex == draw->firstIndex)
            return i;
    }
    return 0;
}

// The lights pbr.slang's shading loops over at this pixel: the shadowed ones, the other global ones, and the local
// ones of the pixel's cluster (every local one without clusters).
uint getDebugLightCount(SceneData *scene, float3 position, float2 pixel)
{
    uint count = scene->shadowedLightCount;
    for (uint i = 0; i < scene->globalLightCount; ++i)
        count += isShadowedLight(scene, scene->sceneLightsPtr[i]) ? 0 : 1;
    uint32_t *clusters = scene->clustersPtr;
    if (clusters != nullptr)
    {
        float viewDepth = -mul(scene->view, float4(position, 1.0)).z;
        int cluster = getCluster(scene, pixel, viewDepth);
        return count + (cluster < 0 ? 0 : min(clusters[CLUSTER_COUNTS + cluster], MAX_CLUSTER_LIGHTS));
    }
    for (uint i = 0; i < scene->localLightCount; ++i)
        count += isShadowedLight(scene, scene->sceneLightsPtr[scene->globalLightCount + i]) ? 0 : 1;
    return count;
}

// The normal the shading uses: the normal map's, turned to the side seen from a back face.
float3 getDebugNormal(Sampler2D gTextures[], MaterialData *model, float3 position, float3 normal, float4 tangent, float2 uv0, float2 uv1, float2 uv2, bool backFace)
{
    NormalMaterial material = model->material.normal;
    float3 n = normal;
    if (hasTexture(material.normalTextureIndex))
    {
        float2 uv = getTextureUV(model, TRANSFORM_NORMAL, material.normalTextureCoordinate, uv0, uv1, uv2);
        n = getNormalFromMap(gTextures[material.normalTextureIndex], uv, position, normal, tangent, material.scale, (model->flags & MATERIAL_NORMAL_XY) != 0);
    }
    n = normalize(n);
    return backFace ? -n : n;
}

// The view's value for one fragment. OVERDRAW's pipelines add it up, so each fragment counts 1. A color is lit from
// the camera, so the shapes stay readable.
float4 getDebugColor(Sampler2D gTextures[], SceneData *scene, MaterialData *model, float3 position, float3 normal, float4 tangent, float2 uv0, float2 uv1, float2 uv2,
                     float2 pixel, bool backFace, uint lod)
{
    uint view = scene->debugView;
    if (view == DEBUG_VIEW_OVERDRAW)
        return float4(1.0);
    if (view == DEBUG_VIEW_LIGHT_COUNT)
        return float4(float3(float(getDebugLightCount(scene, position, pixel))), 1.0);
    if (view == DEBUG_VIEW_UVS)
    {
        // An 8 x 8 checker per UV unit shows the texel density and where the UVs repeat.
        float2 cell = floor(uv0 * 8.0);
        float checker = fmod(abs(cell.x + cell.y), 2.0) < 0.5 ? 1.0 : 0.7;
        return float4(float3(frac(uv0), 0.0) * checker, 1.0);
    }
    float3 n = getDebugNormal(gTextures, model, position, normal, tangent, uv0, uv1, uv2, backFace);
    if (view == DEBUG_VIEW_NORMALS)
        return float4(n * 0.5 + 0.5, 1.0);
    float shade = 0.35 + 0.65 * saturate(dot(n, normalize(scene->cameraWorldPosition - position)));
    float3 color = view == DEBUG_VIEW_LODS ? DEBUG_LOD_COLORS[min(lod, MAX_MESH_LODS - 1)] : (pc.argument == DEBUG_LATE_PASS ? DEBUG_LATE_COLOR : DEBUG_EARLY_COLOR);
    return float4(color * shade, 1.0);
}
// clang-format on
