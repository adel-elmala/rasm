// clang-format off
// The environment bake (src/renderer/environmentBake.cpp). Compute only, once per environment: the equirectangular
// image becomes a cube with mips, which is filtered into the specular cube (one roughness per mip) and the
// irradiance cube. brdfMain makes the BRDF LUT, once per engine. Each thread writes one texel of one face.

#include "environment.slang"

// Must match gfx::EnvironmentPushConstants in src/renderer/uberMaterialTypes.h; validateRasmStructs checks it by name.
struct EnvironmentPushConstants
{
    uint32_t    source;         // bindless texture, or the storage slot of the mip before
    uint32_t    target;         // storage slot of the mip to write
    float       roughness;
    uint32_t    size;           // the target mip's width
};

[[vk::push_constant]]
ConstantBuffer<EnvironmentPushConstants> pc;

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

[[vk::binding(0, 0)]]
SamplerCube gCubeTextures[];

// Binding 1: one mip of a texture each, as a 2D array (6 layers for a cube, 1 for a 2D texture).
[[vk::binding(1, 0)]]
[format("rgba16f")]
RWTexture2DArray<float4> gStorageImages[];

static const uint   SPECULAR_SAMPLES    = 256;
static const uint   IRRADIANCE_SAMPLES  = 512;
static const uint   BRDF_SAMPLES        = 512;

float3 getTexelDirection(uint3 id)
{
    return getCubeDirection(id.z, (float2(id.xy) + 0.5) / float(pc.size));
}

// Four lookups per texel, at its quarters, so the image's finer rows near the poles are averaged instead of skipped.
[shader("compute")]
[numthreads(8, 8, 1)]
void cubeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.size || id.y >= pc.size)
        return;
    // The sampler repeats, so u wraps around the image's seam; v must not wrap, or the top row blends with the bottom
    // one at the poles, so it stays half a texel inside the image.
    uint width, height;
    gTextures[pc.source].GetDimensions(width, height);
    float halfTexel = 0.5 / float(height);
    float3 color = float3(0.0);
    for (uint i = 0; i < 4; ++i)
    {
        float2 st   = (float2(id.xy) + float2(0.25 + 0.5 * float(i & 1), 0.25 + 0.5 * float(i >> 1))) / float(pc.size);
        float2 uv   = getEquirectUV(getCubeDirection(id.z, st));
        uv.y        = clamp(uv.y, halfTexel, 1.0 - halfTexel);
        color      += gTextures[pc.source].SampleLevel(uv, 0.0).rgb;
    }
    gStorageImages[pc.target][id] = float4(color * 0.25, 1.0);
}

// Each mip is the average of the 2x2 texels under it in the mip before (sizes are powers of two).
[shader("compute")]
[numthreads(8, 8, 1)]
void downsampleMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.size || id.y >= pc.size)
        return;
    uint3 s         = uint3(id.xy * 2, id.z);
    float4 sum      = gStorageImages[pc.source][s] + gStorageImages[pc.source][s + uint3(1, 0, 0)] +
                      gStorageImages[pc.source][s + uint3(0, 1, 0)] + gStorageImages[pc.source][s + uint3(1, 1, 0)];
    gStorageImages[pc.target][id] = sum * 0.25;
}

