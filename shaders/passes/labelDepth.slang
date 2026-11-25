// clang-format off
#include "core.slang"

// The Label Depth pass: after a shaded view's draw passes and before its Transparent pass, each label slot's anchor is
// tested against the depth they left, and the label fades in or out whole by the answer. Its glyphs are then drawn
// over everything (lib/vector.slang), so a label is never cut glyph by glyph by the ground or a thing in front of it.

static const uint   LABEL_DEPTH_GROUP_SIZE  = 64;       // Must match LABEL_DEPTH_GROUP_SIZE in gpuScene.cpp.
// SceneData::labelProbesPtr[slot].w. Must match LABEL_PROBE_* in src/renderer/labelPlacement.h.
static const uint   LABEL_PROBE_NONE        = 0;        // not in the view: the slot is left as it is
static const uint   LABEL_PROBE_DEPTH       = 1;        // tested against the depth
static const uint   LABEL_PROBE_OVERLAY     = 2;        // an overlay's: always shown
static const uint   LABEL_PROBE_RESET       = 4;        // added when the label was hidden: its fade starts at the answer

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

// The same bindless array, for a multisampled depth: a slot is read only through the declaration that matches it.
[[vk::binding(0, 0)]]
Sampler2DMS<float> gMsTextures[];

// The farthest depth (reversed-Z: the smallest) of one pixel, of all its samples with MSAA.
float loadDepth(SceneData *scene, int2 pixel)
{
    if (scene->depthSamples <= 1)
        return gTextures[scene->depthTexture].Load(int3(pixel, 0)).r;
    Sampler2DMS<float> depth = gMsTextures[scene->depthTexture];
    float farthest = 1.0;
    for (uint sample = 0; sample < scene->depthSamples; ++sample)
        farthest = min(farthest, depth.Load(pixel, int(sample)));
    return farthest;
}

// Whether the anchor is in front of the depth at one of the 3 x 3 pixels around its own: the ground it stands on is
// farther than it above it on screen, and a thin thing in front of it or an edge does not hide it.
bool isAnchorVisible(SceneData *scene, float4 probe)
{
    int2 size       = int2(scene->viewportWidth, scene->viewportHeight);
    int2 center     = int2(floor(probe.xy));
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 pixel = clamp(center + int2(x, y), int2(0), size - 1);
            if (loadDepth(scene, pixel) <= probe.z)
                return true;
        }
    }
    return false;
}

// One label slot: its fade by depth steps toward its answer (or starts at it, for a label that was hidden), and the
// slot's alpha, its placement's, takes it.
[shader("compute")]
[numthreads(LABEL_DEPTH_GROUP_SIZE, 1, 1)]
void labelDepthMain(uint3 threadId : SV_DispatchThreadID)
{
    SceneData *scene    = pc.scene;
    uint slot           = threadId.x;
    if (slot >= scene->labelSlotCount)
        return;
    float4 probe        = scene->labelProbesPtr[slot];
    uint test           = uint(probe.w + 0.5);
    if (test == LABEL_PROBE_NONE)
        return;
    bool reset          = test >= LABEL_PROBE_RESET;
    test               -= reset ? LABEL_PROBE_RESET : 0;
    float target        = test == LABEL_PROBE_OVERLAY || isAnchorVisible(scene, probe) ? 1.0 : 0.0;
    float fade          = reset ? target : clamp(scene->labelFadesPtr[slot] + (target > 0.5 ? scene->labelFadeStep : -scene->labelFadeStep), 0.0, 1.0);
    scene->labelFadesPtr[slot]  = fade;
    scene->labelsPtr[slot]     *= fade;
}
// clang-format on
