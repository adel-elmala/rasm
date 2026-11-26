// clang-format off
#pragma once

#include "core.slang"

// Weighted blended order-independent transparency (McGuire and Bavoil 2013), the parts the cull and the draws share
// (src/renderer/weightedBlend.cpp). Each weighted fragment adds its premultiplied color and its alpha, times a weight,
// into one target, and its alpha into a coverage, in any order; the composite (passes/weightedComposite.slang) divides
// the sums for the mean color and lays it over what is behind by the coverage.
//
// The weight falls with the fragment's place between the nearest and the farthest weighted draw of the view, so
// nearer surfaces count more in the mean. The place is measured the way the projection measures size: by the ratio of
// distances with perspective (log), by the distance itself orthographic. It never uses metres or the depth buffer's
// value, so a scene at 1 m or 10 km, reversed-Z or not, gets the same weights.

// The weight halves every WEIGHT_HALVINGS-th of the range: 1 at the nearest, 1/32 at the farthest.
static const float  WEIGHT_HALVINGS     = 5.0;

// A view depth as a word whose unsigned order is the float order, also below 0 (an orthographic view's), so the cull
// keeps the range with atomic min and max. The empty range is nearest = 0xFFFFFFFF, farthest = 0.
uint getDepthBits(float depth)
{
    uint bits = asuint(depth);
    return (bits & 0x80000000) != 0 ? ~bits : bits | 0x80000000;
}

float fromDepthBits(uint bits)
{
    return asfloat((bits & 0x80000000) != 0 ? bits & 0x7FFFFFFF : ~bits);
}

bool isOrthographic(SceneData *scene)
{
    return scene->projection[3][3] != 0.0;
}

// The depths the weights span: the CPU's steady range (weightedBlend.cpp), or this frame's from the cull until the CPU
// has one.
float2 getWeightedDepths(SceneData *scene)
{
    float2 depths = scene->weightedDepths;
    if (depths.y <= depths.x && scene->weightedRangePtr != nullptr)
        depths = float2(fromDepthBits(scene->weightedRangePtr[0]), fromDepthBits(scene->weightedRangePtr[1]));
    return depths;
}

// The fragment's place in the range, 0 at the nearest and 1 at the farthest; 0 for an empty range.
float getWeightedPlace(SceneData *scene, float viewDepth)
{
    float2 depths = getWeightedDepths(scene);
    if (!(depths.y > depths.x))
        return 0.0;
    if (isOrthographic(scene))
        return saturate((viewDepth - depths.x) / (depths.y - depths.x));
    float nearest = max(depths.x, 1e-6);
    return saturate(log2(max(viewDepth, nearest) / nearest) / log2(max(depths.y, nearest) / nearest));
}

float getWeight(float place)
{
    return exp2(-WEIGHT_HALVINGS * place);
}

// clang-format on
