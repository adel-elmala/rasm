// clang-format off
#pragma once

#include "brdf.slang"
#include "cubeMap.slang"

// Image-based lighting: an environment's light on a surface, and the sampling the bake filters it with. Reads no engine
// data, so the bake (passes/environment.slang), the sky and the uber shader share it.

// What a view's SceneData says about its scene's environment.
struct EnvironmentLight
{
    uint        specular;       // bindless cube: mip m holds the light seen through roughness m / (specularMips - 1)
    uint        specularMips;
    uint        irradiance;     // bindless cube: the cosine-weighted light from each normal's hemisphere, divided by pi
    uint        brdfLut;        // bindless 2D: the split sum's scale (x) and bias (y) of f0, at (n.v, roughness)
    float       intensity;
    float2      rotation;       // cos and sin of the environment's turn around +Y
};

// The environment turned by its rotation (counter-clockwise around +Y, seen from above): a world direction looks up
// the unturned environment at the direction turned back.
float3 toEnvironment(float3 d, float2 rotation)
{
    return float3(d.x * rotation.x - d.z * rotation.y, d.y, d.x * rotation.y + d.z * rotation.x);
}

// Equirectangular image coordinates of a direction: -Z at the image's centre, +X a quarter to its right, +Y at its
// top row. Seen from inside, turning right moves right in the image.
float2 getEquirectUV(float3 d)
{
    float u = 0.5 + atan2(d.x, -d.z) / (2.0 * PI);
    float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
    return float2(u, v);
}

// The split sum (Karis 2013): the prefiltered light in the reflected direction, times the BRDF's scale and bias of f0
// from the LUT. Diffuse reads the irradiance cube at the normal.
float3 getEnvironmentLight(SamplerCube cubes[], Sampler2D textures[], EnvironmentLight environment, float3 n, float3 v, float NoV, float roughness, float3 f0, float3 diffuseColor)
{
    float3 r            = toEnvironment(reflect(-v, n), environment.rotation);
    float lod           = roughness * float(environment.specularMips - 1);
    float3 prefiltered  = cubes[environment.specular].SampleLevel(r, lod).rgb;
    float2 dfg          = textures[environment.brdfLut].SampleLevel(float2(NoV, roughness), 0.0).xy;
    float3 specular     = prefiltered * (f0 * dfg.x + dfg.y);
    float3 diffuse      = cubes[environment.irradiance].SampleLevel(toEnvironment(n, environment.rotation), 0.0).rgb * diffuseColor;
    return (diffuse + specular) * environment.intensity;
}

// Point i of n of the Hammersley set: evenly spread over the unit square, the same every run.
float2 getHammersley(uint i, uint n)
{
    return float2((float(i) + 0.5) / float(n), float(reversebits(i)) * 2.3283064365386963e-10);
}

// Two directions that make an orthonormal frame with n (Duff et al. 2017).
void getTangentFrame(float3 n, out float3 t, out float3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// A half vector around n with the GGX distribution of alpha (= roughness squared): its pdf is D * n.h.
float3 sampleGgx(float2 xi, float alpha, float3 n)
{
    float phi       = 2.0 * PI * xi.x;
    float cosTheta  = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta  = sqrt(1.0 - cosTheta * cosTheta);
    float3 t, b;
    getTangentFrame(n, t, b);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + n * cosTheta);
}

// A direction around n with pdf cos(theta) / pi.
float3 sampleCosine(float2 xi, float3 n)
{
    float phi       = 2.0 * PI * xi.x;
    float r         = sqrt(xi.y);
    float3 t, b;
    getTangentFrame(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - xi.y, 0.0)));
}

// Filtered importance sampling (Colbert and Krivanek 2007): a sample of pdf stands for 1 / (count * pdf) of the
// sphere, so it reads the mip whose texels are about that big. One mip coarser (the +1) smooths what the few samples
// leave, so a small bright light such as the sun does not make fireflies.
float getSampleMip(float pdf, uint sampleCount, uint cubeSize, uint mipCount)
{
    float texelSolidAngle   = 4.0 * PI / (6.0 * float(cubeSize) * float(cubeSize));
    float sampleSolidAngle  = 1.0 / (float(sampleCount) * pdf + 1e-6);
    return clamp(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0, float(mipCount - 1));
}

// clang-format on
