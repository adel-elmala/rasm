// clang-format off
#include "core.slang"
#include "frustumCulling.slang"
#include "lodSelection.slang"
#include "meshletCulling.slang"
#include "occlusionCulling.slang"

// Culling packs each run's visible draws at the start of its range of drawInfoPtr, in list order, so the draw
// order is the same every frame. It takes two dispatches over groups of 64 draws that each lie inside one run.
// countMain tests the draws and counts each group's visible ones. writeMain adds up the counts of the run's
// earlier groups to find where its group's draws go, and the run's last group writes drawCountsPtr[run], which
// the scene's draw passes draw with drawIndexedIndirectCount. The tests live in lib/: frustumCulling.slang and
// occlusionCulling.slang. countMain also picks each visible draw's LOD (lodSelection.slang) and keeps it beside
// the draw's slot, and writeMain writes that LOD's index range.
//
// Occlusion culling runs both dispatches twice per scene view and frame (pc.argument is the phase), each in a pass:
//   CULL_EARLY, before the early draws: the draws that are in the frustum and were visible last time.
//   passes/hiz.slang, after those are drawn: builds the Hi-Z from the depth they left.
//   CULL_LATE: tests every draw in the frustum against the Hi-Z, stores the result for the next early
//   pass, and keeps the visible draws the early pass did not draw.
// A view with hizLevelCount == 0 only runs CULL_EARLY, as frustum culling.
// The blend run (SceneData::blendRun) is drawn by the Transparent pass after both draw passes, so the early phase
// keeps none of its draws and the late phase keeps all that pass, sorted afterwards by passes/sort.slang.
//
// Meshlets. In a run of SceneData::meshletRuns, a visible draw whose LOD has meshlets is not drawn whole: writeMain
// writes it as chunks of up to 64 meshlets, in list order, each run's from runChunkFirst. Two more dispatches then
// test each meshlet (frustum, normal cone, Hi-Z) and pack the visible ones per run, again in order:
//   meshletCountMain, a group per chunk: the tests, the chunk's visible meshlets as a 64-bit mask, and their count added
//   to the sum of its group of 64 chunks.
//   meshletWriteMain, a group per chunk: a DrawInfo per visible meshlet, at runMeshletFirst of the run, after those
//   of the run's earlier chunks; the run's last chunk writes the run's count.
// Each view has a visibility bit per meshlet of an instance (InstanceData::meshletVisibility), the meshlets' own
// two-pass test: the early phase draws a meshlet that passed its last test, the late one tests every meshlet of a seen
// instance and draws those the early phase did not. The bits are a hint only: whatever they hold, the late phase draws
// every seen meshlet the early one left out. A run's meshlets past runMeshletBudget are not split: the draw goes in
// one chunk flagged CHUNK_WHOLE, drawn whole among the meshlet draws.

