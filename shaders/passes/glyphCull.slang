// clang-format off
#include "core.slang"
#include "frustumCulling.slang"
#include "lodSelection.slang"
#include "meshletCulling.slang"
#include "occlusionCulling.slang"
#include "glyphs.slang"

// The glyph cull (src/renderer/glyphCull.cpp), after the view's last cull pass, which kept the glyph chunks in view
// (appendGlyphChunk) and their batches of GLYPH_BATCH_GLYPHS. Two dispatches, a group a batch:
//   countMain tests each glyph's sphere against the frustum and, with a Hi-Z, against the depth the early draws left,
//   picks the LOD of its shape by its size on screen, and takes its place among its chunk's visible glyphs of that LOD
//   (groupshared counts, then one atomic per LOD and group).
//   writeMain writes each visible glyph to the list, chunk slot << 16 | glyph, after its chunk's glyphs of the LODs
//   before its own, and the chunk's first group writes a draw command per LOD: the shape's index range, instanced by
//   the LOD's count from its place in the list. The last draw pass draws them all with one drawIndexedIndirectCount.
// The order inside a LOD follows the groups' atomics, so it changes between frames; opaque glyphs tie only where two
// meet at the same depth.

// cullStatsPtr's glyph words, after cull.slang's (lib/meshletCulling.slang has the first). Must match CULL_STATS_GLYPHS
// in gpuScene.h. A visible glyph's triangles count with the draws' visible triangles (STATS_TRIANGLES).
static const uint   STATS_GLYPHS            = STATS_MESHLETS + 5;   // glyphs in the chunks the cull kept
static const uint   STATS_GLYPHS_VISIBLE    = STATS_GLYPHS + 1;

groupshared uint    lodCounts[MAX_MESH_LODS];
groupshared uint    lodPlaces[MAX_MESH_LODS];

struct GlyphBatch
{
    uint            slotIndex;
    GlyphChunkSlot  slot;
    uint            glyph;              // the thread's glyph in its chunk
    InstanceData   *instance;
    GlyphChunk      chunk;
    GeometryData   *shape;
};

GlyphBatch getGlyphBatch(SceneData *scene, uint group, uint thread)
{
    uint batch          = scene->glyphBatchesPtr[group];
    GlyphBatch result;
    result.slotIndex    = batch >> 8;
    result.slot         = scene->glyphChunksPtr[result.slotIndex];
    result.glyph        = (batch & 0xFF) * GLYPH_BATCH_GLYPHS + thread;
    result.instance     = scene->instancesPtr + result.slot.row;
    result.chunk        = loadGlyphChunk(scene, uint(scene->geometryPtr[result.instance->geometryIndex].vertexOffset));
    result.shape        = scene->geometryPtr + uint(result.instance->meshletVisibility);
    return result;
}

