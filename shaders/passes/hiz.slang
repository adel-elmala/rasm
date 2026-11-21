// clang-format off
#include "core.slang"
#include "occlusionCulling.slang"

// Builds the Hi-Z that occlusion culling tests against (lib/occlusionCulling.slang), from the depth the early
// draws left. The Hi-Z pass dispatches it once per level, level 0 first, with a barrier between levels.

static const uint   HIZ_GROUP_SIZE      = 8;            // Must match HIZ_GROUP_SIZE in gpuScene.cpp.

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

// The same bindless array, for a multisampled depth: a slot is read only through the declaration that matches it.
[[vk::binding(0, 0)]]
Sampler2DMS<float> gMsTextures[];

// The farthest depth (reversed-Z: the smallest) of one pixel: with several samples per pixel, the farthest sample, so
// the Hi-Z stays conservative where a pixel holds an edge. No resolved depth is needed.
float loadDepth(SceneData *scene, uint2 pixel)
{
    if (scene->depthSamples <= 1)
        return gTextures[scene->depthTexture].Load(int3(int2(pixel), 0)).r;
    Sampler2DMS<float> depth = gMsTextures[scene->depthTexture];
    float farthest = 1.0;
    for (uint sample = 0; sample < scene->depthSamples; ++sample)
        farthest = min(farthest, depth.Load(int2(pixel), int(sample)));
    return farthest;
}

// Builds Hi-Z level pc.argument: each texel is the farthest of the 2x2 texels under it in the level before, or of
// the 2x2 pixels (every sample of each) in the depth texture for level 0. Odd sizes repeat the last row or column, so
// every pixel is covered.
[shader("compute")]
[numthreads(HIZ_GROUP_SIZE, HIZ_GROUP_SIZE, 1)]
void hizMain(uint3 threadId : SV_DispatchThreadID)
{
    SceneData *scene    = pc.scene;
    uint level          = pc.argument;
    uint2 size;
    uint offset         = getHizLevel(scene, level, size);
    if (threadId.x >= size.x || threadId.y >= size.y)
        return;

    uint2 sourceSize    = uint2(scene->viewportWidth, scene->viewportHeight);
    uint sourceOffset   = 0;
    if (level > 0)
        sourceOffset    = getHizLevel(scene, level - 1, sourceSize);

    uint2 p0            = threadId.xy * 2;
    uint2 p1            = min(p0 + 1, sourceSize - 1);
    float farthest;
    if (level == 0)
    {
        farthest        = min(min(loadDepth(scene, p0), loadDepth(scene, uint2(p1.x, p0.y))),
                              min(loadDepth(scene, uint2(p0.x, p1.y)), loadDepth(scene, p1)));
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
