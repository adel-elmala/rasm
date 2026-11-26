// clang-format off
// The weighted composite (src/renderer/weightedBlend.cpp): one triangle over the view's target in the Transparent pass,
// after the grid and before the sorted see-through draws. Each pixel divides the weighted sums for the mean color of
// its weighted surfaces and lays it over what is behind by their coverage, with PREMULTIPLIED_ALPHA:
//
//     result = mean * coverage + behind * (1 - coverage),    mean = sum(color * alpha * weight) / sum(alpha * weight)
//
// With a motion target the motion goes the same way, the weighted mean motion over the motion behind. A multisampled
// target is composited once per sample (RASM_PER_SAMPLE, sampleRateShading), so each sample keeps its own surfaces; a
// device without sample shading composites each pixel's mean of its samples (RASM_MULTISAMPLED alone).

#include "core.slang"

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

[[vk::binding(0, 0)]]
Sampler2DMS<float4> gMsTextures[];

struct VSOutput
{
    float4 pos : SV_POSITION;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID)
{
    float2 uv       = float2((vertexID << 1) & 2, vertexID & 2);
    VSOutput output;
    output.pos      = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return output;
}

#if defined(RASM_MOTION_VECTORS)
struct FragmentOutput
{
    float4 color  : SV_TARGET0;
    float4 motion : SV_TARGET1;
};
#endif

float4 loadWeighted(uint texture, int2 pixel, uint sample)
{
#if defined(RASM_PER_SAMPLE)
    return gMsTextures[texture].Load(pixel, int(sample));
#elif defined(RASM_MULTISAMPLED)
    float4 sum = float4(0.0);
    for (uint s = 0; s < pc.scene->weightedSamples; ++s)
        sum += gMsTextures[texture].Load(pixel, int(s));
    return sum / float(pc.scene->weightedSamples);
#else
    return gTextures[texture].Load(int3(pixel, 0));
#endif
}

[shader("fragment")]
#if defined(RASM_MOTION_VECTORS)
FragmentOutput fragMain(VSOutput input
#else
float4 fragMain(VSOutput input
#endif
#if defined(RASM_PER_SAMPLE)
    , uint sample : SV_SampleIndex
#endif
    )
#if !defined(RASM_MOTION_VECTORS)
    : SV_TARGET
#endif
{
#if !defined(RASM_PER_SAMPLE)
    uint sample         = 0;
#endif
    SceneData *scene    = pc.scene;
    int2 pixel          = int2(input.pos.xy);
    // With motion the coverage is the alpha of the target that sums the motion in rg (BlendMode::WEIGHTED_MOTION).
    float4 sums         = loadWeighted(scene->weightedCoverage, pixel, sample);
#if defined(RASM_MOTION_VECTORS)
    float coverage      = saturate(sums.a);
#else
    float coverage      = saturate(sums.r);
#endif
    // Nothing weighted here: the blend keeps what is behind.
    if (coverage <= 0.0)
        discard;
    float4 accum        = loadWeighted(scene->weightedAccum, pixel, sample);
    float3 mean         = accum.rgb / max(accum.a, 1e-6);
    float4 color        = float4(mean * coverage, coverage);
#if defined(RASM_MOTION_VECTORS)
    FragmentOutput output;
    output.color        = color;
    float2 motion       = sums.rg / max(accum.a, 1e-6);
    output.motion       = float4(motion * coverage, 0.0, coverage);
    return output;
#else
    return color;
#endif
}

// clang-format on
