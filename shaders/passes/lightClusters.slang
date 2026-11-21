// clang-format off
#include "core.slang"
#include "frustumCulling.slang"
#include "lightClusters.slang"

// Sorts a shaded view's local lights into its clusters (lib/lightClusters.slang), in three dispatches each frame:
//   lightsMain, one group: each local light's sphere in view space, kept when it is in the frustum, as a bit per light.
//   The slices span the visible spheres' depths, from the nearest to the farthest, and no nearer than the camera's
//   near plane.
//   tilesMain, a group per screen tile: the visible spheres that touch the tile's column of the view, packed in the
//   scene's order with a prefix sum.
//   clustersMain, a thread per cluster: the cluster's box in view space, and the spheres of its tile that touch it.
// A cluster tests only its tile's lights, so the work grows with the lights near each tile, not with all of them.
// Nothing is appended with atomics, so the lists are the same on every run. A view's shadowed lights are left out:
// fragMain shades them in their own loop.

static const uint   LIGHT_GROUP_SIZE        = 256;
static const uint   TILE_GROUP_SIZE         = 64;
static const uint   CLUSTER_GROUP_SIZE      = 64;   // Must match CLUSTER_GROUP_SIZE in lightClusters.cpp.
// cullStatsPtr: words after culling's. Must match CULL_STATS_LIGHTS_* in gpuScene.h.
static const uint   STATS_LIGHTS_VISIBLE    = 13;
static const uint   STATS_LIGHTS_DROPPED    = 14;
static const float  FLOAT_MAX               = 3.402823466e38;

groupshared uint    nearestBits;
groupshared uint    farthestBits;
groupshared uint    visibleCount;
groupshared uint    tileHits[TILE_GROUP_SIZE];
groupshared uint    tileCount;

// Floats as uints that keep their order, negative ones too, so atomic min and max compare them.
uint toOrderedBits(float value)
{
    uint bits = asuint(value);
    return (bits & 0x80000000) != 0 ? ~bits : bits | 0x80000000;
}

float fromOrderedBits(uint bits)
{
    return asfloat((bits & 0x80000000) != 0 ? bits & 0x7FFFFFFF : ~bits);
}

// A visible bit per local light comes after the spheres.
uint getMaskOffset(SceneData *scene)
{
    return CLUSTER_SPHERES + 4 * scene->localLightCount;
}