// One mip of the specular cube: the environment seen through GGX of this roughness, with n = v = r (Karis 2013). A
// roughness of 0 is a mirror, so its mip copies the environment's mip of the same size.
[shader("compute")]
[numthreads(8, 8, 1)]
void specularMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.size || id.y >= pc.size)
        return;
    uint cubeSize, cubeHeight, cubeMips;
    gCubeTextures[pc.source].GetDimensions(0, cubeSize, cubeHeight, cubeMips);
    float3 n = getTexelDirection(id);
    if (pc.roughness <= 0.0)
    {
        gStorageImages[pc.target][id] = float4(gCubeTextures[pc.source].SampleLevel(n, log2(float(cubeSize) / float(pc.size))).rgb, 1.0);
        return;
    }

    float alpha     = pc.roughness * pc.roughness;
    float3 color    = float3(0.0);
    float weight    = 0.0;
    for (uint i = 0; i < SPECULAR_SAMPLES; ++i)
    {
        float3 h    = sampleGgx(getHammersley(i, SPECULAR_SAMPLES), alpha, n);
        float NoH   = saturate(dot(n, h));
        float3 l    = 2.0 * NoH * h - n;
        float NoL   = dot(n, l);
        if (NoL <= 0.0)
            continue;
        // With n = v, the pdf of l is D * n.h / (4 v.h) = D / 4.
        float pdf   = D_GGX(NoH, alpha) * 0.25;
        color      += gCubeTextures[pc.source].SampleLevel(l, getSampleMip(pdf, SPECULAR_SAMPLES, cubeSize, cubeMips)).rgb * NoL;
        weight     += NoL;
    }
    gStorageImages[pc.target][id] = float4(color / max(weight, 1e-6), 1.0);
}

// The irradiance cube: the mean light over each normal's hemisphere, weighted by cos(theta). With cosine-distributed
// samples that is their plain average, which is the irradiance divided by pi, so diffuse is it times the base color.
[shader("compute")]
[numthreads(8, 8, 1)]
void irradianceMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.size || id.y >= pc.size)
        return;
    uint cubeSize, cubeHeight, cubeMips;
    gCubeTextures[pc.source].GetDimensions(0, cubeSize, cubeHeight, cubeMips);
    float3 n        = getTexelDirection(id);
    float3 color    = float3(0.0);
    for (uint i = 0; i < IRRADIANCE_SAMPLES; ++i)
    {
        float3 l    = sampleCosine(getHammersley(i, IRRADIANCE_SAMPLES), n);
        float pdf   = max(dot(n, l), 1e-4) / PI;
        color      += gCubeTextures[pc.source].SampleLevel(l, getSampleMip(pdf, IRRADIANCE_SAMPLES, cubeSize, cubeMips)).rgb;
    }
    gStorageImages[pc.target][id] = float4(color / float(IRRADIANCE_SAMPLES), 1.0);
}

// The BRDF LUT at (n.v, roughness): the GGX specular integral for f0 = 0 and 1, as f0 * x + y. Each sample adds
// F * G_vis * 4 * n.l * v.h / n.h, the BRDF times n.l over the pdf of its half vector.
[shader("compute")]
[numthreads(8, 8, 1)]
void brdfMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.size || id.y >= pc.size)
        return;
    float NoV       = max((float(id.x) + 0.5) / float(pc.size), 1e-4);
    float roughness = (float(id.y) + 0.5) / float(pc.size);
    float alpha     = roughness * roughness;
    float3 n        = float3(0.0, 0.0, 1.0);
    float3 v        = float3(sqrt(1.0 - NoV * NoV), 0.0, NoV);
    float2 sum      = float2(0.0);
    for (uint i = 0; i < BRDF_SAMPLES; ++i)
    {
        float3 h    = sampleGgx(getHammersley(i, BRDF_SAMPLES), alpha, n);
        float VoH   = saturate(dot(v, h));
        float3 l    = 2.0 * VoH * h - v;
        float NoL   = saturate(l.z);
        float NoH   = saturate(h.z);
        if (NoL <= 0.0)
            continue;
        float g     = V_SmithGGXCorrelated(NoV, NoL, alpha) * 4.0 * NoL * VoH / max(NoH, 1e-6);
        float fc    = pow(1.0 - VoH, 5.0);
        sum        += float2((1.0 - fc) * g, fc * g);
    }
    gStorageImages[pc.target][uint3(id.xy, 0)] = float4(sum / float(BRDF_SAMPLES), 0.0, 1.0);
}

// clang-format on
