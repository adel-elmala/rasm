// clang-format off
#pragma once

// A count as a heat color, one step per doubling: 1 dark blue, 2 blue, 4 cyan, 8 green, 16 yellow, 32 red, 64 and
// more white; 0 is black. The colors are display values (sRGB-encoded). Must match HEAT_COLORS in
// src/renderer/engineUi.cpp, the views' keys.
static const float3 HEAT_COLORS[7] = {float3(0.05, 0.05, 0.35), float3(0.10, 0.30, 0.95), float3(0.05, 0.80, 0.90), float3(0.15, 0.85, 0.20),
                                      float3(0.98, 0.88, 0.10), float3(0.95, 0.15, 0.10), float3(1.0, 1.0, 1.0)};

float3 getHeatColor(float count)
{
    if (count < 1.0)
        return float3(0.0);
    float t     = min(log2(count), 6.0);
    uint step   = min(uint(t), 5u);
    return lerp(HEAT_COLORS[step], HEAT_COLORS[step + 1], t - float(step));
}
// clang-format on