// Each thread takes 32 lights at a time and writes their visible bits as one word.
[numthreads(LIGHT_GROUP_SIZE, 1, 1)]
void lightsMain(uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene        = pc.scene;
    uint32_t *clusters      = scene->clustersPtr;
    if (threadId.x == 0)
    {
        nearestBits         = toOrderedBits(FLOAT_MAX);
        farthestBits        = toOrderedBits(-FLOAT_MAX);
        visibleCount        = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint localCount         = scene->localLightCount;
    uint wordCount          = (localCount + 31) / 32;
    uint maskOffset         = getMaskOffset(scene);
    uint visible            = 0;
    for (uint word = threadId.x; word < wordCount; word += LIGHT_GROUP_SIZE)
    {
        uint mask           = 0;
        for (uint bit = 0; bit < 32; ++bit)
        {
            uint i          = word * 32 + bit;
            if (i >= localCount)
                break;
            uint row        = scene->sceneLightsPtr[scene->globalLightCount + i];
            if (isShadowedLight(scene, row))
                continue;
            float4 bounds   = getLightSphere(scene->lightsPtr[row]);
            Sphere sphere   = { bounds.xyz, bounds.w, 1.0 };
            if (!isInFrustum(scene, sphere))
                continue;
            float3 center   = mul(scene->view, float4(bounds.xyz, 1.0)).xyz;
            uint sphereWord = CLUSTER_SPHERES + 4 * i;
            clusters[sphereWord + 0] = asuint(center.x);
            clusters[sphereWord + 1] = asuint(center.y);
            clusters[sphereWord + 2] = asuint(center.z);
            clusters[sphereWord + 3] = asuint(bounds.w);
            mask           |= 1u << bit;
            ++visible;
            InterlockedMin(nearestBits, toOrderedBits(-center.z - bounds.w));
            InterlockedMax(farthestBits, toOrderedBits(-center.z + bounds.w));
        }
        clusters[maskOffset + word] = mask;
    }
    InterlockedAdd(visibleCount, visible);
    GroupMemoryBarrierWithGroupSync();

    if (threadId.x != 0)
        return;
    // No visible light: a far depth before the near one, so no pixel is in a slice.
    float sliceNear         = max(fromOrderedBits(nearestBits), scene->nearZ);
    float sliceFar          = fromOrderedBits(farthestBits);
    if (visibleCount == 0)
    {
        sliceNear           = 1.0;
        sliceFar            = 0.0;
    }
    clusters[CLUSTER_SLICE_NEAR]        = asuint(sliceNear);
    clusters[CLUSTER_SLICE_FAR]         = asuint(sliceFar);
    clusters[CLUSTER_VISIBLE_LIGHTS]    = visibleCount;
    InterlockedAdd(scene->cullStatsPtr[STATS_LIGHTS_VISIBLE], visibleCount);
}

// The view-space point at a pixel's NDC x and y and a view depth. The projection has no skew, so clip x and y take
// only x and y of the point, and clip w only its depth: a perspective camera, an orthographic one, and an off-centre
// (jittered) one alike.
float3 getViewPoint(float4x4 projection, float2 ndc, float depth)
{
    float z                 = -depth;
    float w                 = projection[3][2] * z + projection[3][3];
    float x                 = (ndc.x * w - projection[0][2] * z - projection[0][3]) / projection[0][0];
    float y                 = (ndc.y * w - projection[1][2] * z - projection[1][3]) / projection[1][1];
    return float3(x, y, z);
}

// A tile's corners in NDC: x from the left, y from the top of the screen (NDC y is up).
void getTileRect(uint tile, out float2 ndcMin, out float2 ndcMax)
{
    uint tileX              = tile % CLUSTER_TILES_X;
    uint tileY              = tile / CLUSTER_TILES_X;
    ndcMin                  = float2(-1.0 + 2.0 * float(tileX) / float(CLUSTER_TILES_X), 1.0 - 2.0 * float(tileY + 1) / float(CLUSTER_TILES_Y));
    ndcMax                  = float2(-1.0 + 2.0 * float(tileX + 1) / float(CLUSTER_TILES_X), 1.0 - 2.0 * float(tileY) / float(CLUSTER_TILES_Y));
}

float4 loadSphere(uint32_t *clusters, uint light)
{
    uint word               = CLUSTER_SPHERES + 4 * light;
    return float4(asfloat(clusters[word + 0]), asfloat(clusters[word + 1]), asfloat(clusters[word + 2]), asfloat(clusters[word + 3]));
}

// The four side planes of a tile's column, through its edges at the slices' near and far depth, facing in. A sphere
// that is fully outside one of them is not in the column.
bool touchesTile(float4 planes[4], float4 sphere)
{
    for (uint i = 0; i < 4; ++i)
    {
        if (dot(planes[i].xyz, sphere.xyz) + planes[i].w < -sphere.w)
            return false;
    }
    return true;
}

[numthreads(TILE_GROUP_SIZE, 1, 1)]
void tilesMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene        = pc.scene;
    uint32_t *clusters      = scene->clustersPtr;
    uint tile               = groupId.x;
    float sliceNear         = asfloat(clusters[CLUSTER_SLICE_NEAR]);
    float sliceFar          = asfloat(clusters[CLUSTER_SLICE_FAR]);
    if (!(sliceFar > sliceNear))
    {
        if (threadId.x == 0)
            clusters[TILE_COUNTS + tile] = 0;
        return;
    }

    // Corner c is at x = c & 1, y = c >> 1 of the rectangle; each side plane holds an edge's two corners at the near
    // depth and one of them at the far depth, and faces the tile's centre.
    float2 ndcMin, ndcMax;
    getTileRect(tile, ndcMin, ndcMax);
    float3 nearCorners[4], farCorners[4];
    for (uint c = 0; c < 4; ++c)
    {
        float2 ndc          = float2((c & 1) != 0 ? ndcMax.x : ndcMin.x, (c & 2) != 0 ? ndcMax.y : ndcMin.y);
        nearCorners[c]      = getViewPoint(scene->projection, ndc, sliceNear);
        farCorners[c]       = getViewPoint(scene->projection, ndc, sliceFar);
    }
    float3 centre           = getViewPoint(scene->projection, (ndcMin + ndcMax) * 0.5, (sliceNear + sliceFar) * 0.5);
    const uint2 edges[4]    = { uint2(0, 1), uint2(1, 3), uint2(3, 2), uint2(2, 0) };
    float4 planes[4];
    for (uint e = 0; e < 4; ++e)
    {
        float3 a            = nearCorners[edges[e].x];
        float3 normal       = normalize(cross(nearCorners[edges[e].y] - a, farCorners[edges[e].x] - a));
        if (dot(normal, centre - a) < 0.0)
            normal          = -normal;
        // A little room, so a light that grazes the edge is kept.
        planes[e]           = float4(normal, -dot(normal, a) + 1e-3 * max(sliceFar, 1.0));
    }

    if (threadId.x == 0)
        tileCount           = 0;
    GroupMemoryBarrierWithGroupSync();

    // TILE_GROUP_SIZE lights at a time; the hits are packed in order by a prefix sum of the hit flags.
    uint localCount         = scene->localLightCount;
    uint maskOffset         = getMaskOffset(scene);
    uint listOffset         = TILE_LISTS + tile * MAX_TILE_LIGHTS;
    for (uint first = 0; first < localCount; first += TILE_GROUP_SIZE)
    {
        // Every thread reads the same two words, so a chunk without a visible light is skipped by the whole group.
        uint word           = first / 32;
        if (clusters[maskOffset + word] == 0 && (first + 32 >= localCount || clusters[maskOffset + word + 1] == 0))
            continue;
        uint i              = first + threadId.x;
        bool hit            = i < localCount && (clusters[maskOffset + i / 32] & (1u << (i % 32))) != 0 && touchesTile(planes, loadSphere(clusters, i));
        tileHits[threadId.x] = hit ? 1 : 0;
        GroupMemoryBarrierWithGroupSync();
        for (uint step = 1; step < TILE_GROUP_SIZE; step *= 2)
        {
            uint before     = threadId.x >= step ? tileHits[threadId.x - step] : 0;
            GroupMemoryBarrierWithGroupSync();
            tileHits[threadId.x] += before;
            GroupMemoryBarrierWithGroupSync();
        }
        uint slot           = tileCount + tileHits[threadId.x] - (hit ? 1 : 0);
        if (hit && slot < MAX_TILE_LIGHTS)
            clusters[listOffset + slot] = i;
        GroupMemoryBarrierWithGroupSync();
        if (threadId.x == TILE_GROUP_SIZE - 1)
            tileCount      += tileHits[threadId.x];
        GroupMemoryBarrierWithGroupSync();
    }

    if (threadId.x != 0)
        return;
    clusters[TILE_COUNTS + tile] = min(tileCount, MAX_TILE_LIGHTS);
    if (tileCount > MAX_TILE_LIGHTS)
        InterlockedAdd(scene->cullStatsPtr[STATS_LIGHTS_DROPPED], tileCount - MAX_TILE_LIGHTS);
}

