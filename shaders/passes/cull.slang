// clang-format off
#include "core.slang"

// Culling packs each run's visible draws at the start of its range of drawInfoPtr, in list order, so the draw
// order is the same every frame. It takes two dispatches over groups of 64 draws that each lie inside one run.
// countMain tests the draws and counts each group's visible ones. writeMain adds up the counts of the run's
// earlier groups to find where its group's draws go, and the run's last group writes drawCountsPtr[run], which
// drawScene draws with drawIndexedIndirectCount.
//
// Occlusion culling runs both dispatches twice per drawScene call (pc.argument is the phase):
//   CULL_EARLY, before the frame: the draws that are in the frustum and were visible last time.
//   hizMain, after those are drawn: one dispatch per level builds the Hi-Z from the depth they left.
//   CULL_LATE: tests every draw in the frustum against the Hi-Z, stores the result for the next early
//   pass, and keeps the visible draws the early pass did not draw.
// A call with hizLevelCount == 0 only runs CULL_EARLY, as frustum culling.

static const uint   CULL_GROUP_SIZE     = 64;           // Must match CULL_GROUP_SIZE in gpuScene.cpp.
static const uint   HIZ_GROUP_SIZE      = 8;            // Must match HIZ_GROUP_SIZE in gpuScene.cpp.
static const uint   CULLED              = 0xFFFFFFFF;   // cullSlotsPtr of a culled draw
static const uint   CULL_EARLY          = 0;
static const uint   CULL_LATE           = 1;

struct CullGroup
{
    uint            run;
    uint            firstDraw;
    uint            endDraw;                            // the end of the run
    uint            runFirstGroup;
    uint            runEndGroup;
};

struct Sphere
{
    float3          center;
    float           radius;
};

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

groupshared uint    visibleFlags[CULL_GROUP_SIZE];
groupshared uint    visibleTriangles[CULL_GROUP_SIZE];
groupshared uint    partialSums[CULL_GROUP_SIZE];

