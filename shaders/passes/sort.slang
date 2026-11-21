// clang-format off
#include "core.slang"
#include "frustumCulling.slang"

// Sorts the visible draws of the blend run back to front, after the cull pass that kept them (gpuScene.cpp,
// recordBlendSort). A bitonic sort over sortCapacity keys, a power of two of at least SORT_BLOCK: the draws past the
// visible count get the largest key and go to the end. Each key is (~view depth, draw), so an ascending sort puts
// the farthest draw first, and two draws at the same depth keep a fixed order.
//
//   sortKeysMain       a key per element, the draws copied aside, then each block of SORT_BLOCK sorted in shared memory
//   sortMergeMain      one compare step over the whole array, for the steps of distance SORT_BLOCK and more
//   sortLocalMergeMain the steps below SORT_BLOCK of one merge, inside each block
//   sortWriteMain      the copied draws written back in sorted order
//
// pc.argument holds log2 of the merge size k (bits 8 to 15) and of the step's distance j (bits 0 to 7). Element i
// and i + j are put in order, ascending where i & k is 0, descending elsewhere. Must match blendSort.cpp.

static const uint   SORT_BLOCK          = 1024;
static const uint   SORT_GROUP_SIZE     = 256;          // each thread handles 2 of a block's 512 pairs per step
static const uint2  SORT_PADDING        = uint2(0xFFFFFFFF, 0xFFFFFFFF);

groupshared uint2   blockKeys[SORT_BLOCK];

bool isBefore(uint2 a, uint2 b)
{
    return a.x < b.x || (a.x == b.x && a.y < b.y);
}

uint getBlendCount(SceneData *scene)
{
    return min(scene->drawCountsPtr[scene->blendRun], scene->sortCapacity);
}

// The lower element of pair p for a step of distance j.
uint getPairFirst(uint pair, uint j)
{
    return 2 * j * (pair / j) + pair % j;
}

void orderPair(inout uint2 a, inout uint2 b, bool ascending)
{
    if (isBefore(b, a) == ascending)
    {
        uint2 t = a;
        a = b;
        b = t;
    }
}

// The steps j = from, from / 2, ..., 1 of the merge of size k, on the block that starts at element blockStart.
void mergeBlock(uint blockStart, uint thread, uint k, uint from)
{
    for (uint j = from; j > 0; j >>= 1)
    {
        for (uint p = thread; p < SORT_BLOCK / 2; p += SORT_GROUP_SIZE)
        {
            uint i = getPairFirst(p, j);
            uint2 a = blockKeys[i];
            uint2 b = blockKeys[i + j];
            orderPair(a, b, ((blockStart + i) & k) == 0);
            blockKeys[i] = a;
            blockKeys[i + j] = b;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[shader("compute")]
[numthreads(SORT_GROUP_SIZE, 1, 1)]
void sortKeysMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    uint count          = getBlendCount(scene);
    uint first          = scene->runFirst[scene->blendRun];
    uint blockStart     = groupId.x * SORT_BLOCK;
    for (uint e = threadId.x; e < SORT_BLOCK; e += SORT_GROUP_SIZE)
    {
        uint i          = blockStart + e;
        uint2 key       = SORT_PADDING;
        if (i < count)
        {
            DrawInfo draw   = scene->drawInfoPtr[first + i];
            scene->sortDrawsPtr[i] = draw;
            // The view depth of the bounds' center: positive in front of the camera, which looks down -Z.
            float depth     = -mul(scene->view, float4(getWorldSphere(scene, draw).center, 1.0)).z;
            key             = uint2(~asuint(max(depth, 0.0)), i);
        }
        blockKeys[e]    = key;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint k = 2; k <= SORT_BLOCK; k <<= 1)
        mergeBlock(blockStart, threadId.x, k, k >> 1);

    for (uint e = threadId.x; e < SORT_BLOCK; e += SORT_GROUP_SIZE)
        scene->sortKeysPtr[blockStart + e] = blockKeys[e];
}

[shader("compute")]
[numthreads(SORT_GROUP_SIZE, 1, 1)]
void sortMergeMain(uint3 dispatchId : SV_DispatchThreadID)
{
    SceneData *scene    = pc.scene;
    uint k              = 1u << ((pc.argument >> 8) & 0xFF);
    uint j              = 1u << (pc.argument & 0xFF);
    uint i              = getPairFirst(dispatchId.x, j);
    if (i + j >= scene->sortCapacity)
        return;
    uint2 a             = scene->sortKeysPtr[i];
    uint2 b             = scene->sortKeysPtr[i + j];
    orderPair(a, b, (i & k) == 0);
    scene->sortKeysPtr[i]       = a;
    scene->sortKeysPtr[i + j]   = b;
}

[shader("compute")]
[numthreads(SORT_GROUP_SIZE, 1, 1)]
void sortLocalMergeMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene    = pc.scene;
    uint k              = 1u << ((pc.argument >> 8) & 0xFF);
    uint blockStart     = groupId.x * SORT_BLOCK;
    for (uint e = threadId.x; e < SORT_BLOCK; e += SORT_GROUP_SIZE)
        blockKeys[e]    = scene->sortKeysPtr[blockStart + e];
    GroupMemoryBarrierWithGroupSync();

    mergeBlock(blockStart, threadId.x, k, SORT_BLOCK >> 1);

    for (uint e = threadId.x; e < SORT_BLOCK; e += SORT_GROUP_SIZE)
        scene->sortKeysPtr[blockStart + e] = blockKeys[e];
}

[shader("compute")]
[numthreads(SORT_GROUP_SIZE, 1, 1)]
void sortWriteMain(uint3 dispatchId : SV_DispatchThreadID)
{
    SceneData *scene    = pc.scene;
    uint i              = dispatchId.x;
    if (i < getBlendCount(scene))
        scene->drawInfoPtr[scene->runFirst[scene->blendRun] + i] = scene->sortDrawsPtr[scene->sortKeysPtr[i].y];
}
// clang-format on
