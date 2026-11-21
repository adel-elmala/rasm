// clang-format off
// The bloom chain (src/renderer/bloom.cpp), after Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare" (SIGGRAPH 2014). The down passes halve the image again and again with a 13-tap filter; the up passes walk
// back, each adding a tent-blurred copy of the level below to its own level. Each thread writes one texel of the
// target, which is a storage image the pass only writes; every input is sampled, bilinear and clamped.

// Must match gfx::BloomPushConstants in src/renderer/uberMaterialTypes.h; validateRasmStructs checks it by name.
struct BloomPushConstants
{
    uint32_t    source;     // bindless texture: the level above (down), or the level below (up)
    uint32_t    target;     // storage slot of the level to write
    uint32_t    base;       // bindless texture: the up pass's own level of the down chain
    float       scale;      // the up pass's result is multiplied by it: 1 / the level count on the last one
};

[[vk::push_constant]]
ConstantBuffer<BloomPushConstants> pc;

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

[[vk::binding(1, 0)]]
[format("rgba16f")]
RWTexture2DArray<float4> gStorageImages[];

static const uint INVALID_TEXTURE_INDEX = 0xFFFFFFFF;

// The target texel's center in UV, or false for a thread past the target's edge.
bool getTargetUV(uint3 id, out float2 uv)
{
    uint width, height, layers;
    gStorageImages[pc.target].GetDimensions(width, height, layers);
    uv = (float2(id.xy) + 0.5) / float2(width, height);
    return id.x < width && id.y < height;
}

float3 sampleSource(uint texture, float2 uv)
{
    return gTextures[texture].SampleLevel(uv, 0.0).rgb;
}

float getLuma(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// The mean of a 2x2 box, weighted down by its brightness (Karis 2013): one very bright pixel then adds about as much
// as a dim one instead of flashing as the camera moves.
float getKarisWeight(float3 box)
{
    return 1.0 / (1.0 + getLuma(box));
}

// 13 bilinear taps around the target texel, which sits on a corner of 2x2 source texels: each tap is the mean of
// 2x2 texels, so they cover 6x6. They form five 2x2 boxes of taps, the inner one weighted 0.5 and the four corner
// ones 0.125, which keeps a flat image flat.
//    a . b . c
//    . j . k .
//    d . e . f
//    . l . m .
//    g . h . i
float3 downsample(float2 uv, bool karis)
{
    uint width, height;
    gTextures[pc.source].GetDimensions(width, height);
    float2 t = 1.0 / float2(width, height);
    float3 a = sampleSource(pc.source, uv + t * float2(-2.0, -2.0));
    float3 b = sampleSource(pc.source, uv + t * float2( 0.0, -2.0));
    float3 c = sampleSource(pc.source, uv + t * float2( 2.0, -2.0));
    float3 d = sampleSource(pc.source, uv + t * float2(-2.0,  0.0));
    float3 e = sampleSource(pc.source, uv);
    float3 f = sampleSource(pc.source, uv + t * float2( 2.0,  0.0));
    float3 g = sampleSource(pc.source, uv + t * float2(-2.0,  2.0));
    float3 h = sampleSource(pc.source, uv + t * float2( 0.0,  2.0));
    float3 i = sampleSource(pc.source, uv + t * float2( 2.0,  2.0));
    float3 j = sampleSource(pc.source, uv + t * float2(-1.0, -1.0));
    float3 k = sampleSource(pc.source, uv + t * float2( 1.0, -1.0));
    float3 l = sampleSource(pc.source, uv + t * float2(-1.0,  1.0));
    float3 m = sampleSource(pc.source, uv + t * float2( 1.0,  1.0));

    float3 boxes[5] = { (j + k + l + m) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25,
                        (d + e + g + h) * 0.25, (e + f + h + i) * 0.25 };
    float  weights[5] = { 0.5, 0.125, 0.125, 0.125, 0.125 };
    float3 sum = float3(0.0);
    float  weightSum = 0.0;
    for (uint n = 0; n < 5; ++n)
    {
        float w = weights[n] * (karis ? getKarisWeight(boxes[n]) : 1.0);
        sum += boxes[n] * w;
        weightSum += w;
    }
    return sum / weightSum;
}

// The first level reads the scene, where a single pixel can be very bright.
[shader("compute")]
[numthreads(8, 8, 1)]
void firstDownsampleMain(uint3 id : SV_DispatchThreadID)
{
    float2 uv;
    if (getTargetUV(id, uv))
        gStorageImages[pc.target][uint3(id.xy, 0)] = float4(downsample(uv, true), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downsampleMain(uint3 id : SV_DispatchThreadID)
{
    float2 uv;
    if (getTargetUV(id, uv))
        gStorageImages[pc.target][uint3(id.xy, 0)] = float4(downsample(uv, false), 1.0);
}

// A 3x3 tent of the level below, one of its texels apart, added to this level of the down chain.
//    1 2 1
//    2 4 2  / 16
//    1 2 1
[shader("compute")]
[numthreads(8, 8, 1)]
void upsampleMain(uint3 id : SV_DispatchThreadID)
{
    float2 uv;
    if (!getTargetUV(id, uv))
        return;
    float3 color = sampleSource(pc.base, uv);
    if (pc.source != INVALID_TEXTURE_INDEX)
    {
        uint width, height;
        gTextures[pc.source].GetDimensions(width, height);
        float2 t = 1.0 / float2(width, height);
        float3 tent = sampleSource(pc.source, uv) * 4.0;
        tent += (sampleSource(pc.source, uv + float2(-t.x, 0.0)) + sampleSource(pc.source, uv + float2(t.x, 0.0)) +
                 sampleSource(pc.source, uv + float2(0.0, -t.y)) + sampleSource(pc.source, uv + float2(0.0, t.y))) * 2.0;
        tent += sampleSource(pc.source, uv + float2(-t.x, -t.y)) + sampleSource(pc.source, uv + float2(t.x, -t.y)) +
                sampleSource(pc.source, uv + float2(-t.x, t.y)) + sampleSource(pc.source, uv + float2(t.x, t.y));
        color += tent / 16.0;
    }
    gStorageImages[pc.target][uint3(id.xy, 0)] = float4(color * pc.scale, 1.0);
}

// clang-format on
