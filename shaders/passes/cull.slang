// clang-format off
#include "core.slang"
#include "frustumCulling.slang"
#include "lodSelection.slang"
#include "meshletCulling.slang"
#include "occlusionCulling.slang"
#include "weightedBlend.slang"

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
// keeps none of its draws and the late phase keeps all that pass, sorted afterwards by passes/sort.slang. So do the
// weighted and the additive run (weightedRun, additiveRun), drawn after the view's last draws, in any order; the weighted
// draws the phase keeps, and its weighted glyph chunks, also give the view depths their weights span (weightedRangePtr,
// lib/weightedBlend.slang).
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
static const uint   CULL_RASTER         = 1u << 11;     // a point cloud chunk sent to the compute path (lib/points.slang), or a glyph chunk kept (lib/glyphs.slang): no slot
// A LOD with fewer meshlets is drawn whole: the draw's own test already culled it, and its few meshlets would cost
// more to test than they save (measured in 05_city and 18_meshlets, RTX 3060).
static const uint   MESHLET_MIN_DRAW    = 8;
// cullStatsPtr: visible draws, visible triangles (low, high: STATS_TRIANGLES in lib/meshletCulling.slang), the same
// triangles at LOD 0 (low, high), then the visible draws at each LOD, the light counts of lightClusters.slang, then
// the meshlets. Must match the CULL_STATS_* words in gpuScene.h.
static const uint   STATS_DRAWS         = 0;
static const uint   STATS_LOD0_TRIANGLES = 3;
static const uint   STATS_LOD_DRAWS     = 5;
static const uint   STATS_LOD_TRIANGLES = STATS_MESHLETS + 3;    // low, high: the visible draws' triangles at their LOD, before meshlet culling