static const uint   CULL_GROUP_SIZE     = 64;           // Must match CULL_GROUP_SIZE in gpuScene.cpp.
static const uint   CULLED              = 0xFFFFFFFF;   // cullSlotsPtr of a culled draw
static const uint   CULL_LOD_SHIFT      = 16;           // cullSlotsPtr of a visible draw: slot | lod << CULL_LOD_SHIFT
// cullSlotsPtr flags of a draw drawn by meshlets, below the LOD.
static const uint   CULL_MESHLETS       = 1u << 15;     // in chunks, not as a whole draw
static const uint   CULL_WHOLE          = 1u << 14;     // over the run's budget: one CHUNK_WHOLE chunk (writeMain)
static const uint   CULL_DRAWN_EARLY    = 1u << 13;     // late phase: the early one drew the whole draw
static const uint   CULL_EARLY_MESHLETS = 1u << 12;     // late phase: the early one drew its meshlets that passed their last test
static const uint   CULL_EARLY          = 0;            // Must match CULL_EARLY and CULL_LATE in gpuScene.cpp.
static const uint   CULL_LATE           = 1;
// A chunk: x = the draw in allDrawsPtr, y = lod | CHUNK_* flags, z = its first meshlet in the LOD, w = its meshlets.
static const uint   CHUNK_LOD_MASK      = 0xFF;
static const uint   CHUNK_WHOLE         = 1u << 8;
static const uint   CHUNK_DRAWN_EARLY   = 1u << 9;
static const uint   CHUNK_EARLY_MESHLETS = 1u << 10;
// The meshlet dispatches' groups: x, then rows of x. Must match MESHLET_DISPATCH_WIDTH in gpuScene.cpp.
static const uint   MESHLET_DISPATCH_WIDTH = 65535;
// A LOD with fewer meshlets is drawn whole: the draw's own test already culled it, and its few meshlets would cost
// more to test than they save (measured in 05_city and 18_meshlets, RTX 3060).
static const uint   MESHLET_MIN_DRAW    = 8;
static const uint   MESHLET_CHUNK_GROUP = 64;           // chunks per sum of chunkSumsPtr; each run's chunks start at a multiple. Must match gpuScene.cpp.
// The meshlet dispatches' pc.argument: the phase, and the run above it.
static const uint   MESHLET_RUN_SHIFT   = 4;
// cullStatsPtr: visible draws, visible triangles (low, high), the same triangles at LOD 0 (low, high), then the
// visible draws at each LOD, the light counts of lightClusters.slang, then the meshlets. Must match the CULL_STATS_*
// words in gpuScene.h.
static const uint   STATS_DRAWS         = 0;
static const uint   STATS_TRIANGLES     = 1;
static const uint   STATS_LOD0_TRIANGLES = 3;
static const uint   STATS_LOD_DRAWS     = 5;
static const uint   STATS_MESHLETS      = STATS_LOD_DRAWS + MAX_MESH_LODS + 2;
static const uint   STATS_MESHLETS_CULLED = STATS_MESHLETS + 1;
static const uint   STATS_MESHLETS_WHOLE = STATS_MESHLETS + 2;

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
groupshared uint    visibleLod0Triangles[CULL_GROUP_SIZE];
groupshared uint    visibleLods[CULL_GROUP_SIZE];          // MAX_MESH_LODS for a culled draw
groupshared uint    groupChunks[CULL_GROUP_SIZE];          // a draw's chunks, 0 when it is not drawn by meshlets
groupshared uint    groupMeshlets[CULL_GROUP_SIZE];        // a draw's meshlets, at its LOD
groupshared uint    partialSums[CULL_GROUP_SIZE];
groupshared uint    partialChunks[CULL_GROUP_SIZE];
groupshared uint    partialMeshlets[CULL_GROUP_SIZE];

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

// Adds a 64-bit count, low word first, to counter[0] (low) and counter[1] (high). Not every backend has 64-bit
// atomics, so a carry out of the low word goes into the high one.
void addCount64(uint32_t *counter, uint low, uint high)
{
    uint previousLow;
    InterlockedAdd(counter[0], low, previousLow);
    high += previousLow + low < previousLow ? 1 : 0;
    if (high > 0)
        InterlockedAdd(counter[1], high);
}

bool isMeshletRun(SceneData *scene, uint run)
{
    return ((scene->meshletRuns >> run) & 1) != 0;
}

