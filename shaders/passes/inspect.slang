// clang-format off
// The Inspect pass (src/renderer/inspector.cpp): copies one resource of the frame into the capture target, as the
// display values the RASM window's Buffers section shows with ImGui::Image. The graph runs it right after the pass that
// last writes the resource, so it sees what that pass left, even in a transient target whose memory a later target
// takes. Each thread writes one texel of the capture target's top-left width x height, from the source texel under it.
#define RASM_OWN_PUSH_CONSTANTS
#include "core.slang"
#include "heat.slang"
#include "lightClusters.slang"
#include "occlusionCulling.slang"

// Must match gfx::INSPECT_* in src/renderer/uberMaterialTypes.h and rasm::INSPECT_* in include/rasm/types.h.
static const uint   INSPECT_MODE_COLOR          = 0;
static const uint   INSPECT_MODE_MOTION         = 1;
static const uint   INSPECT_MODE_DEPTH          = 2;
static const uint   INSPECT_MODE_HIZ            = 3;
static const uint   INSPECT_MODE_CLUSTERS       = 4;
static const uint   INSPECT_FLAG_MULTISAMPLED   = 1 << 0;
static const uint   INSPECT_FLAG_CUBE           = 1 << 1;
static const uint   INSPECT_ALL_SAMPLES         = 0xFFFFFFFF;
static const uint   INSPECT_TILES               = CLUSTER_SLICES;
static const uint   INSPECT_GROUP_SIZE          = 8;            // Must match INSPECT_GROUP_SIZE in inspector.cpp.
// A motion of this many pixels times the exposure shows at full color.
static const float  MOTION_FULL_PIXELS          = 16.0;

// Must match gfx::InspectParams; validateRasmStructs checks it through pc.
struct InspectParams
{
    SceneData*  scene;          // the view's, for DEPTH, HIZ and CLUSTERS
    uint        mode;
    uint        source;         // bindless index of the texture
    uint        target;         // storage slot of the capture target
    uint        flags;
    uint        channel;        // 0 all, 1 to 4 one as gray
    uint        face;
    uint        level;          // HIZ: the level; CLUSTERS: the slice, or INSPECT_TILES
    uint        sample;
    uint        sourceWidth;    // of the level, in texels
    uint        sourceHeight;
    uint        width;          // of the region written
    uint        height;
    float       exposure;
    float       depthRange;
};

// Must match gfx::InspectPushConstants; validateRasmStructs checks it by name.
struct InspectPushConstants
{
    InspectParams*  params;
    uint            padding[2];
};

[[vk::push_constant]]
ConstantBuffer<InspectPushConstants> pc;

// One bindless array, declared once per kind of texture: a slot is read only through the declaration that matches it.
[[vk::binding(0, 0)]]
Sampler2D gTextures[];

[[vk::binding(0, 0)]]
Sampler2DMS<float4> gMsTextures[];

[[vk::binding(0, 0)]]
SamplerCube gCubeTextures[];

[[vk::binding(1, 0)]]
[format("rgba16f")]
RWTexture2DArray<float4> gStorageImages[];

// The UI shows a float texture's numbers as display values, so linear light is encoded first.
float3 encodeSrgb(float3 linear)
{
    float3 v = saturate(linear);
    return select(v <= 0.0031308, v * 12.92, 1.055 * pow(v, 1.0 / 2.4) - 0.055);
}

float3 hsvToRgb(float hue, float saturation, float value)
{
    float3 k = abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return value * lerp(float3(1.0), saturate(k - 1.0), saturation);
}

// A source texel of a 2D texture, multisampled or not. INSPECT_ALL_SAMPLES averages the samples, as a box resolve.
float4 loadTexel(InspectParams *p, uint2 texel)
{
    if ((p->flags & INSPECT_FLAG_MULTISAMPLED) == 0)
        return gTextures[p->source].Load(int3(int2(texel), 0));
    Sampler2DMS<float4> texture = gMsTextures[p->source];
    uint width, height, samples;
    texture.GetDimensions(width, height, samples);
    if (p->sample != INSPECT_ALL_SAMPLES)
        return texture.Load(int2(texel), int(min(p->sample, samples - 1)));
    float4 sum = float4(0.0);
    for (uint s = 0; s < samples; ++s)
        sum += texture.Load(int2(texel), int(s));
    return sum / float(samples);
}

// A cube face's texel, through its direction (the table of cube map face selection in the Vulkan spec): s and t go
// from -1 to 1 across the face, t down.
float4 loadCubeTexel(InspectParams *p, uint2 texel)
{
    float2 st   = (float2(texel) + 0.5) / float2(p->sourceWidth, p->sourceHeight) * 2.0 - 1.0;
    float s     = st.x;
    float t     = st.y;
    float3 directions[6] = {float3(1.0, -t, -s), float3(-1.0, -t, s), float3(s, 1.0, t), float3(s, -1.0, -t), float3(s, -t, 1.0), float3(-s, -t, -1.0)};
    return gCubeTextures[p->source].SampleLevel(directions[min(p->face, 5u)], 0.0);
}