// The view depths a draw's bounding sphere spans; with perspective not nearer than the near plane.
float2 getSphereDepths(SceneData *scene, Sphere sphere)
{
    float depth         = -mul(scene->view, float4(sphere.center, 1.0)).z;
    float nearest       = depth - sphere.radius;
    float farthest      = depth + sphere.radius;
    if (!isOrthographic(scene))
    {
        nearest         = max(nearest, scene->nearZ);
        farthest        = max(farthest, nearest);
    }
    return float2(nearest, farthest);
}

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
groupshared uint    visibleLodTriangles[CULL_GROUP_SIZE];    // at the picked LOD, whole: before meshlet culling
groupshared uint    visibleLods[CULL_GROUP_SIZE];          // MAX_MESH_LODS for a culled draw
groupshared uint    groupChunks[CULL_GROUP_SIZE];          // a draw's chunks, 0 when it is not drawn by meshlets
groupshared uint    groupMeshlets[CULL_GROUP_SIZE];        // a draw's meshlets, at its LOD
groupshared uint    groupNearest;                          // the group's kept weighted draws' view depths (getDepthBits)
groupshared uint    groupFarthest;
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
    uint lodTriangles   = 0;
    uint lod            = MAX_MESH_LODS;
    uint chunks         = 0;
    uint meshlets       = 0;
    bool points         = group.run == scene->pointRun;
    uint nearestBits    = 0xFFFFFFFF;                   // a kept weighted draw's view depths (getDepthBits)
    uint farthestBits   = 0;
    if (drawIndex < group.endDraw)
    {
        DrawInfo draw   = scene->allDrawsPtr[drawIndex];
        Sphere sphere   = getWorldSphere(scene, draw);
        uint vector     = getVectorFlags(scene, draw);
        // Vector features, point cloud chunks and additive draws cast no shadow.
        bool inFrustum  = isInFrustum(scene, sphere) && !((vector != 0 || points || group.run == scene->additiveRun) && (scene->viewFlags & VIEW_DEPTH_ONLY) != 0);
        bool candidate  = false;
        uint earlyFlags = 0;                            // late phase: what the early phase drew of it
        if (scene->hizLevelCount == 0)
        {
            candidate   = inFrustum;
        }
        else if (pc.argument == CULL_EARLY)
        {
            // The blend runs are drawn once, after both draw passes, so only the late phase keeps their draws.
            bool late   = group.run == scene->blendRun || group.run == scene->weightedRun || group.run == scene->additiveRun;
            candidate   = inFrustum && scene->visibilityPtr[draw.firstInstance] != 0 && !late;
        }
        else
        {
            // The early pass's slots are still in cullSlotsPtr until this thread overwrites them below.
            uint early      = scene->cullSlotsPtr[drawIndex];
            if (early != CULLED)
                earlyFlags  = (early & CULL_MESHLETS) != 0 && (early & CULL_WHOLE) == 0 ? CULL_EARLY_MESHLETS : CULL_DRAWN_EARLY;
            bool seen       = inFrustum && ((vector & VECTOR_OVERLAY) != 0 || !isOccluded(scene, sphere));
            scene->visibilityPtr[draw.firstInstance] = seen ? 1 : 0;
            candidate       = seen;
        }
        bool glyphs     = false;
        if (candidate && points)
            glyphs      = (scene->materialsPtr + scene->instancesPtr[draw.firstInstance].materialIndex)->materialId == MaterialId.GLYPHS;
        if (glyphs)
        {
            // A glyph chunk takes no slot: its glyphs are culled one by one into the view's glyph list (passes/glyphCull.slang),
            // which the last draw pass draws. The late phase sees it drawn; a chunk the buffer has no room for is not drawn.
            InstanceData *instance = scene->instancesPtr + draw.firstInstance;
            counted         = earlyFlags == 0;
            uint lodCount   = instance->meshletVisibility >= 0 ? clamp(scene->geometryPtr[uint(instance->meshletVisibility)].lodCount, 1, MAX_MESH_LODS) : 0;
            bool blend      = ((scene->materialsPtr + instance->materialIndex)->flags & MATERIAL_WEIGHTED) != 0;
            bool kept       = counted && scene->glyphCullPtr != nullptr && lodCount > 0 && appendGlyphChunk(scene, draw.firstInstance, draw.indexCount, lodCount, blend);
            packed          = kept ? CULL_RASTER : CULLED;
            counted         = kept;
            lod             = 0;
            if (kept && blend)
            {
                float2 depths   = getSphereDepths(scene, sphere);
                nearestBits     = getDepthBits(depths.x);
                farthestBits    = getDepthBits(depths.y);
            }
        }
        else if (candidate && points)
        {
            // A chunk keeps a prefix of its points: the count goes above CULL_LOD_SHIFT, less one, in place of a LOD
            // (16 bits, up to MAX_POINT_CHUNK_POINTS).
            uint total      = draw.indexCount / POINT_INDICES;
            InstanceData *instance = scene->instancesPtr + draw.firstInstance;
            PointData style = (scene->materialsPtr + instance->materialIndex)->points;
            float drawn     = getPointDrawnShare(instance->meshletVisibility);
            uint count      = selectPointCount(scene, style, total, drawn, sphere.center, sphere.radius);
            // A chunk faded out draws nothing.
            counted         = earlyFlags == 0 && count > 0;
            // Small points go to the compute path, the others to the run's triangles. The chunk takes no slot, and the
            // late phase sees it drawn.
            uint4 rect      = uint4(0);
            float spacing   = (style.flags & POINT_ADAPTIVE) != 0 ? getWorldSpacing(loadPointSpacing(scene, uint(getDrawGeometry(scene, draw)->vertexOffset)), getWorldMatrix(scene, instance)) : 0.0;
            bool raster     = counted && scene->pointRasterPtr != nullptr && draw.firstInstance < POINT_RASTER_MAX_ROWS &&
                              isRasterChunk(scene, style, spacing, sphere.center, sphere.radius, count, rect) && appendPointBatches(scene, draw.firstInstance, count, count < total, rect);
            visible         = counted && !raster;
            packed          = raster ? CULL_RASTER : CULLED;
            lod             = max(count, 1u) - 1;
            triangles       = counted ? count : 0;
            lod0Triangles   = counted ? total : 0;
            lodTriangles    = triangles;
        }
        else if (candidate)
        {
            GeometryData *geometry = getDrawGeometry(scene, draw);
            lod             = selectLod(scene, geometry, sphere);
            bool byMeshlets = isMeshletRun(scene, group.run) && geometry->lods[lod].meshletCount >= MESHLET_MIN_DRAW && vector == 0;
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
            lodTriangles    = counted ? geometry->lods[lod].indexCount / 3 : 0;
            if (visible && group.run == scene->weightedRun)
            {
                float2 depths   = getSphereDepths(scene, sphere);
                nearestBits     = getDepthBits(depths.x);
                farthestBits    = getDepthBits(depths.y);
            }
        }
    }
    if (threadId.x == 0)
    {
        groupNearest    = 0xFFFFFFFF;
        groupFarthest   = 0;
    }

    visibleFlags[threadId.x]            = visible ? 1 : 0;
    visibleTriangles[threadId.x]        = triangles;
    visibleLod0Triangles[threadId.x]    = lod0Triangles;
    visibleLodTriangles[threadId.x]     = lodTriangles;
    visibleLods[threadId.x]             = counted ? (points ? 0 : lod) : MAX_MESH_LODS;
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
        uint lodLow             = 0;
        uint lodHigh            = 0;
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
            uint lodSum         = lodLow + visibleLodTriangles[i];
            lodHigh            += lodSum < lodLow ? 1 : 0;
            lodLow              = lodSum;
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
            addCount64(scene->cullStatsPtr + STATS_LOD_TRIANGLES, lodLow, lodHigh);
        }
    }

    // Only a view with weighted draws measures their range, so the others pay no second barrier (a uniform branch): the
    // group's range in shared memory, then one pair of atomics a group.
    if (scene->weightedRangePtr != nullptr)
    {
        if (nearestBits <= farthestBits)
        {
            InterlockedMin(groupNearest, nearestBits);
            InterlockedMax(groupFarthest, farthestBits);
        }
        GroupMemoryBarrierWithGroupSync();
        if (threadId.x == 0 && groupNearest <= groupFarthest)
        {
            InterlockedMin(scene->weightedRangePtr[0], groupNearest);
            InterlockedMax(scene->weightedRangePtr[1], groupFarthest);
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
            // Counted once per frame: by the phase that first draws it.
            if ((packed & (CULL_DRAWN_EARLY | CULL_EARLY_MESHLETS)) == 0)
                InterlockedAdd(scene->cullStatsPtr[STATS_MESHLETS_WHOLE], 1);
        }
    }
    else if (packed != CULLED && packed != CULL_RASTER)
    {
        uint slot       = packed & ((1u << CULL_LOD_SHIFT) - 1);
        DrawInfo visibleDraw = group.run == scene->pointRun ? getPointDraw(draw, lod + 1) : getLodDraw(getDrawGeometry(scene, draw), draw, lod);
        scene->drawInfoPtr[scene->runFirst[group.run] + base + slot] = visibleDraw;
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

// Tests a chunk's meshlets (lib/meshletCulling.slang) and keeps the visible ones as the chunk's mask, and their count in
// the sum of its group of 64 chunks: a sum the order of the adds does not change, so meshletWriteMain's draws stay in
// chunk order.
[shader("compute")]
[numthreads(CULL_GROUP_SIZE, 1, 1)]
void meshletCountMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    MeshletChunk chunk  = getMeshletChunk(scene, groupId);
    if (!chunk.valid)
        return;

    uint2 mask          = cullMeshletChunk(scene, chunk, threadId.x);
    if (threadId.x == 0)
    {
        scene->chunkMasksPtr[chunk.index] = mask;
        uint visibleCount = countbits(mask.x) + countbits(mask.y);
        if (visibleCount > 0)
            InterlockedAdd(scene->chunkSumsPtr[chunk.index / MESHLET_CHUNK_GROUP], visibleCount);
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
    if (lane >= chunk.entry.w || !isInMask(mask, lane))
        return;

    uint slot           = scene->runMeshletFirst[chunk.run] + base + countMaskBelow(mask, lane);
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