// Writes each draw's slot among its group's visible draws and its LOD (or CULLED), and the group's visible
// count. The group's visible draws, triangles and LODs go into the frame's stats with atomics. A draw drawn by
// meshlets takes no slot: the group counts its chunks and meshlets instead.
[shader("compute")]
[numthreads(CULL_GROUP_SIZE, 1, 1)]
void countMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    CullGroup group     = getCullGroup(scene, groupId.x);
    uint drawIndex      = group.firstDraw + threadId.x;
    bool visible        = false;                        // drawn whole, in a slot
    bool counted        = false;                        // first drawn in this phase: its draw counts in the stats
    uint packed         = CULLED;
    uint triangles      = 0;
    uint lod0Triangles  = 0;
    uint lod            = MAX_MESH_LODS;
    uint chunks         = 0;
    uint meshlets       = 0;
    if (drawIndex < group.endDraw)
    {
        DrawInfo draw   = scene->allDrawsPtr[drawIndex];
        Sphere sphere   = getWorldSphere(scene, draw);
        bool inFrustum  = isInFrustum(scene, sphere);
        bool candidate  = false;
        uint earlyFlags = 0;                            // late phase: what the early phase drew of it
        if (scene->hizLevelCount == 0)
        {
            candidate   = inFrustum;
        }
        else if (pc.argument == CULL_EARLY)
        {
            // The blend run is drawn once, after both draw passes, so only the late phase keeps its draws.
            candidate   = inFrustum && scene->visibilityPtr[draw.firstInstance] != 0 && group.run != scene->blendRun;
        }
        else
        {
            // The early pass's slots are still in cullSlotsPtr until this thread overwrites them below.
            uint early      = scene->cullSlotsPtr[drawIndex];
            if (early != CULLED)
                earlyFlags  = (early & CULL_MESHLETS) != 0 && (early & CULL_WHOLE) == 0 ? CULL_EARLY_MESHLETS : CULL_DRAWN_EARLY;
            bool seen       = inFrustum && !isOccluded(scene, sphere);
            scene->visibilityPtr[draw.firstInstance] = seen ? 1 : 0;
            candidate       = seen;
        }
        if (candidate)
        {
            GeometryData *geometry = getDrawGeometry(scene, draw);
            lod             = selectLod(scene, geometry, sphere);
            bool byMeshlets = isMeshletRun(scene, group.run) && geometry->lods[lod].meshletCount >= MESHLET_MIN_DRAW;
            // A draw drawn by meshlets goes to the late phase also when the early one drew some of them: the late
            // phase tests each meshlet.
            counted         = earlyFlags == 0;
            if (byMeshlets)
            {
                meshlets    = geometry->lods[lod].meshletCount;
                chunks      = (meshlets + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE;
                packed      = CULL_MESHLETS | earlyFlags | lod << CULL_LOD_SHIFT;
            }
            else
            {
                visible     = counted;
                triangles   = visible ? geometry->lods[lod].indexCount / 3 : 0;
            }
            lod0Triangles   = counted ? draw.indexCount / 3 : 0;
        }
    }

    visibleFlags[threadId.x]            = visible ? 1 : 0;
    visibleTriangles[threadId.x]        = triangles;
    visibleLod0Triangles[threadId.x]    = lod0Triangles;
    visibleLods[threadId.x]             = counted ? lod : MAX_MESH_LODS;
    groupChunks[threadId.x]             = chunks;
    groupMeshlets[threadId.x]           = meshlets;
    GroupMemoryBarrierWithGroupSync();

    if (threadId.x < MAX_MESH_LODS)
    {
        uint lodDraws = 0;
        for (uint i = 0; i < CULL_GROUP_SIZE; ++i)
            lodDraws += visibleLods[i] == threadId.x ? 1 : 0;
        if (lodDraws > 0)
            InterlockedAdd(scene->cullStatsPtr[STATS_LOD_DRAWS + threadId.x], lodDraws);
    }

    uint slot = 0;
    for (uint i = 0; i < threadId.x; ++i)
        slot += visibleFlags[i];
    if (drawIndex < group.endDraw)
        scene->cullSlotsPtr[drawIndex] = visible ? (slot | lod << CULL_LOD_SHIFT) : packed;
    if (threadId.x == CULL_GROUP_SIZE - 1)
    {
        uint count = slot + visibleFlags[threadId.x];
        scene->groupCountsPtr[groupId.x] = count;

        // The triangle counts are 64-bit, in two uints (addCount64). A draw drawn by meshlets counts the triangles of
        // its visible meshlets (meshletCountMain).
        uint trianglesLow       = 0;
        uint trianglesHigh      = 0;
        uint lod0Low            = 0;
        uint lod0High           = 0;
        uint draws              = 0;
        uint groupChunkCount    = 0;
        uint groupMeshletCount  = 0;
        for (uint i = 0; i < CULL_GROUP_SIZE; ++i)
        {
            uint sum            = trianglesLow + visibleTriangles[i];
            trianglesHigh      += sum < trianglesLow ? 1 : 0;
            trianglesLow        = sum;
            uint lod0Sum        = lod0Low + visibleLod0Triangles[i];
            lod0High           += lod0Sum < lod0Low ? 1 : 0;
            lod0Low             = lod0Sum;
            draws              += visibleLods[i] < MAX_MESH_LODS ? 1 : 0;
            groupChunkCount    += groupChunks[i];
            groupMeshletCount  += groupMeshlets[i];
        }
        if (scene->meshletRuns != 0)
            scene->groupMeshletsPtr[groupId.x] = uint2(groupChunkCount, groupMeshletCount);
        if (draws > 0)
        {
            InterlockedAdd(scene->cullStatsPtr[STATS_DRAWS], draws);
            addCount64(scene->cullStatsPtr + STATS_TRIANGLES, trianglesLow, trianglesHigh);
            addCount64(scene->cullStatsPtr + STATS_LOD0_TRIANGLES, lod0Low, lod0High);
        }
    }
}