// The threads of a tile's slices are neighbours, so they read the same tile list.
[numthreads(CLUSTER_GROUP_SIZE, 1, 1)]
void clustersMain(uint3 dispatchId : SV_DispatchThreadID)
{
    SceneData *scene        = pc.scene;
    uint32_t *clusters      = scene->clustersPtr;
    uint tile               = dispatchId.x / CLUSTER_SLICES;
    uint slice              = dispatchId.x % CLUSTER_SLICES;
    uint cluster            = tile + CLUSTER_TILES * slice;
    float sliceNear         = asfloat(clusters[CLUSTER_SLICE_NEAR]);
    float sliceFar          = asfloat(clusters[CLUSTER_SLICE_FAR]);
    if (!(sliceFar > sliceNear))
    {
        clusters[CLUSTER_COUNTS + cluster] = 0;
        return;
    }

    // The box around the cluster's eight corners: the tile's rectangle at the slice's two depths. It grows by a
    // little, so a pixel on its edge still finds the lights that touch it.
    bool even               = hasEvenSlices(scene);
    float2 ndcMin, ndcMax;
    getTileRect(tile, ndcMin, ndcMax);
    float depths[2]         = { getClusterSliceDepth(slice, sliceNear, sliceFar, even), getClusterSliceDepth(slice + 1, sliceNear, sliceFar, even) };
    float3 boxMin           = float3(FLOAT_MAX);
    float3 boxMax           = float3(-FLOAT_MAX);
    for (uint corner = 0; corner < 8; ++corner)
    {
        float2 ndc          = float2((corner & 1) != 0 ? ndcMax.x : ndcMin.x, (corner & 2) != 0 ? ndcMax.y : ndcMin.y);
        float3 p            = getViewPoint(scene->projection, ndc, depths[corner >> 2]);
        boxMin              = min(boxMin, p);
        boxMax              = max(boxMax, p);
    }
    float margin            = 1e-3 * max(max(abs(depths[0]), abs(depths[1])), 1.0);
    boxMin                 -= margin;
    boxMax                 += margin;

    uint tileCount          = clusters[TILE_COUNTS + tile];
    uint tileList           = TILE_LISTS + tile * MAX_TILE_LIGHTS;
    uint listOffset         = CLUSTER_LISTS + cluster * MAX_CLUSTER_LIGHTS;
    uint count              = 0;
    uint dropped            = 0;
    for (uint j = 0; j < tileCount; ++j)
    {
        uint i              = clusters[tileList + j];
        float4 sphere       = loadSphere(clusters, i);
        float3 offset       = sphere.xyz - clamp(sphere.xyz, boxMin, boxMax);
        if (dot(offset, offset) > sphere.w * sphere.w)
            continue;
        if (count < MAX_CLUSTER_LIGHTS)
            clusters[listOffset + count++] = scene->sceneLightsPtr[scene->globalLightCount + i];
        else
            ++dropped;
    }
    clusters[CLUSTER_COUNTS + cluster] = count;
    if (dropped > 0)
        InterlockedAdd(scene->cullStatsPtr[STATS_LIGHTS_DROPPED], dropped);
}
// clang-format on
