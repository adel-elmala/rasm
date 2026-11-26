// clang-format off
#pragma once

#include "core.slang"

// What a pick looks through and what a hit is: rasm/picking.h's PICK_SKIP_* and PICK_HIT_*. Shared by the pick
// pipelines of passes/rasm.slang and passes/points.slang.
static const uint   PICK_SKIP_MESHES    = 1 << 0;
static const uint   PICK_SKIP_BLEND     = 1 << 1;
static const uint   PICK_SKIP_TERRAIN   = 1 << 2;
static const uint   PICK_SKIP_VECTORS   = 1 << 3;
static const uint   PICK_SKIP_LABELS    = 1 << 4;
static const uint   PICK_SKIP_OVERLAYS  = 1 << 5;
static const uint   PICK_SKIP_POINTS    = 1 << 6;
static const uint   PICK_SKIP_GLYPHS    = 1 << 7;
static const uint   PICK_HIT_FEATURE    = 1 << 0;
static const uint   PICK_HIT_LABEL      = 1 << 1;
static const uint   PICK_HIT_OVERLAY    = 1 << 2;
static const uint   PICK_HIT_TERRAIN    = 1 << 3;
static const uint   PICK_HIT_BACK_FACE  = 1 << 4;
static const uint   PICK_HIT_POINT      = 1 << 5;
static const uint   PICK_HIT_GLYPH      = 1 << 6;

// A pick's draws carry its PICK_SKIP_* mask in the low byte of the push constant's argument, and its minAlpha in 1/255
// steps above it (picking.cpp).
uint getPickSkip()
{
    return pc.argument & 0xFF;
}

float getPickMinAlpha()
{
    return float((pc.argument >> 8) & 0xFF) / 255.0;
}

// A unit vector in 32 bits: octahedral, 16 bits per axis (picking.cpp's unpackPickNormal).
uint packPickNormal(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    uint2 q = uint2(round(saturate(e * 0.5 + 0.5) * 65535.0));
    return q.x | (q.y << 16);
}
// clang-format on