// Copies the visible draws to their packed place, after the visible draws of the run's earlier groups. A draw drawn
// by meshlets writes its chunks after the chunks of the run's earlier draws, or, past the run's meshlet budget, one
// chunk that draws it whole.
[shader("compute")]
[numthreads(CULL_GROUP_SIZE, 1, 1)]
void writeMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    CullGroup group     = getCullGroup(scene, groupId.x);
    bool meshletRun     = isMeshletRun(scene, group.run);

    uint sum = 0;
    uint chunkSum = 0;
    uint meshletSum = 0;
    for (uint g = group.runFirstGroup + threadId.x; g < groupId.x; g += CULL_GROUP_SIZE)
    {
        sum += scene->groupCountsPtr[g];
        if (meshletRun)
        {
            uint2 counts = scene->groupMeshletsPtr[g];
            chunkSum    += counts.x;
            meshletSum  += counts.y;
        }
    }
    partialSums[threadId.x]     = sum;
    partialChunks[threadId.x]   = chunkSum;
    partialMeshlets[threadId.x] = meshletSum;

    uint drawIndex      = group.firstDraw + threadId.x;
    uint packed         = drawIndex < group.endDraw ? scene->cullSlotsPtr[drawIndex] : CULLED;
    bool byMeshlets     = packed != CULLED && (packed & CULL_MESHLETS) != 0;
    uint lod            = packed >> CULL_LOD_SHIFT;
    DrawInfo draw       = {};
    uint meshlets       = 0;
    if (drawIndex < group.endDraw)
        draw            = scene->allDrawsPtr[drawIndex];
    if (byMeshlets)
        meshlets        = getDrawGeometry(scene, draw)->lods[lod].meshletCount;
    groupChunks[threadId.x]     = (meshlets + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE;
    groupMeshlets[threadId.x]   = meshlets;
    GroupMemoryBarrierWithGroupSync();

    uint base = 0;
    uint chunkBase = 0;
    uint meshletBase = 0;
    for (uint i = 0; i < CULL_GROUP_SIZE; ++i)
    {
        base           += partialSums[i];
        chunkBase      += partialChunks[i];
        meshletBase    += partialMeshlets[i];
    }
    uint groupChunkCount = 0;
    for (uint i = 0; i < CULL_GROUP_SIZE; ++i)
    {
        if (i < threadId.x)
        {
            chunkBase      += groupChunks[i];
            meshletBase    += groupMeshlets[i];
        }
        groupChunkCount += groupChunks[i];
    }

    if (byMeshlets)
    {
        // The meshlets that may all pass fit the run's budget; a draw past it is drawn whole, so no visible meshlet
        // is lost (the run's meshlet draws have room for the budget and one draw per draw of the run).
        bool fits       = meshletBase + meshlets <= scene->runMeshletBudget[group.run];
        uint flags      = (packed & CULL_DRAWN_EARLY) != 0 ? CHUNK_DRAWN_EARLY : 0;
        flags          |= (packed & CULL_EARLY_MESHLETS) != 0 ? CHUNK_EARLY_MESHLETS : 0;
        flags          |= fits ? 0 : CHUNK_WHOLE;
        uint first      = scene->runChunkFirst[group.run] + chunkBase;
        uint chunks     = groupChunks[threadId.x];
        for (uint c = 0; c < chunks; ++c)
        {
            uint count  = fits ? min(CULL_GROUP_SIZE, meshlets - c * CULL_GROUP_SIZE) : (c == 0 ? 1 : 0);
            scene->meshletChunksPtr[first + c] = uint4(drawIndex, lod | flags, c * CULL_GROUP_SIZE, count);
            // Each run's chunks start at a multiple of MESHLET_CHUNK_GROUP, so a group's first chunk is written once.
            if ((first + c) % MESHLET_CHUNK_GROUP == 0)
                scene->chunkSumsPtr[(first + c) / MESHLET_CHUNK_GROUP] = 0;
        }
        if (!fits)
        {
            scene->cullSlotsPtr[drawIndex] = packed | CULL_WHOLE;
            InterlockedAdd(scene->cullStatsPtr[STATS_MESHLETS_WHOLE], 1);
        }
    }
    else if (packed != CULLED)
    {
        uint slot       = packed & ((1u << CULL_LOD_SHIFT) - 1);
        scene->drawInfoPtr[scene->runFirst[group.run] + base + slot] = getLodDraw(getDrawGeometry(scene, draw), draw, lod);
    }
    if (threadId.x == 0 && groupId.x + 1 == group.runEndGroup)
    {
        scene->drawCountsPtr[group.run] = base + scene->groupCountsPtr[groupId.x];
        if (meshletRun)
        {
            uint chunks = chunkBase + groupChunkCount;
            uint rows   = (chunks + MESHLET_DISPATCH_WIDTH - 1) / MESHLET_DISPATCH_WIDTH;
            scene->meshletArgsPtr[group.run] = uint4(min(chunks, MESHLET_DISPATCH_WIDTH), rows, 1, chunks);
            // meshletWriteMain's last chunk writes the run's count; a run without chunks draws none.
            scene->meshletDrawCountsPtr[group.run] = 0;
        }
    }
}

