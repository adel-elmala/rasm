// clang-format off
#pragma once

#include "core.slang"

// The first directional light is the one the shadow map belongs to (pbr.slang applies it to that light).
int getShadowLight(SceneData *scene)
{
    for (uint i = 0; i < min(scene->numLights, MAX_LIGHTS); ++i)
    {
        if (scene->lights[i].type == LightType.DIRECTIONAL)
            return int(i);
    }
    return -1;
}

// How much of the shadow light reaches worldPosition: 1 lit, 0 in shadow. 3x3 PCF with bilinear weights, so 4x4 texels.
// The map was drawn with reversed-Z, so a texel nearer the light holds a larger depth. The offsets assume an
// orthographic light camera (a directional light): its matrix's rows 0 and 2 then scale world units to NDC x and
// depth, which turns one texel into world units and into depth.
float sampleShadow(Sampler2D gTextures[], SceneData *scene, float3 worldPosition, float3 normal)
{
    int lightIndex          = getShadowLight(scene);
    if (!hasTexture(scene->shadowTexture) || lightIndex < 0)
        return 1.0;

    Sampler2D shadowMap     = gTextures[scene->shadowTexture];
    uint width, height;
    shadowMap.GetDimensions(width, height);

    float3 toLight          = normalize(-scene->lights[lightIndex].direction);
    float NoL               = saturate(dot(normal, toLight));
    float texelWorld        = 2.0 / (length(scene->shadowMatrix[0].xyz) * float(width));
    // Push the lookup off the surface along its normal, more where the light grazes it, so a surface does not shadow itself.
    float3 position         = worldPosition + normal * texelWorld * (1.0 + 2.0 * (1.0 - NoL));
    float4 clip             = mul(scene->shadowMatrix, float4(position, 1.0));
    float3 ndc              = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;
    float depthBias         = texelWorld * length(scene->shadowMatrix[2].xyz);

    // Clip space is Y-up and v = 0 is the map's top row, as in every RASM target.
    float2 texel            = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * float2(width, height) - 0.5;
    int2 base               = int2(floor(texel));
    float2 f                = texel - float2(base);
    int2 last               = int2(width, height) - 1;
    float lit               = 0.0;
    for (int y = 0; y < 4; ++y)
    {
        float wy            = y == 0 ? 1.0 - f.y : (y == 3 ? f.y : 1.0);
        for (int x = 0; x < 4; ++x)
        {
            float wx        = x == 0 ? 1.0 - f.x : (x == 3 ? f.x : 1.0);
            int2 p          = clamp(base + int2(x - 1, y - 1), int2(0), last);
            float occluder  = shadowMap.Load(int3(p, 0)).r;
            lit            += (ndc.z + depthBias >= occluder ? 1.0 : 0.0) * wx * wy;
        }
    }
    return lit / 9.0;
}
// clang-format on