// The view depth of a reversed-Z depth, through the view's projection (it has no skew): depth is
// (P[2][2] z + P[2][3]) / (P[3][2] z + P[3][3]) of the view-space z, which is minus the view depth. For RASM's
// perspective cameras that is near / viewDepth, with the far plane at infinity.
float getViewDepth(float4x4 projection, float depth)
{
    return -(projection[2][3] - depth * projection[3][3]) / (depth * projection[3][2] - projection[2][2]);
}

// A gray from 1 at the camera to 0 at depthRange and beyond, where the sky (depth 0) is too. An orthographic view's
// depth is linear already: 1 at its near plane, 0 at its far one.
float3 showDepth(InspectParams *p, float depth)
{
    if (p->scene->projection[3][3] == 1.0)
        return float3(saturate(depth));
    if (depth <= 0.0)
        return float3(0.0);
    return float3(saturate(1.0 - getViewDepth(p->scene->projection, depth) / p->depthRange));
}

// The direction as a hue and the length as how far from white: still pixels are white. getMotion's "no place last
// frame" (2, 2) is black.
float3 showMotion(InspectParams *p, float2 motion)
{
    if (any(motion > 1.5))
        return float3(0.0);
    float2 pixels   = motion * float2(p->sourceWidth, p->sourceHeight) * float2(1.0, -1.0);   // y up, like the picture
    float hue       = atan2(pixels.y, pixels.x) / (2.0 * 3.14159265) + 0.5;
    return hsvToRgb(hue, saturate(length(pixels) * p->exposure / MOTION_FULL_PIXELS), 1.0);
}

float3 showColor(InspectParams *p, float4 color)
{
    if (p->channel == 0)
        return encodeSrgb(color.rgb * p->exposure);
    return encodeSrgb(float3(color[min(p->channel, 4u) - 1] * p->exposure));
}

// The lights of one cluster of the slice, or of one screen tile, as heat; the tiles' edges are darker.
float3 showClusters(InspectParams *p, uint2 texel, uint2 id)
{
    uint32_t *clusters  = p->scene->clustersPtr;
    if (clusters == nullptr)
        return float3(0.0);
    uint tile           = texel.x + CLUSTER_TILES_X * texel.y;
    uint slice          = p->level;
    uint count          = clusters[TILE_COUNTS + tile];
    if (slice < INSPECT_TILES)
        count           = clusters[CLUSTER_COUNTS + tile + CLUSTER_TILES * slice];
    // The last texel of a tile's column or row: the next one shows the next tile.
    uint2 sourceSize    = uint2(p->sourceWidth, p->sourceHeight);
    uint2 size          = uint2(p->width, p->height);
    bool edge           = any((id + 1) * sourceSize / size != id * sourceSize / size);
    return getHeatColor(float(count)) * (edge ? 0.35 : 1.0);
}

float3 showHiz(InspectParams *p, uint2 texel)
{
    if (p->level >= p->scene->hizLevelCount)
        return float3(0.0);
    uint2 size;
    uint offset         = getHizLevel(p->scene, p->level, size);
    return showDepth(p, p->scene->hizPtr[offset + texel.y * size.x + texel.x]);
}

[shader("compute")]
[numthreads(INSPECT_GROUP_SIZE, INSPECT_GROUP_SIZE, 1)]
void inspectMain(uint3 id : SV_DispatchThreadID)
{
    InspectParams *p    = pc.params;
    if (id.x >= p->width || id.y >= p->height)
        return;
    uint2 sourceSize    = uint2(p->sourceWidth, p->sourceHeight);
    uint2 texel         = min(id.xy * sourceSize / uint2(p->width, p->height), sourceSize - 1);

    float3 color        = float3(0.0);
    if (p->mode == INSPECT_MODE_COLOR)
        color           = showColor(p, loadTexel(p, texel));
    else if (p->mode == INSPECT_MODE_MOTION)
        color           = showMotion(p, loadTexel(p, texel).xy);
    else if (p->mode == INSPECT_MODE_DEPTH)
        color           = showDepth(p, ((p->flags & INSPECT_FLAG_CUBE) != 0 ? loadCubeTexel(p, texel) : loadTexel(p, texel)).r);
    else if (p->mode == INSPECT_MODE_HIZ)
        color           = showHiz(p, texel);
    else if (p->mode == INSPECT_MODE_CLUSTERS)
        color           = showClusters(p, texel, id.xy);
    gStorageImages[p->target][uint3(id.xy, 0)] = float4(color, 1.0);
}
// clang-format on