groupshared uint    chunkMask[2];
groupshared uint    chunkBits[2];
groupshared uint    chunkTriangles;
groupshared uint    chunkCulled;

struct MeshletChunk
{
    uint            run;
    uint            index;                              // in meshletChunksPtr
    uint4           entry;
    bool            valid;
};

MeshletChunk getMeshletChunk(SceneData *scene, uint3 groupId)
{
    MeshletChunk chunk;
    chunk.run           = pc.argument >> MESHLET_RUN_SHIFT;
    uint index          = groupId.y * MESHLET_DISPATCH_WIDTH + groupId.x;
    chunk.valid         = index < scene->meshletArgsPtr[chunk.run].w;
    chunk.index         = scene->runChunkFirst[chunk.run] + index;
    chunk.entry         = chunk.valid ? scene->meshletChunksPtr[chunk.index] : uint4(0);
    return chunk;
}

// Tests a chunk's meshlets, a thread each, and keeps the visible ones in the chunk's mask. The late phase also writes
// the chunk's visibility bits: each chunk holds 64 meshlets from a multiple of 64, so its two words are its own.
[shader("compute")]
[numthreads(CULL_GROUP_SIZE, 1, 1)]
void meshletCountMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    MeshletChunk chunk  = getMeshletChunk(scene, groupId);
    if (!chunk.valid)
        return;

    uint phase          = pc.argument & 1;
    uint lane           = threadId.x;
    uint4 entry         = chunk.entry;
    DrawInfo draw       = scene->allDrawsPtr[entry.x];
    InstanceData *instance = scene->instancesPtr + draw.firstInstance;
    GeometryLod lod     = scene->geometryPtr[instance->geometryIndex].lods[entry.y & CHUNK_LOD_MASK];
    bool late           = scene->hizLevelCount > 0 && phase == CULL_LATE;
    bool hasBits        = scene->hizLevelCount > 0 && scene->meshletVisibilityPtr != nullptr && instance->meshletVisibility >= 0;
    if (lane < 2)
    {
        chunkMask[lane] = 0;
        chunkBits[lane] = 0;
    }
    if (lane == 0)
    {
        chunkTriangles  = 0;
        chunkCulled     = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane < entry.w)
    {
        bool visible    = false;
        uint triangles  = 0;
        if ((entry.y & CHUNK_WHOLE) != 0)
        {
            // Drawn whole by the early phase already; one it drew in part is drawn whole now.
            visible     = (entry.y & CHUNK_DRAWN_EARLY) == 0;
            triangles   = visible ? lod.indexCount / 3 : 0;
        }
        else
        {
            uint index      = entry.z + lane;
            GeometryMeshlet meshlet = scene->meshletsPtr[lod.firstMeshlet + index];
            Sphere sphere   = getInstanceSphere(scene, instance, meshlet.center, meshlet.radius);
            bool facing     = !(((scene->coneRuns >> chunk.run) & 1) != 0 && isMeshletBackFacing(scene, instance, meshlet));
            bool tested     = isInFrustum(scene, sphere) && facing;
            bool lastSeen   = !hasBits || readMeshletBit(scene, instance, index);
            // A meshlet the early phase leaves out is not culled yet: the late one decides. The last phase of the
            // frame counts the culled ones.
            bool culled     = false;
            if (scene->hizLevelCount == 0)
            {
                visible     = tested;
                culled      = !tested;
            }
            else if (!late)
            {
                visible     = tested && lastSeen;
            }
            else
            {
                bool seen   = tested && !isOccluded(scene, sphere);
                if (seen)
                    InterlockedOr(chunkBits[lane / 32], 1u << (lane % 32));
                bool drawnEarly = (entry.y & CHUNK_DRAWN_EARLY) != 0 || ((entry.y & CHUNK_EARLY_MESHLETS) != 0 && tested && lastSeen);
                visible     = seen && !drawnEarly;
                culled      = !seen;
            }
            if (culled)
                InterlockedAdd(chunkCulled, 1);
            triangles       = visible ? meshlet.indexCount / 3 : 0;
        }
        if (visible)
        {
            InterlockedOr(chunkMask[lane / 32], 1u << (lane % 32));
            InterlockedAdd(chunkTriangles, triangles);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // The words past the LOD's meshlets belong to no meshlet: the instance's words cover only its LOD's.
    if (late && hasBits && (entry.y & CHUNK_WHOLE) == 0 && lane < 2 && entry.z + lane * 32 < lod.meshletCount)
        scene->meshletVisibilityPtr[uint(instance->meshletVisibility) + entry.z / 32 + lane] = chunkBits[lane];
    if (lane == 0)
    {
        scene->chunkMasksPtr[chunk.index] = uint2(chunkMask[0], chunkMask[1]);
        uint visibleCount = countbits(chunkMask[0]) + countbits(chunkMask[1]);
        if (visibleCount > 0)
        {
            // A sum the order of the adds does not change, so meshletWriteMain's draws stay in chunk order.
            InterlockedAdd(scene->chunkSumsPtr[chunk.index / MESHLET_CHUNK_GROUP], visibleCount);
            InterlockedAdd(scene->cullStatsPtr[STATS_MESHLETS], visibleCount);
            addCount64(scene->cullStatsPtr + STATS_TRIANGLES, chunkTriangles, 0);
        }
        if (chunkCulled > 0)
            InterlockedAdd(scene->cullStatsPtr[STATS_MESHLETS_CULLED], chunkCulled);
    }
}

groupshared uint    writeSums[CULL_GROUP_SIZE];

// Writes each visible meshlet of a chunk as a draw of its index range, after the visible meshlets of the run's
// earlier chunks: the sums of the run's earlier groups of MESHLET_CHUNK_GROUP chunks (meshletCountMain added them up),
// then the chunks before it in its own group. So the draws keep the chunks' order, as the draws keep the list's. A
// CHUNK_WHOLE chunk writes its draw at its LOD. The run's last chunk writes the run's count.
[shader("compute")]
[numthreads(CULL_GROUP_SIZE, 1, 1)]
void meshletWriteMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    MeshletChunk chunk  = getMeshletChunk(scene, groupId);
    if (!chunk.valid)
        return;

    uint lane           = threadId.x;
    uint first          = scene->runChunkFirst[chunk.run];
    uint chunkGroup     = chunk.index / MESHLET_CHUNK_GROUP;
    uint sum            = 0;
    for (uint g = first / MESHLET_CHUNK_GROUP + lane; g < chunkGroup; g += CULL_GROUP_SIZE)
        sum            += scene->chunkSumsPtr[g];
    uint earlier        = chunkGroup * MESHLET_CHUNK_GROUP + lane;
    if (earlier < chunk.index)
    {
        uint2 earlierMask = scene->chunkMasksPtr[earlier];
        sum            += countbits(earlierMask.x) + countbits(earlierMask.y);
    }
    writeSums[lane]     = sum;
    GroupMemoryBarrierWithGroupSync();
    uint base           = 0;
    for (uint i = 0; i < CULL_GROUP_SIZE; ++i)
        base           += writeSums[i];

    uint2 mask          = scene->chunkMasksPtr[chunk.index];
    if (lane == 0 && chunk.index + 1 == first + scene->meshletArgsPtr[chunk.run].w)
        scene->meshletDrawCountsPtr[chunk.run] = base + countbits(mask.x) + countbits(mask.y);
    uint word           = lane < 32 ? mask.x : mask.y;
    if (lane >= chunk.entry.w || (word & (1u << (lane % 32))) == 0)
        return;

    uint below          = lane < 32 ? countbits(mask.x & ((1u << lane) - 1)) : countbits(mask.x) + countbits(mask.y & ((1u << (lane - 32)) - 1));
    uint slot           = scene->runMeshletFirst[chunk.run] + base + below;
    DrawInfo draw       = scene->allDrawsPtr[chunk.entry.x];
    GeometryData *geometry = getDrawGeometry(scene, draw);
    uint lod            = chunk.entry.y & CHUNK_LOD_MASK;
    if ((chunk.entry.y & CHUNK_WHOLE) != 0)
    {
        scene->meshletDrawsPtr[slot] = getLodDraw(geometry, draw, lod);
        return;
    }
    GeometryMeshlet meshlet = scene->meshletsPtr[geometry->lods[lod].firstMeshlet + chunk.entry.z + lane];
    draw.firstIndex     = meshlet.firstIndex;
    draw.indexCount     = meshlet.indexCount;
    scene->meshletDrawsPtr[slot] = draw;
}
// clang-format on