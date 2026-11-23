// clang-format off
#pragma once

#include "core.slang"
#include "meshletCulling.slang"

// The mesh-shader path of meshlets (MeshletPath::MESH_SHADERS). A task workgroup takes one chunk of 64 meshlets that
// passes/cull.slang's writeMain wrote, tests them as meshletCountMain does (cullMeshletChunk), and launches a mesh
// workgroup per visible meshlet, in the chunk's order. The mesh shaders (meshMain in passes/rasm.slang and
// passes/shadow.slang) read the meshlet's own vertices and triangles from SceneData::meshletDataPtr. So the meshlets need
// no draw list and no pass of their own: the draw pass culls them.

static const uint   MESHLET_MAX_VERTICES    = 64;       // Must match MESHLET_MAX_VERTICES in src/assets/meshlets.h.
static const uint   MESHLET_MAX_TRIANGLES   = 124;      // Must match MESHLET_MAX_TRIANGLES in src/assets/meshlets.h.

struct MeshletPayload
{
    uint            chunk;                              // in meshletChunksPtr
    uint            lanes[MESHLET_CHUNK_SIZE];          // the visible meshlets' places in the chunk, in order
};

groupshared MeshletPayload taskPayload;

[shader("amplification")]
[numthreads(MESHLET_CHUNK_SIZE, 1, 1)]
void taskMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    MeshletChunk chunk  = getMeshletChunk(scene, groupId);
    uint count          = 0;
    if (chunk.valid)
    {
        uint2 mask      = cullMeshletChunk(scene, chunk, threadId.x);
        if (isInMask(mask, threadId.x))
            taskPayload.lanes[countMaskBelow(mask, threadId.x)] = threadId.x;
        count           = countbits(mask.x) + countbits(mask.y);
    }
    if (threadId.x == 0)
        taskPayload.chunk = chunk.index;
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(count, 1, 1, taskPayload);
}

// What a mesh workgroup draws: one visible meshlet of the payload's chunk.
struct MeshletDraw
{
    DrawInfo        draw;
    InstanceData*   instance;
    uint            lod;
    uint            firstIndex;                         // its index range's start, for the MESHLETS debug view's key
    uint            firstData;
    uint            vertexCount;
    uint            triangleCount;
};

MeshletDraw getMeshletDraw(SceneData *scene, MeshletPayload payload, uint group)
{
    uint4 entry         = scene->meshletChunksPtr[payload.chunk];
    MeshletDraw meshlet;
    meshlet.draw        = scene->allDrawsPtr[entry.x];
    meshlet.instance    = scene->instancesPtr + meshlet.draw.firstInstance;
    meshlet.lod         = entry.y & CHUNK_LOD_MASK;
    GeometryLod lod     = scene->geometryPtr[meshlet.instance->geometryIndex].lods[meshlet.lod];
    GeometryMeshlet geometry = scene->meshletsPtr[lod.firstMeshlet + entry.z + payload.lanes[group]];
    meshlet.firstIndex  = geometry.firstIndex;
    meshlet.firstData   = geometry.firstData;
    meshlet.vertexCount = geometry.vertexCount;
    meshlet.triangleCount = geometry.indexCount / 3;
    return meshlet;
}

// The meshlet's vertex i: a vertex of its sub-mesh, as an index buffer would name it.
uint getMeshletVertex(SceneData *scene, MeshletDraw meshlet, uint i)
{
    return scene->meshletDataPtr[meshlet.firstData + i];
}

// The meshlet's triangle t: three of its own vertices, a byte each.
uint3 getMeshletTriangle(SceneData *scene, MeshletDraw meshlet, uint t)
{
    uint packed         = scene->meshletDataPtr[meshlet.firstData + meshlet.vertexCount + t];
    return uint3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF);
}
// clang-format on