// The groups of run 0 come first, then those of run 1, and so on.
CullGroup getCullGroup(SceneData *scene, uint groupIndex)
{
    CullGroup group = {};
    uint runFirstGroup = 0;
    for (uint run = 0; run < scene->runCount; ++run)
    {
        uint first          = scene->runFirst[run];
        uint end            = run + 1 < scene->runCount ? scene->runFirst[run + 1] : scene->drawCount;
        uint runEndGroup    = runFirstGroup + (end - first + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE;
        if (groupIndex < runEndGroup)
        {
            group.run           = run;
            group.firstDraw     = first + (groupIndex - runFirstGroup) * CULL_GROUP_SIZE;
            group.endDraw       = end;
            group.runFirstGroup = runFirstGroup;
            group.runEndGroup   = runEndGroup;
            return group;
        }
        runFirstGroup = runEndGroup;
    }
    return group;
}

// The geometry's bounding sphere in world space.
Sphere getWorldSphere(SceneData *scene, DrawInfo draw)
{
    InstanceData *instance  = scene->instancesPtr + draw.firstInstance;
    GeometryData *geometry  = scene->geometryPtr + instance->geometryIndex;

    // The radius grows by the matrix's largest stretch: the square root of the largest eigenvalue of
    // transpose(M) * M, whose entries are the dot products of M's columns. Its largest row sum of absolute
    // values bounds that eigenvalue (Gershgorin), exactly when the columns are orthogonal, and never too small
    // when a non-uniform scale is followed by a rotation.
    float4x4 world          = instance->worldMatrix;
    float3x3 world3         = (float3x3)world;
    float3 c0               = mul(world3, float3(1.0, 0.0, 0.0));
    float3 c1               = mul(world3, float3(0.0, 1.0, 0.0));
    float3 c2               = mul(world3, float3(0.0, 0.0, 1.0));
    float d01               = abs(dot(c0, c1));
    float d02               = abs(dot(c0, c2));
    float d12               = abs(dot(c1, c2));
    float stretch2          = max(max(dot(c0, c0) + d01 + d02, dot(c1, c1) + d01 + d12), dot(c2, c2) + d02 + d12);

    Sphere sphere;
    sphere.radius           = geometry->boundsRadius * sqrt(stretch2);
    sphere.center           = mul(world, float4(geometry->boundsCenter, 1.0)).xyz;
    return sphere;
}

// A sphere fully outside one of the frustum planes is culled.
bool isInFrustum(SceneData *scene, Sphere sphere)
{
    for (uint i = 0; i < 6; ++i)
    {
        float4 plane = scene->frustumPlanes[i];
        if (dot(plane.xyz, sphere.center) + plane.w < -sphere.radius)
            return false;
    }
    return true;
}

// Hi-Z level 0 is half the pass's size, rounded up, and each level halves the one before, down to 1x1. A texel
// of level L holds the farthest depth (reversed-Z: the smallest) of the 2^(L+1)-pixel square it covers.
uint2 getHizLevel0Size(SceneData *scene)
{
    return (uint2(scene->viewportWidth, scene->viewportHeight) + 1) / 2;
}

// The sphere is occluded when its nearest point is farther than the farthest depth in the pixels it covers.
// It is bounded by the view-space box around it, whose eight corners give the screen rectangle and the nearest
// depth for perspective and orthographic cameras alike.
bool isOccluded(SceneData *scene, Sphere sphere)
{
    float3 center       = mul(scene->view, float4(sphere.center, 1.0)).xyz;
    float2 ndcMin       = float2(1e30, 1e30);
    float2 ndcMax       = float2(-1e30, -1e30);
    float nearestDepth  = 0.0;
    for (uint i = 0; i < 8; ++i)
    {
        float3 corner   = center + sphere.radius * float3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0, (i & 4) != 0 ? 1.0 : -1.0);
        float4 clip     = mul(scene->projection, float4(corner, 1.0));
        if (clip.w <= 0.0)
            return false;                               // reaches behind the camera
        float3 ndc      = clip.xyz / clip.w;
        ndcMin          = min(ndcMin, ndc.xy);
        ndcMax          = max(ndcMax, ndc.xy);
        nearestDepth    = max(nearestDepth, ndc.z);
    }
    if (nearestDepth >= 1.0)
        return false;                                   // reaches the near plane

    // Clip space is Y-up and pixel rows run top-down. Pixel coordinates are clamped before they become integers.
    float2 viewport     = float2(scene->viewportWidth, scene->viewportHeight);
    float2 pixelMin     = clamp(float2(ndcMin.x * 0.5 + 0.5, 0.5 - ndcMax.y * 0.5) * viewport, 0.0, viewport - 1.0);
    float2 pixelMax     = clamp(float2(ndcMax.x * 0.5 + 0.5, 0.5 - ndcMin.y * 0.5) * viewport, 0.0, viewport - 1.0);

    // The first level where the rectangle covers at most 2x2 texels. The 1x1 top level always qualifies.
    uint2 texelMin      = uint2(pixelMin) >> 1;
    uint2 texelMax      = uint2(pixelMax) >> 1;
    uint2 size          = getHizLevel0Size(scene);
    uint offset         = 0;
    for (uint level = 0; level + 1 < scene->hizLevelCount && (texelMax.x - texelMin.x > 1 || texelMax.y - texelMin.y > 1); ++level)
    {
        offset         += size.x * size.y;
        size            = (size + 1) / 2;
        texelMin      >>= 1;
        texelMax      >>= 1;
    }

    float *texels       = scene->hizPtr + offset;
    float farthest      = min(min(texels[texelMin.y * size.x + texelMin.x], texels[texelMin.y * size.x + texelMax.x]),
                              min(texels[texelMax.y * size.x + texelMin.x], texels[texelMax.y * size.x + texelMax.x]));
    return nearestDepth < farthest;
}

