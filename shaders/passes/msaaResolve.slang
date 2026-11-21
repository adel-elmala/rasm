// clang-format off
// The MSAA resolve (src/renderer/msaaResolve.cpp): each thread averages the samples of one pixel of a multisampled
// color into a single-sample storage image. Each sample is weighted by 1 / (1 + its exposed luminance), which is the
// mean of the samples after a reversible tone curve (x / (1 + x)), turned back: a bright sample can no longer outweigh
// the others, so an edge between a bright sky and a dark roof keeps its in-between shade after tone mapping.

// Must match gfx::ResolvePushConstants in src/renderer/uberMaterialTypes.h; validateRasmStructs checks it by name.
struct ResolvePushConstants
{
    uint32_t    source;     // bindless index of the multisampled color
    uint32_t    target;     // storage slot of the resolved color
    uint32_t    samples;    // per pixel of the source
    float       exposure;   // the camera's, so the weights see the light as the tone map will
};

[[vk::push_constant]]
ConstantBuffer<ResolvePushConstants> pc;

[[vk::binding(0, 0)]]
Sampler2DMS<float4> gMsTextures[];

[[vk::binding(1, 0)]]
[format("rgba16f")]
RWTexture2DArray<float4> gStorageImages[];

float getLuma(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolveMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height, layers;
    gStorageImages[pc.target].GetDimensions(width, height, layers);
    if (id.x >= width || id.y >= height)
        return;
    Sampler2DMS<float4> source = gMsTextures[pc.source];
    float3 sum = float3(0.0);
    float weights = 0.0;
    for (uint sample = 0; sample < pc.samples; ++sample)
    {
        float3 color = max(source.Load(int2(id.xy), int(sample)).rgb, 0.0);
        float weight = 1.0 / (1.0 + getLuma(color) * pc.exposure);
        sum += color * weight;
        weights += weight;
    }
    gStorageImages[pc.target][uint3(id.xy, 0)] = float4(sum / weights, 1.0);
}

// clang-format on
