// clang-format off
#include "core.slang"

// Culling packs each run's visible draws at the start of its range of drawInfoPtr, in list order, so the draw
// order is the same every frame. It takes two dispatches over groups of 64 draws that each lie inside one run.
// countMain tests the draws and counts each group's visible ones. writeMain adds up the counts of the run's
// earlier groups to find where its group's draws go, and the run's last group writes drawCountsPtr[run], which
// drawScene draws with drawIndexedIndirectCount.

static const uint   CULL_GROUP_SIZE     = 64;           // Must match CULL_GROUP_SIZE in gpuScene.cpp.
static const uint   CULLED              = 0xFFFFFFFF;   // cullSlotsPtr of a culled draw

struct CullGroup
{
    uint            run;
    uint            firstDraw;
    uint            endDraw;                            // the end of the run
    uint            runFirstGroup;
    uint            runEndGroup;
};

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

// A draw whose bounding sphere lies fully outside a frustum plane is culled.
bool isVisible(SceneData *scene, DrawInfo draw)
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
    float radius            = geometry->boundsRadius * sqrt(stretch2);
    float3 center           = mul(world, float4(geometry->boundsCenter, 1.0)).xyz;

    for (uint i = 0; i < 6; ++i)
    {
        float4 plane = scene->frustumPlanes[i];
        if (dot(plane.xyz, center) + plane.w < -radius)
            return false;
    }
    return true;
}

// Writes each draw's slot among its group's visible draws (or CULLED), and the group's visible count. The
// group's visible draws and triangles go into the frame's stats with one atomic each.
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
        visible         = isVisible(scene, draw);
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

        uint groupTriangles = 0;
        for (uint i = 0; i < CULL_GROUP_SIZE; ++i)
            groupTriangles += visibleTriangles[i];
        if (count > 0)
        {
            InterlockedAdd(scene->cullStatsPtr[0], count);
            InterlockedAdd(scene->cullStatsPtr[1], groupTriangles);
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
// clang-format on
