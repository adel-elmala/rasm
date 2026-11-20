// clang-format off
#pragma once

#include "core.slang"

// How much of the light reaches worldPosition in one cascade: 1 lit, 0 in shadow, -1 outside the map. 3x3 PCF with
// bilinear weights, so 4x4 texels, read as four 2x2 gathers. The map was drawn with reversed-Z, so a texel nearer the
// light holds a larger depth. A cascade's camera is orthographic: its matrix's rows 0 and 2 scale world units to NDC x
// and depth, which turns one texel into world units and depth.
float sampleCascade(Sampler2D shadowMap, float4x4 shadowMatrix, float3 worldPosition, float3 normal, float NoL)
{
    uint width, height;
    shadowMap.GetDimensions(width, height);

    float texelWorld        = 2.0 / (length(shadowMatrix[0].xyz) * float(width));
    // Push the lookup off the surface along its normal, more where the light grazes it, so a surface does not shadow itself.
    float3 position         = worldPosition + normal * texelWorld * (1.0 + 2.0 * (1.0 - NoL));
    float4 clip             = mul(shadowMatrix, float4(position, 1.0));
    float3 ndc              = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
        return -1.0;
    float depthBias         = texelWorld * length(shadowMatrix[2].xyz);

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

// How much of the shadow light (SceneData::shadowLight) reaches worldPosition: 1 lit, 0 in shadow. The first cascade
// whose split is beyond the fragment's view depth holds it; past the last split it is lit, faded over the last tenth
// of the distance so the shadow does not end in a line.
float sampleShadow(Sampler2D gTextures[], SceneData *scene, float3 worldPosition, float3 normal)
{
    int lightIndex          = scene->shadowLight;
    uint count              = scene->shadowCascadeCount;
    if (count == 0 || lightIndex < 0)
        return 1.0;

    float3 toLight          = normalize(-scene->lights[lightIndex].direction);
    float NoL               = saturate(dot(normal, toLight));
    float viewDepth         = -mul(scene->view, float4(worldPosition, 1.0)).z;
    float distance          = scene->shadowSplits[count - 1];
    // Unrolled, so each cascade's texture index is a constant element of the scene data, the same for every pixel of
    // the draw, even where neighbouring pixels pick different cascades.
    [unroll]
    for (uint i = 0; i < MAX_SHADOW_CASCADES; ++i)
    {
        if (i >= count || viewDepth > scene->shadowSplits[i])
            continue;
        float lit           = sampleCascade(gTextures[scene->shadowTextures[i]], scene->shadowMatrices[i], worldPosition, normal, NoL);
        // The normal offset can push the lookup off a cascade's edge; the next cascade is bigger and holds it.
        if (lit < 0.0)
            continue;
        return lerp(lit, 1.0, saturate((viewDepth - 0.9 * distance) / (0.1 * distance)));
    }
    return 1.0;
}
// clang-format on
