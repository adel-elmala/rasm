// clang-format off
#include "core.slang"
#include "frustumCulling.slang"
#include "occlusionCulling.slang"

// Culling packs each run's visible draws at the start of its range of drawInfoPtr, in list order, so the draw
// order is the same every frame. It takes two dispatches over groups of 64 draws that each lie inside one run.
// countMain tests the draws and counts each group's visible ones. writeMain adds up the counts of the run's
// earlier groups to find where its group's draws go, and the run's last group writes drawCountsPtr[run], which
// drawScene draws with drawIndexedIndirectCount. The tests live in lib/: frustumCulling.slang and
// occlusionCulling.slang.
//
// Occlusion culling runs both dispatches twice per drawScene call (pc.argument is the phase):
//   CULL_EARLY, before the frame: the draws that are in the frustum and were visible last time.
//   passes/hiz.slang, after those are drawn: builds the Hi-Z from the depth they left.
//   CULL_LATE: tests every draw in the frustum against the Hi-Z, stores the result for the next early
//   pass, and keeps the visible draws the early pass did not draw.
// A call with hizLevelCount == 0 only runs CULL_EARLY, as frustum culling.

static const uint   CULL_GROUP_SIZE     = 64;           // Must match CULL_GROUP_SIZE in gpuScene.cpp.
static const uint   CULLED              = 0xFFFFFFFF;   // cullSlotsPtr of a culled draw
static const uint   CULL_EARLY          = 0;            // Must match CULL_EARLY and CULL_LATE in gpuScene.cpp.
static const uint   CULL_LATE           = 1;

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
// clang-format on
