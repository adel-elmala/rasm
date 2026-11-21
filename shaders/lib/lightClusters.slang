// clang-format off
#pragma once

#include "core.slang"

// A shaded view's lights, sorted into clusters: boxes of the view, 16 x 9 screen tiles each cut into 24 depth slices.
// passes/lightClusters.slang builds them each frame, and fragMain reads the list of its pixel's cluster. Must match
// src/renderer/lightClusters.h.
static const uint   CLUSTER_TILES_X         = 16;
static const uint   CLUSTER_TILES_Y         = 9;
static const uint   CLUSTER_SLICES          = 24;
static const uint   CLUSTER_COUNT           = CLUSTER_TILES_X * CLUSTER_TILES_Y * CLUSTER_SLICES;
// Must match rasm::MAX_CLUSTER_LIGHTS.
static const uint   MAX_CLUSTER_LIGHTS      = 256;

// Lights are sorted in two steps: first into the 16 x 9 tiles, each a column of the view, then each cluster tests only
// its tile's lights. A tile keeps at most CLUSTER_SLICES * MAX_CLUSTER_LIGHTS lights: more could not all fit in its
// clusters anyway.
static const uint   CLUSTER_TILES           = CLUSTER_TILES_X * CLUSTER_TILES_Y;
static const uint   MAX_TILE_LIGHTS         = CLUSTER_SLICES * MAX_CLUSTER_LIGHTS;

// The words of a view's cluster buffer (SceneData::clustersPtr): the header (the slices' near and far view depth as
// float bits, the visible local lights, a spare word), a light count per cluster, MAX_CLUSTER_LIGHTS light rows per
// cluster, a light count per tile, MAX_TILE_LIGHTS local light indices per tile, then a sphere (view-space center,
// radius) per local light, and a visible bit per local light.
static const uint   CLUSTER_SLICE_NEAR      = 0;
static const uint   CLUSTER_SLICE_FAR       = 1;
static const uint   CLUSTER_VISIBLE_LIGHTS  = 2;
static const uint   CLUSTER_COUNTS          = 4;
static const uint   CLUSTER_LISTS           = CLUSTER_COUNTS + CLUSTER_COUNT;
static const uint   TILE_COUNTS             = CLUSTER_LISTS + CLUSTER_COUNT * MAX_CLUSTER_LIGHTS;
static const uint   TILE_LISTS              = TILE_COUNTS + CLUSTER_TILES;
static const uint   CLUSTER_SPHERES         = TILE_LISTS + CLUSTER_TILES * MAX_TILE_LIGHTS;

// An orthographic camera's slices are even; a perspective camera's grow by the same factor each, so a cluster is
// about as deep as it is wide everywhere.
bool hasEvenSlices(SceneData *scene)
{
    return scene->projection[3][3] == 1.0;
}

// The view depth where a slice starts; slice CLUSTER_SLICES is sliceFar. Must match getClusterSliceDepth in
// src/renderer/lightClusters.cpp.
float getClusterSliceDepth(uint slice, float sliceNear, float sliceFar, bool even)
{
    float t = float(slice) / float(CLUSTER_SLICES);
    if (even)
        return sliceNear + (sliceFar - sliceNear) * t;
    return sliceNear * pow(sliceFar / sliceNear, t);
}

// The slice that holds a view depth, or -1 outside sliceNear..sliceFar, where no local light reaches. Must match
// getClusterSlice in src/renderer/lightClusters.cpp.
int getClusterSlice(float depth, float sliceNear, float sliceFar, bool even)
{
    if (!(sliceFar > sliceNear) || !(depth >= sliceNear) || depth > sliceFar || (!even && !(sliceNear > 0.0)))
        return -1;
    float t = even ? (depth - sliceNear) / (sliceFar - sliceNear) : log(depth / sliceNear) / log(sliceFar / sliceNear);
    return min(int(t * float(CLUSTER_SLICES)), int(CLUSTER_SLICES) - 1);
}

// The cluster of a pixel (SV_Position.xy) at a view depth, or -1 where no local light reaches.
int getCluster(SceneData *scene, float2 pixel, float depth)
{
    uint32_t *clusters  = scene->clustersPtr;
    int slice           = getClusterSlice(depth, asfloat(clusters[CLUSTER_SLICE_NEAR]), asfloat(clusters[CLUSTER_SLICE_FAR]), hasEvenSlices(scene));
    if (slice < 0)
        return -1;
    uint x              = min(uint(pixel.x * float(CLUSTER_TILES_X) / float(scene->viewportWidth)), CLUSTER_TILES_X - 1);
    uint y              = min(uint(pixel.y * float(CLUSTER_TILES_Y) / float(scene->viewportHeight)), CLUSTER_TILES_Y - 1);
    return int(x + CLUSTER_TILES_X * (y + CLUSTER_TILES_Y * uint(slice)));
}

// The view shades its shadowed lights in a loop of their own, with their shadows, so the other loops skip them.
bool isShadowedLight(SceneData *scene, uint row)
{
    for (uint s = 0; s < scene->shadowedLightCount; ++s)
    {
        if (scene->shadowedLights[s].light == row)
            return true;
    }
    return false;
}

// The world-space sphere around everything a local light reaches: a point light's range, or the smallest sphere
// around a spot light's cone of that length, cut off by the range's sphere. Up to 45 degrees that sphere goes through
// the apex and the rim of the cone's end; past it, it is centred on the rim's circle.
float4 getLightSphere(Light light)
{
    if (light.type != LightType.SPOT)
        return float4(light.position, light.range);
    float3 axis         = normalize(light.direction);
    float cosAngle      = -light.spotOffset / light.spotScale;
    if (cosAngle >= 0.70710678)
    {
        float radius    = light.range / (2.0 * cosAngle);
        return float4(light.position + axis * radius, radius);
    }
    float sinAngle      = sqrt(saturate(1.0 - cosAngle * cosAngle));
    return float4(light.position + axis * (light.range * cosAngle), light.range * sinAngle);
}
// clang-format on