// The glyph's shape sphere in render space; its scale also takes the glyph's largest, which selectLod measures by.
Sphere getGlyphSphere(SceneData *scene, InstanceData *instance, GeometryData *shape, Glyph glyph)
{
    float3 scale        = abs(glyph.scale);
    float largest       = max(scale.x, max(scale.y, scale.z));
    float3 center       = glyph.position + mul(getQuaternionMatrix(glyph.orientation), shape->boundsCenter * glyph.scale);
    Sphere sphere       = getInstanceSphere(scene, instance, center, shape->boundsRadius * largest);
    sphere.scale       *= largest;
    return sphere;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void resetMain()
{
    GlyphCullHeader *header = pc.scene->glyphCullPtr;
    header->groups      = 0;
    header->groupsY     = 1;
    header->groupsZ     = 1;
    header->chunkCount  = 0;
    header->commandCount = 0;
}

[shader("compute")]
[numthreads(GLYPH_BATCH_GLYPHS, 1, 1)]
void countMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    GlyphBatch batch    = getGlyphBatch(scene, groupId.x, threadId.x);
    if (threadId.x < MAX_MESH_LODS)
        lodCounts[threadId.x] = 0;
    GroupMemoryBarrierWithGroupSync();

    bool inChunk        = batch.glyph < batch.chunk.glyphCount;
    bool visible        = false;
    uint lod            = 0;
    uint place          = 0;
    if (inChunk)
    {
        Glyph glyph     = loadGlyph(scene, batch.chunk, batch.glyph);
        Sphere sphere   = getGlyphSphere(scene, batch.instance, batch.shape, glyph);
        visible         = isInFrustum(scene, sphere) && (scene->hizLevelCount == 0 || !isOccluded(scene, sphere));
        if (visible)
        {
            lod         = min(selectLod(scene, batch.shape, sphere), batch.slot.lodCount - 1);
            InterlockedAdd(lodCounts[lod], 1u, place);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (threadId.x < batch.slot.lodCount && lodCounts[threadId.x] > 0)
    {
        uint first      = 0;
        InterlockedAdd(scene->glyphChunksPtr[batch.slotIndex].counts[threadId.x], lodCounts[threadId.x], first);
        lodPlaces[threadId.x] = first;
    }
    if (threadId.x == 0)
        InterlockedAdd(scene->cullStatsPtr[STATS_GLYPHS], min(batch.chunk.glyphCount - min(batch.glyph, batch.chunk.glyphCount), GLYPH_BATCH_GLYPHS));
    GroupMemoryBarrierWithGroupSync();

    if (inChunk)
        scene->glyphSlotsPtr[batch.slot.firstBatch * GLYPH_BATCH_GLYPHS + batch.glyph] = visible ? (lodPlaces[lod] + place) | lod << GLYPH_LOD_SHIFT : GLYPH_CULLED;
}

[shader("compute")]
[numthreads(GLYPH_BATCH_GLYPHS, 1, 1)]
void writeMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    GlyphBatch batch    = getGlyphBatch(scene, groupId.x, threadId.x);
    // Each LOD's glyphs start after the chunk's glyphs of the LODs before it.
    if (threadId.x < MAX_MESH_LODS)
    {
        uint first      = 0;
        for (uint l = 0; l < threadId.x; ++l)
            first      += batch.slot.counts[l];
        lodPlaces[threadId.x] = batch.slot.firstBatch * GLYPH_BATCH_GLYPHS + first;
    }
    GroupMemoryBarrierWithGroupSync();

    if (batch.glyph < batch.chunk.glyphCount)
    {
        uint packed     = scene->glyphSlotsPtr[batch.slot.firstBatch * GLYPH_BATCH_GLYPHS + batch.glyph];
        if (packed != GLYPH_CULLED)
            scene->glyphListPtr[lodPlaces[packed >> GLYPH_LOD_SHIFT] + (packed & GLYPH_PLACE_MASK)] = batch.slotIndex << 16 | batch.glyph;
    }

    // The chunk's first batch writes its commands: a LOD with no glyph draws no instance.
    if (batch.glyph == threadId.x && threadId.x < batch.slot.lodCount)
    {
        GeometryLod lodRange = batch.shape->lods[threadId.x];
        uint count      = batch.slot.counts[threadId.x];
        DrawInfo command;
        command.indexCount      = lodRange.indexCount;
        command.instanceCount   = count;
        command.firstIndex      = lodRange.firstIndex;
        command.vertexOffset    = batch.shape->vertexOffset;
        command.firstInstance   = lodPlaces[threadId.x];
        scene->glyphCommandsPtr[batch.slot.firstCommand + threadId.x] = command;
        if (count > 0)
        {
            InterlockedAdd(scene->cullStatsPtr[STATS_GLYPHS_VISIBLE], count);
            uint64_t triangles = uint64_t(count) * (lodRange.indexCount / 3);
            addCount64(scene->cullStatsPtr + STATS_TRIANGLES, uint(triangles), uint(triangles >> 32));
        }
    }
}
// clang-format on
