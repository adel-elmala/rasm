// clang-format off
#include "core.slang"
#include "occlusionCulling.slang"

// Builds the Hi-Z that occlusion culling tests against (lib/occlusionCulling.slang), from the depth the early
// draws left. drawScene dispatches it once per level, level 0 first, with a barrier between levels.

static const uint   HIZ_GROUP_SIZE      = 8;            // Must match HIZ_GROUP_SIZE in gpuScene.cpp.

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

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
