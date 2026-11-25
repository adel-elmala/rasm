// clang-format off
#pragma once

// Positions far from the origin are kept as two floats, a high and a low part (splitPosition in
// src/renderer/renderSpace.cpp). The difference of two such positions is taken part by part, the high parts first:
// near each other they are the same floats, so that difference is exact, and the low parts carry the rest.
//
// The order of these sums must survive the shader compilers. Regrouped, as in (aHigh - pixel) - bHigh, a big number
// meets a small one first and the low parts are lost: lighting far from the origin then falls in 0.5 m blocks.
// NoContraction (HLSL's precise) is meant to forbid that, but Slang drops `precise` when it emits SPIR-V, and the
// NVIDIA driver regrouped across the decoration anyway (2026-11-25). An OR with a zero that comes from memory does
// stop every compiler: it cannot know the result, so it cannot regroup through it. It costs an OR per component.

float3 keepOrder(float3 value, uint zero)
{
    return asfloat(asuint(value) | zero);
}

// (aHigh + aLow) - (bHigh + bLow), with the precision of the doubles the parts were split from. zero is 0, read from a
// buffer or push constants (SceneData::zero, AnimatePushConstants::zero).
float3 subtractSplit(float3 aHigh, float3 aLow, float3 bHigh, float3 bLow, uint zero)
{
    float3 high = keepOrder(aHigh - bHigh, zero);
    float3 low  = keepOrder(aLow - bLow, zero);
    return keepOrder(high + low, zero);
}
// clang-format on
