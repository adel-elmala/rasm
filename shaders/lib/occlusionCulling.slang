// clang-format off
#pragma once

#include "core.slang"
#include "frustumCulling.slang"

// Hi-Z level 0 is half the pass's size, rounded up, and each level halves the one before, down to 1x1. A texel
// of level L holds the farthest depth (reversed-Z: the smallest) of the 2^(L+1)-pixel square it covers.
// passes/hiz.slang builds it; getHizLevelCount in gpuScene.cpp counts the same levels.
uint2 getHizLevel0Size(SceneData *scene)
{
    return (uint2(scene->viewportWidth, scene->viewportHeight) + 1) / 2;
}

// The sphere is occluded when its nearest point is farther than the farthest depth in the pixels it covers.
// It is bounded by the view-space box around it, whose eight corners give the screen rectangle and the nearest
// depth for perspective and orthographic cameras alike.
bool isOccluded(SceneData *scene, Sphere sphere)
{
    float3 center       = mul(scene->view, float4(sphere.center, 1.0)).xyz;
    float2 ndcMin       = float2(1e30, 1e30);
    float2 ndcMax       = float2(-1e30, -1e30);
    float nearestDepth  = 0.0;
    for (uint i = 0; i < 8; ++i)
    {
        float3 corner   = center + sphere.radius * float3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0, (i & 4) != 0 ? 1.0 : -1.0);
        float4 clip     = mul(scene->projection, float4(corner, 1.0));
        if (clip.w <= 0.0)
            return false;                               // reaches behind the camera
        float3 ndc      = clip.xyz / clip.w;
        ndcMin          = min(ndcMin, ndc.xy);
        ndcMax          = max(ndcMax, ndc.xy);
        nearestDepth    = max(nearestDepth, ndc.z);
    }
    if (nearestDepth >= 1.0)
        return false;                                   // reaches the near plane

    // Clip space is Y-up and pixel rows run top-down. Pixel coordinates are clamped before they become integers.
    float2 viewport     = float2(scene->viewportWidth, scene->viewportHeight);
    float2 pixelMin     = clamp(float2(ndcMin.x * 0.5 + 0.5, 0.5 - ndcMax.y * 0.5) * viewport, 0.0, viewport - 1.0);
    float2 pixelMax     = clamp(float2(ndcMax.x * 0.5 + 0.5, 0.5 - ndcMin.y * 0.5) * viewport, 0.0, viewport - 1.0);

    // The first level where the rectangle covers at most 2x2 texels. The 1x1 top level always qualifies.
    uint2 texelMin      = uint2(pixelMin) >> 1;
    uint2 texelMax      = uint2(pixelMax) >> 1;
    uint2 size          = getHizLevel0Size(scene);
    uint offset         = 0;
    for (uint level = 0; level + 1 < scene->hizLevelCount && (texelMax.x - texelMin.x > 1 || texelMax.y - texelMin.y > 1); ++level)
    {
        offset         += size.x * size.y;
        size            = (size + 1) / 2;
        texelMin      >>= 1;
        texelMax      >>= 1;
    }

    float *texels       = scene->hizPtr + offset;
    float farthest      = min(min(texels[texelMin.y * size.x + texelMin.x], texels[texelMin.y * size.x + texelMax.x]),
                              min(texels[texelMax.y * size.x + texelMin.x], texels[texelMax.y * size.x + texelMax.x]));
    return nearestDepth < farthest;
}
// clang-format on