// Writes each draw's slot among its group's visible draws (or CULLED), and the group's visible count. The
// group's visible draws and triangles go into the frame's stats with atomics.
[shader("compute")]
[numthreads(CULL_GROUP_SIZE, 1, 1)]
void countMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    CullGroup group     = getCullGroup(scene, groupId.x);
    uint drawIndex      = group.firstDraw + threadId.x;
    bool visible        = false;
    uint triangles      = 0;
    if (drawIndex < group.endDraw)
    {
        DrawInfo draw   = scene->allDrawsPtr[drawIndex];
        Sphere sphere   = getWorldSphere(scene, draw);
        bool inFrustum  = isInFrustum(scene, sphere);
        if (scene->hizLevelCount == 0)
        {
            visible     = inFrustum;
        }
        else if (pc.argument == CULL_EARLY)
        {
            visible     = inFrustum && scene->visibilityPtr[draw.firstInstance] != 0;
        }
        else
        {
            // The early pass's slots are still in cullSlotsPtr until this thread overwrites them below.
            bool drawnEarly = scene->cullSlotsPtr[drawIndex] != CULLED;
            bool seen       = inFrustum && !isOccluded(scene, sphere);
            scene->visibilityPtr[draw.firstInstance] = seen ? 1 : 0;
            visible         = seen && !drawnEarly;
        }
        triangles       = visible ? draw.indexCount / 3 : 0;
    }

    visibleFlags[threadId.x]        = visible ? 1 : 0;
    visibleTriangles[threadId.x]    = triangles;
    GroupMemoryBarrierWithGroupSync();

    uint slot = 0;
    for (uint i = 0; i < threadId.x; ++i)
        slot += visibleFlags[i];
    if (drawIndex < group.endDraw)
        scene->cullSlotsPtr[drawIndex] = visible ? slot : CULLED;
    if (threadId.x == CULL_GROUP_SIZE - 1)
    {
        uint count = slot + visibleFlags[threadId.x];
        scene->groupCountsPtr[groupId.x] = count;

        // The triangles are a 64-bit count in two uints, low word first, because not every backend has 64-bit
        // atomics: a carry out of the low word goes into the high one.
        uint trianglesLow   = 0;
        uint trianglesHigh  = 0;
        for (uint i = 0; i < CULL_GROUP_SIZE; ++i)
        {
            uint sum        = trianglesLow + visibleTriangles[i];
            trianglesHigh  += sum < trianglesLow ? 1 : 0;
            trianglesLow    = sum;
        }
        if (count > 0)
        {
            uint previousLow;
            InterlockedAdd(scene->cullStatsPtr[0], count);
            InterlockedAdd(scene->cullStatsPtr[1], trianglesLow, previousLow);
            trianglesHigh  += previousLow + trianglesLow < previousLow ? 1 : 0;
            if (trianglesHigh > 0)
                InterlockedAdd(scene->cullStatsPtr[2], trianglesHigh);
        }
    }
}

// Copies the visible draws to their packed place, after the visible draws of the run's earlier groups.
[shader("compute")]
[numthreads(CULL_GROUP_SIZE, 1, 1)]
void writeMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    CullGroup group     = getCullGroup(scene, groupId.x);

    uint sum = 0;
    for (uint g = group.runFirstGroup + threadId.x; g < groupId.x; g += CULL_GROUP_SIZE)
        sum += scene->groupCountsPtr[g];
    partialSums[threadId.x] = sum;
    GroupMemoryBarrierWithGroupSync();

    uint base = 0;
    for (uint i = 0; i < CULL_GROUP_SIZE; ++i)
        base += partialSums[i];

    uint drawIndex = group.firstDraw + threadId.x;
    if (drawIndex < group.endDraw)
    {
        uint slot = scene->cullSlotsPtr[drawIndex];
        if (slot != CULLED)
            scene->drawInfoPtr[scene->runFirst[group.run] + base + slot] = scene->allDrawsPtr[drawIndex];
    }
    if (threadId.x == 0 && groupId.x + 1 == group.runEndGroup)
        scene->drawCountsPtr[group.run] = base + scene->groupCountsPtr[groupId.x];
}

// Builds Hi-Z level pc.argument: each texel is the farthest of the 2x2 texels under it in the level before, or
// in the depth texture for level 0. Odd sizes repeat the last row or column, so every pixel is covered.
[shader("compute")]
[numthreads(HIZ_GROUP_SIZE, HIZ_GROUP_SIZE, 1)]
void hizMain(uint3 threadId : SV_DispatchThreadID)
{
    SceneData *scene    = pc.scene;
    uint level          = pc.argument;
    uint2 sourceSize    = uint2(scene->viewportWidth, scene->viewportHeight);
    uint2 size          = getHizLevel0Size(scene);
    uint sourceOffset   = 0;
    uint offset         = 0;
    for (uint i = 0; i < level; ++i)
    {
        sourceOffset    = offset;
        sourceSize      = size;
        offset         += size.x * size.y;
        size            = (size + 1) / 2;
    }
    if (threadId.x >= size.x || threadId.y >= size.y)
        return;

    uint2 p0            = threadId.xy * 2;
    uint2 p1            = min(p0 + 1, sourceSize - 1);
    float farthest;
    if (level == 0)
    {
        Sampler2D depth = gTextures[scene->depthTexture];
        farthest        = min(min(depth.Load(int3(p0.x, p0.y, 0)).r, depth.Load(int3(p1.x, p0.y, 0)).r),
                              min(depth.Load(int3(p0.x, p1.y, 0)).r, depth.Load(int3(p1.x, p1.y, 0)).r));
    }
    else
    {
        float *source   = scene->hizPtr + sourceOffset;
        farthest        = min(min(source[p0.y * sourceSize.x + p0.x], source[p0.y * sourceSize.x + p1.x]),
                              min(source[p1.y * sourceSize.x + p0.x], source[p1.y * sourceSize.x + p1.x]));
    }
    scene->hizPtr[offset + threadId.y * size.x + threadId.x] = farthest;
}
// clang-format on
