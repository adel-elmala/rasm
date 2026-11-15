// clang-format off
#pragma once

#include "core.slang"

// How much of the shadow light (SceneData::shadowLight) reaches worldPosition: 1 lit, 0 in shadow. 3x3 PCF with
// bilinear weights, so 4x4 texels, read as four 2x2 gathers. The map was drawn with reversed-Z, so a texel nearer the
// light holds a larger depth. The offsets assume an orthographic light camera (addScenePasses accepts no other): its
// matrix's rows 0 and 2 then scale world units to NDC x and depth, which turns one texel into world units and depth.
float sampleShadow(Sampler2D gTextures[], SceneData *scene, float3 worldPosition, float3 normal)
{
    int lightIndex          = scene->shadowLight;
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
    float2 size             = float2(width, height);
    float2 texel            = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * size - 0.5;
    float2 base             = floor(texel);
    float2 f                = texel - base;
    // Column (and row) weights of the 4x4 texels: the bilinear weights of 3x3 taps one texel apart, added up.
    float4 wx               = float4(1.0 - f.x, 1.0, 1.0, f.x);
    float4 wy               = float4(1.0 - f.y, 1.0, 1.0, f.y);
    float lit               = 0.0;
    for (int qy = 0; qy < 2; ++qy)
    {
        for (int qx = 0; qx < 2; ++qx)
        {
            // The corner shared by texels base - 1 + 2q and base + 2q: the gather reads those 2x2, clamped at the
            // edges by the depth's sampler. Its order is (x, y + 1), (x + 1, y + 1), (x + 1, y), (x, y).
            float4 occluder = shadowMap.Gather((base + float2(2 * qx, 2 * qy)) / size);
            float4 lit4     = float4(ndc.z + depthBias >= occluder);
            float x0        = wx[2 * qx];
            float x1        = wx[2 * qx + 1];
            float y0        = wy[2 * qy];
            float y1        = wy[2 * qy + 1];
            lit            += dot(lit4, float4(x0 * y1, x1 * y1, x1 * y0, x0 * y0));
        }
    }
    return lit / 9.0;
}
// clang-format on
