// clang-format off
#pragma once

#include "core.slang"
#include "frustumCulling.slang"
#include "occlusionCulling.slang"

// The meshlet test of one chunk, shared by passes/cull.slang's meshletCountMain (vertex pulling) and the task shaders of
// lib/meshletTask.slang (mesh shaders): the same meshlets pass on both paths, so both draw the same image.

static const uint   CULL_EARLY          = 0;            // Must match CULL_EARLY and CULL_LATE in gpuScene.cpp.
static const uint   CULL_LATE           = 1;
static const uint   MESHLET_CHUNK_SIZE  = 64;           // meshlets per chunk, a thread each
// A chunk: x = the draw in allDrawsPtr, y = lod | CHUNK_* flags, z = its first meshlet in the LOD, w = its meshlets.
static const uint   CHUNK_LOD_MASK      = 0xFF;
static const uint   CHUNK_WHOLE         = 1u << 8;
static const uint   CHUNK_DRAWN_EARLY   = 1u << 9;
static const uint   CHUNK_EARLY_MESHLETS = 1u << 10;
// The meshlet dispatches' and task draws' groups: x, then rows of x. Must match MESHLET_DISPATCH_WIDTH in gpuScene.cpp.
static const uint   MESHLET_DISPATCH_WIDTH = 65535;
// The meshlet dispatches' and task draws' pc.argument: the phase, and the run above it.
static const uint   MESHLET_RUN_SHIFT   = 4;
static const uint   MESHLET_CHUNK_GROUP = 64;           // chunks per sum of chunkSumsPtr; each run's chunks start at a multiple. Must match gpuScene.cpp.
// cullStatsPtr words the meshlet test adds to. Must match the CULL_STATS_* words in gpuScene.h.
static const uint   STATS_TRIANGLES     = 1;
static const uint   STATS_MESHLETS      = 5 + MAX_MESH_LODS + 2;
static const uint   STATS_MESHLETS_CULLED = STATS_MESHLETS + 1;
static const uint   STATS_MESHLETS_WHOLE = STATS_MESHLETS + 2;

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

// A meshlet's back-face test: meshoptimizer's cone test (meshopt_computeMeshletBounds), done in the instance's own
// space. Which side of a triangle's plane a point is on does not change under an affine matrix, also one that scales
// unevenly or mirrors, so the camera is moved into object space instead of the cone into world space. The test holds
// only for runs culled by face (SceneData::coneRuns): a mirroring instance is drawn with FRONT culling, which drops the
// same triangles.
bool isMeshletBackFacing(SceneData *scene, InstanceData *instance, GeometryMeshlet meshlet)
{
    float4x4 world          = getWorldMatrix(scene, instance);
    // The normal matrix is the inverse transpose of the world matrix's 3x3, so its transpose is the inverse.
    float3x3 toObject       = transpose(getNormalMatrix(scene, instance));
    float4 viewPoint        = scene->cullViewPoint;
    if (viewPoint.w == 0.0)
    {
        // An orthographic view looks the same way from everywhere.
        float3 direction    = normalize(mul(toObject, viewPoint.xyz));
        return dot(direction, meshlet.coneAxis) >= meshlet.coneCutoff;
    }
    float3 camera           = mul(toObject, viewPoint.xyz - mul(world, float4(0.0, 0.0, 0.0, 1.0)).xyz);
    float3 toCenter         = meshlet.center - camera;
    return dot(toCenter, meshlet.coneAxis) >= meshlet.coneCutoff * length(toCenter) + meshlet.radius;
}

// A meshlet's place among an instance's visibility bits: the bit of meshlet m of the instance's LOD.
bool readMeshletBit(SceneData *scene, InstanceData *instance, uint meshlet)
{
    return (scene->meshletVisibilityPtr[uint(instance->meshletVisibility) + meshlet / 32] & (1u << (meshlet % 32))) != 0;
}

struct MeshletChunk
{
    uint            run;
    uint            index;                              // in meshletChunksPtr
    uint4           entry;
    bool            valid;
};

// The chunk of a meshlet dispatch's or task draw's group: the run in pc.argument, the group among the run's chunks.
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

groupshared uint    chunkMask[2];
groupshared uint    chunkBits[2];
groupshared uint    chunkTriangles;
groupshared uint    chunkCulled;

// Tests a chunk's meshlets, a thread each, and returns the ones to draw in this phase as a 64-bit mask. Every thread of
// the group calls it. The late phase also writes the chunk's visibility bits: each chunk holds 64 meshlets from a
// multiple of 64, so its two words are its own. The meshlets drawn, their triangles and those culled go into the stats.
uint2 cullMeshletChunk(SceneData *scene, MeshletChunk chunk, uint lane)
{
    uint phase          = pc.argument & 1;
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
                // A draw the early phase drew whole has none culled.
                culled      = !seen && (entry.y & CHUNK_DRAWN_EARLY) == 0;
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
    uint2 mask          = uint2(chunkMask[0], chunkMask[1]);
    if (lane == 0)
    {
        uint visibleCount = countbits(mask.x) + countbits(mask.y);
        if (visibleCount > 0)
        {
            InterlockedAdd(scene->cullStatsPtr[STATS_MESHLETS], visibleCount);
            addCount64(scene->cullStatsPtr + STATS_TRIANGLES, chunkTriangles, 0);
        }
        if (chunkCulled > 0)
            InterlockedAdd(scene->cullStatsPtr[STATS_MESHLETS_CULLED], chunkCulled);
    }
    return mask;
}

// The visible meshlets of a mask below a lane: where the lane's meshlet goes among the chunk's visible ones.
uint countMaskBelow(uint2 mask, uint lane)
{
    return lane < 32 ? countbits(mask.x & ((1u << lane) - 1)) : countbits(mask.x) + countbits(mask.y & ((1u << (lane - 32)) - 1));
}

bool isInMask(uint2 mask, uint lane)
{
    return ((lane < 32 ? mask.x : mask.y) & (1u << (lane % 32))) != 0;
}
// clang-format on
