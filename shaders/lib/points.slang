// clang-format off
#pragma once

#include "core.slang"

// Point cloud chunks (passes/points.slang, rasm/pointCloud.h). A chunk lives in the point pool (SceneData::pointsPtr,
// 8 B units) at its GeometryData::vertexOffset: a header of POINT_CHUNK_HEADER_UNITS, a uint2 per point, then the
// colors, a word each. Its draw's indices are the shared 0, 1, 2, ...: point p is the triangle 3p, 3p + 1, 3p + 2
// (POINT_INDICES), so drawing the first k points draws a prefix of the chunk's order. A triangle a point, not a quad of
// two: 12-16% faster on an RTX 3060, as the cost is per point (vertices and setup), not per pixel.

// PointData::flags. Must match rasm::gfx::POINT_* in uberMaterialTypes.h.
static const uint   POINT_METRES            = 1 << 0;
static const uint   POINT_ROUND             = 1 << 1;
static const uint   POINT_SPHERE            = 1 << 2;
static const uint   POINT_SCALAR            = 1 << 3;
static const uint   POINT_LOG               = 1 << 4;
static const uint   POINT_CLAMP             = 1 << 5;
// PointChunkHeader::flags, and the header's size. Must match rasm::gfx::POINT_CHUNK_*.
static const uint   POINT_CHUNK_COLORS      = 1 << 0;
static const uint   POINT_CHUNK_SCALARS     = 1 << 1;
static const uint   POINT_CHUNK_HEADER_UNITS = 6;
static const uint   POINT_INDICES           = 3;        // a triangle a point
static const float  POINT_STEP_SCALE        = 1.0 / 65535.0;

// rasm::gfx::PointChunkHeader, read as three 16-byte pairs of units.
struct PointChunk
{
    float3          boxMin;
    uint            pointCount;
    float3          boxStep;
    uint            flags;
    float           scalarMin;
    float           scalarStep;
    uint            colorOffset;        // in words from the header
};

PointChunk loadPointChunk(SceneData *scene, uint base)
{
    uint2 u0            = scene->pointsPtr[base];
    uint2 u1            = scene->pointsPtr[base + 1];
    uint2 u2            = scene->pointsPtr[base + 2];
    uint2 u3            = scene->pointsPtr[base + 3];
    uint2 u4            = scene->pointsPtr[base + 4];
    PointChunk chunk;
    chunk.boxMin        = asfloat(uint3(u0.x, u0.y, u1.x));
    chunk.pointCount    = u1.y;
    chunk.boxStep       = asfloat(uint3(u2.x, u2.y, u3.x));
    chunk.flags         = u3.y;
    chunk.scalarMin     = asfloat(u4.x);
    chunk.scalarStep    = asfloat(u4.y);
    chunk.colorOffset   = scene->pointsPtr[base + 5].x;
    return chunk;
}

// How far a point's triangle reaches past the chunk's sphere: half its size and a pixel, in this view's pixels at the
// sphere's far side (as getVectorPadding), so culling keeps a chunk whose points reach into the view.
float getPointPadding(SceneData *scene, PointData style, float3 center, float radius)
{
    bool orthographic   = dot(scene->lodDepthPlane, scene->lodDepthPlane) == 0.0;
    float depth         = orthographic ? 1.0 : max(dot(scene->lodDepthPlane, float4(center, 1.0)) + radius, 0.0);
    float pixelWorld    = depth / max(scene->lodPixelsPerUnit, 1e-6);
    bool metres         = (style.flags & POINT_METRES) != 0;
    return (metres ? 0.5 * style.size : 0.0) + ((metres ? 0.0 : 0.5 * style.size) + 1.0) * pixelWorld;
}

// LOD by prefix: the first points of the chunk's order are an even sample of it, so the chunk draws only as many as
// cover its square on screen about density times: density * diameter^2 / area, area the point's square, or pi / 4 of
// it for a round point. This is the one place that decides what a chunk draws. Measured at the sphere's point nearest
// the camera, as selectLod does, so a camera inside the sphere draws them all; a size in metres cancels the distance. A
// forced LOD k (F2) draws total >> k; lodErrorPixels 0 or density 0 draws all.
uint selectPointCount(SceneData *scene, PointData style, uint total, float3 center, float radius)
{
    if (scene->forcedLod >= 0)
        return max(total >> min(uint(scene->forcedLod), 16u), 1u);
    if (scene->lodErrorPixels <= 0.0 || style.density <= 0.0)
        return total;
    float pixelsPerUnit = scene->lodPixelsPerUnit;
    if (any(scene->lodDepthPlane != float4(0.0)))
    {
        float depth     = dot(scene->lodDepthPlane, float4(center, 1.0)) - radius;
        if (depth <= 0.0)
            return total;
        pixelsPerUnit  /= depth;
    }
    float diameter      = 2.0 * radius * pixelsPerUnit;
    float size          = max((style.flags & POINT_METRES) != 0 ? style.size * pixelsPerUnit : style.size, 1.0);
    // A round point covers pi / 4 of its square: as many more points cover the same ground.
    float area          = (style.flags & (POINT_ROUND | POINT_SPHERE)) != 0 ? 0.785398 * size * size : size * size;
    float wanted        = ceil(style.density * diameter * diameter / area);
    return uint(clamp(wanted, 1.0, float(total)));
}
// clang-format on
