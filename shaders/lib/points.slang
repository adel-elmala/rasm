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
static const uint   POINT_ADAPTIVE          = 1 << 6;
static const uint   POINT_MAX_SIZE_SHIFT    = 16;       // flags' high 16 bits: an adaptive point's largest diameter, in 1/16 pixels
static const float  POINT_ADAPTIVE_COVER    = 4.0;      // times an adaptive point's chunk covers its ground (getPointDiameter)
// PointChunkHeader::flags, and the header's size. Must match rasm::gfx::POINT_CHUNK_*.
static const uint   POINT_CHUNK_COLORS      = 1 << 0;
static const uint   POINT_CHUNK_SCALARS     = 1 << 1;
static const uint   POINT_CHUNK_HEADER_UNITS = 6;
static const uint   POINT_INDICES           = 3;        // a triangle a point
static const float  POINT_STEP_SCALE        = 1.0 / 65535.0;
// The compute rasterizer (passes/pointRaster.slang). Must match rasm::gfx::POINT_* in uberMaterialTypes.h.
static const float  POINT_RASTER_MAX_SIZE   = 8.0;      // a chunk whose points are this many pixels across or less goes to it (POINT_PIXELS_MAX_SIZE under MSAA)
static const float  POINT_RASTER_MIN_DENSITY = 0.125;    // and that draws this many points of 2 pixels per pixel of its rect on screen
static const uint   POINT_BATCH_POINTS      = 1024;     // a group of the raster dispatch
static const uint   POINT_RASTER_MAX_ROWS   = 1 << 16;  // a pixel's word keeps the instance row in 16 bits
static const uint   POINT_CLEAR_GROUP_SIZE  = 256;
static const uint   POINT_BATCH_THINNED     = 1u << 31; // a batch's second word: its chunk draws a prefix, not all its points
static const uint   POINT_MAX_CLEAR_GROUPS  = 65535;

// rasm::gfx::PointChunkHeader, read as words: boxMin 0, pointCount 12, boxStep 16, flags 28, scalarMin 32, scalarStep 36,
// colorOffset 40, spacing 44 (bytes), which uberMaterialTypes.h asserts.
struct PointChunk
{
    float3          boxMin;
    uint            pointCount;
    float3          boxStep;
    uint            flags;
    float           scalarMin;
    float           scalarStep;
    uint            colorOffset;        // in words from the header
    float           spacing;            // between neighbouring points, in its entity's units; 0: unknown
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
    chunk.spacing       = asfloat(scene->pointsPtr[base + 5].y);
    return chunk;
}

float getPointMaxSize(PointData style)
{
    return float(style.flags >> POINT_MAX_SIZE_SHIFT) / 16.0;
}

float loadPointSpacing(SceneData *scene, uint base)
{
    return asfloat(scene->pointsPtr[base + 5].y);
}

// A chunk's spacing is in its entity's units: in the world it is that times the entity's scale (its matrix's first
// column), which getPointDiameter's pixel size is in.
float getWorldSpacing(float spacing, float4x4 world)
{
    return spacing * length(mul(world, float4(1.0, 0.0, 0.0, 0.0)).xyz);
}

// A chunk's LOD (rasm::PointChunkLod, Renderer::setPointChunkLod), in its instance's meshletVisibility word: -1 when it
// was never set (all its points, no finer chunk), else bits 24 to 30 the share of its points drawn (of 127) and bits 3o
// to 3o + 2 the drawn share (of 7) of the finer chunk in octant o of its entity's space, o = (x >= 0) | (y >= 0) << 1 |
// (z >= 0) << 2. Must match encodePointChunkLod in src/scene/entity.cpp.
float getPointDrawnShare(int lod)
{
    return lod < 0 ? 1.0 : float((uint(lod) >> 24) & 0x7F) / 127.0;
}

float getPointFinerShare(int lod, float3 local)
{
    if (lod < 0)
        return 0.0;
    uint octant     = (local.x >= 0.0 ? 1u : 0u) | (local.y >= 0.0 ? 2u : 0u) | (local.z >= 0.0 ? 4u : 0u);
    return float((uint(lod) >> (3 * octant)) & 7u) / 7.0;
}

// A point's diameter in pixels at clip w: its size in pixels, or in metres through a pixel's size in the world at its
// depth; at least a pixel, so it neither vanishes nor flickers. POINT_ADAPTIVE grows it, up to maxSize, once the chunk's
// points no longer cover its ground density times at that size (the rule the prefix LOD keeps, selectPointCount): from
// where its spacing on screen times sqrt(density / area) passes the size (area 1 for a square, pi / 4 for a round
// point), and sqrt(POINT_ADAPTIVE_COVER / density) times faster than that, so near the camera it covers the ground about
// 4 times. A random sample, whose points fall where they will, leaves under 1% of its ground open at 4 (e^-4.6), and 5
// to 10% at 2. Points that already cover do not grow, so the far ones keep their size and the compute path. finer, the
// drawn share of a finer chunk over the point (getPointFinerShare), takes it back to the size: as that chunk fades in,
// its points cover its share and these the square of what is left of their growth, which together never cover less.
// The triangle and the compute path both size a point here, and the cull pass a chunk, with finer 0 (its largest).
float getPointDiameter(SceneData *scene, PointData style, float spacing, float finer, float w)
{
    float pixelWorld    = 2.0 * max(abs(w), 1e-6) / (abs(scene->projection[1][1]) * float(scene->viewportHeight));
    bool metres         = (style.flags & POINT_METRES) != 0;
    float size          = max(metres ? style.size / pixelWorld : style.size, 1.0);
    if ((style.flags & POINT_ADAPTIVE) == 0 || !(spacing > 0.0))
        return size;
    float area          = (style.flags & (POINT_ROUND | POINT_SPHERE)) != 0 ? 0.785398 : 1.0;
    float density       = max(style.density, 1.0);
    float covering      = spacing / pixelWorld * sqrt(density / area);
    float faster        = sqrt(max(POINT_ADAPTIVE_COVER / density, 1.0));
    float grown         = clamp(size + faster * max(covering - size, 0.0), size, max(getPointMaxSize(style), size));
    return lerp(grown, size, saturate(finer));
}

// The shape a point of this diameter is cut to, on both paths: POINT_SHAPE_PIXELS (whole pixels, all their samples) up
// to POINT_PIXELS_MAX_SIZE, a square under 3 pixels (a disc that small misses the pixel centres it covers), else its
// style's.
static const float  POINT_SHAPE_SQUARE      = 0.0;
static const float  POINT_SHAPE_ROUND       = 1.0;
static const float  POINT_SHAPE_SPHERE      = 2.0;
static const float  POINT_SHAPE_PIXELS      = 3.0;
static const float  POINT_PIXELS_MAX_SIZE   = 2.0;

float getPointShape(PointData style, float diameter)
{
    if (diameter <= POINT_PIXELS_MAX_SIZE)
        return POINT_SHAPE_PIXELS;
    if (diameter < 3.0)
        return POINT_SHAPE_SQUARE;
    return (style.flags & POINT_SPHERE) != 0 ? POINT_SHAPE_SPHERE : ((style.flags & POINT_ROUND) != 0 ? POINT_SHAPE_ROUND : POINT_SHAPE_SQUARE);
}

// A SPHERE point's depth where the pixel is distance2 (0 to 1, squared) from its centre across its disc: its surface's
// view depth there, back through the projection: (P22 (-z) + P23) / (P32 (-z) + P33), for a perspective and an
// orthographic projection alike. centre is the sphere's centre's distance in front of the camera, radius its radius.
float getSphereDepth(float4x4 p, float centre, float radius, float distance2)
{
    float z         = centre - radius * sqrt(saturate(1.0 - distance2));
    return (p[2][2] * -z + p[2][3]) / (p[3][2] * -z + p[3][3]);
}

// Where the point's scalar falls in the colormap, 0 to 1. POINT_NO_COLORMAP when it takes none (POINT_SCALAR off, a
// chunk without scalars, no colormap), POINT_OUT_OF_RANGE when its scalar is outside the range without POINT_CLAMP: it
// draws nothing. The shaders sample the colormap themselves, from their own bindless array.
static const float  POINT_NO_COLORMAP       = -1.0;
static const float  POINT_OUT_OF_RANGE      = -2.0;

float getPointColormapPosition(PointData style, PointChunk chunk, uint scalarStep)
{
    if ((style.flags & POINT_SCALAR) == 0 || (chunk.flags & POINT_CHUNK_SCALARS) == 0 || style.colormap == INVALID_TEXTURE_INDEX)
        return POINT_NO_COLORMAP;
    float value     = chunk.scalarMin + float(scalarStep) * chunk.scalarStep;
    if ((style.flags & POINT_LOG) != 0)
        value       = value > 0.0 ? log(value) : -1e30;
    float span      = style.rangeMax - style.rangeMin;
    float t         = span != 0.0 ? (value - style.rangeMin) / span : 0.0;
    if ((style.flags & POINT_CLAMP) == 0 && !(t >= 0.0 && t <= 1.0))
        return POINT_OUT_OF_RANGE;
    return saturate(t);
}

// The colormap texel of position t (getPointColormapPosition): its 256 entries' centres.
float2 getColormapCoordinate(float t)
{
    return float2((t * 255.0 + 0.5) / 256.0, 0.5);
}

// The point's color before its colormap: PointData::color, times the point's own color when it has one and is not
// colored by its scalar.
float4 getPointBaseColor(SceneData *scene, PointData style, PointChunk chunk, uint base, uint point)
{
    if ((style.flags & POINT_SCALAR) != 0 || (chunk.flags & POINT_CHUNK_COLORS) == 0)
        return style.color;
    uint2 pair      = scene->pointsPtr[base + chunk.colorOffset / 2 + point / 2];
    uint packed     = (point & 1) != 0 ? pair.y : pair.x;
    float4 decoded  = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    // Points are opaque: the colors' alpha is not read, so colors packed without one (0) still draw.
    return style.color * float4(decodeSrgb(decoded.r), decodeSrgb(decoded.g), decodeSrgb(decoded.b), 1.0);
}

// How far a point's triangle reaches past the chunk's sphere: half its size and a pixel, in this view's pixels at the
// sphere's far side (as getVectorPadding), so culling keeps a chunk whose points reach into the view.
float getPointPadding(SceneData *scene, PointData style, float3 center, float radius)
{
    bool orthographic   = dot(scene->lodDepthPlane, scene->lodDepthPlane) == 0.0;
    float depth         = orthographic ? 1.0 : max(dot(scene->lodDepthPlane, float4(center, 1.0)) + radius, 0.0);
    float pixelWorld    = depth / max(scene->lodPixelsPerUnit, 1e-6);
    bool metres         = (style.flags & POINT_METRES) != 0;
    float pixels        = metres ? 0.0 : 0.5 * style.size;
    // An adaptive point reaches up to maxSize pixels.
    if ((style.flags & POINT_ADAPTIVE) != 0)
        pixels          = max(pixels, 0.5 * getPointMaxSize(style));
    return (metres ? 0.5 * style.size : 0.0) + (pixels + 1.0) * pixelWorld;
}

// LOD by prefix: the first points of the chunk's order are an even sample of it, so the chunk draws only as many as
// cover its square on screen about density times: density * diameter^2 / area, area the point's square, or pi / 4 of
// it for a round point. This is the one place that decides what a chunk draws. Measured at the sphere's point nearest
// the camera, as selectLod does, so a camera inside the sphere draws them all; a size in metres cancels the distance. A
// forced LOD k (F2) draws total >> k; lodErrorPixels 0 or density 0 draws all. Then drawn, the chunk's drawn share
// (getPointDrawnShare), keeps that share of them, 0 none, so a chunk fades in and out by its prefix.
uint selectPointCount(SceneData *scene, PointData style, uint total, float drawn, float3 center, float radius)
{
    uint count          = selectPointPrefix(scene, style, total, center, radius);
    return drawn >= 1.0 ? count : uint(float(count) * max(drawn, 0.0) + 0.5);
}

uint selectPointPrefix(SceneData *scene, PointData style, uint total, float3 center, float radius)
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

// The split between the two paths, per chunk. The compute path costs a little per point and, once any chunk takes it,
// a clear and a resolve per pixel of the rect its chunks cover; the triangles cost more per point and nothing per pixel:
// ten times at 1 pixel, four at 2 to 4, three at 6 and 8, the same at 12, and less past it, where a thread writing a
// point's pixels one after another is slower than the rasterizer (RTX 3060, a view of one sample, points covering
// their ground 4 to 8 times). So a chunk takes it when its points are POINT_RASTER_MAX_SIZE pixels or less, two thirds
// of that break-even, in every shape, as the compute path cuts each shape to the same pixels as the triangles
// (getPointShape). Under MSAA, where the triangles' edges cover samples by their distance, only the points that cover
// whole pixels on both paths take it, up to POINT_PIXELS_MAX_SIZE. The size is measured at the chunk's sphere's point
// nearest the camera, its largest, in this view's own projection (not the LOD camera's, which a frozen view keeps).
// The chunk must also draw POINT_RASTER_MIN_DENSITY points per pixel of its rect on screen, a point of d pixels
// counting as (d / 2)^2 of 2 pixels, which cover as many pixels and save as much more over a triangle. A chunk the
// camera is inside of, or one partly behind it, goes to the triangles. rect is the pixels its points can reach: its
// box's corners on screen, and a point's reach past them, a whole diameter (a SPHERE point's front moves its centre).
bool isRasterChunk(SceneData *scene, PointData style, float spacing, float3 center, float radius, uint count, out uint4 rect)
{
    rect                = uint4(0);
    float4x4 viewProjection = mul(scene->projection, scene->view);
    float w             = mul(viewProjection, float4(center, 1.0)).w;
    if (scene->projection[3][3] == 0.0)
        w              -= radius;
    float diameter      = w > 0.0 ? getPointDiameter(scene, style, spacing, 0.0, w) : 0.0;
    if (w <= 0.0 || diameter > (scene->depthSamples > 1 ? POINT_PIXELS_MAX_SIZE : POINT_RASTER_MAX_SIZE))
        return false;
    float2 size         = float2(scene->viewportWidth, scene->viewportHeight);
    float2 low          = size;
    float2 high         = float2(0.0);
    for (uint corner = 0; corner < 8; ++corner)
    {
        float3 offset   = float3((corner & 1) != 0 ? radius : -radius, (corner & 2) != 0 ? radius : -radius, (corner & 4) != 0 ? radius : -radius);
        float4 clip     = mul(viewProjection, float4(center + offset, 1.0));
        if (clip.w <= 0.0)
            return false;
        float2 pixel    = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) * size;
        low             = min(low, pixel);
        high            = max(high, pixel);
    }
    float reach         = max(ceil(diameter), POINT_PIXELS_MAX_SIZE);
    low                 = clamp(floor(low) - reach, 0.0, size - 1.0);
    high                = clamp(ceil(high) + reach, 0.0, size - 1.0);
    rect                = uint4(uint2(low), uint2(high));
    float2 extent       = high - low + 1.0;
    return float(count) * max(0.25 * diameter * diameter, 1.0) >= POINT_RASTER_MIN_DENSITY * extent.x * extent.y;
}

// Adds a chunk's first count points to the view's batches, POINT_BATCH_POINTS a batch, marked POINT_BATCH_THINNED when
// the LOD left some of its points out (count < its points), and grows the rect that the
// clear and the resolve cover by the chunk's (an empty rect, its min past its max, runs neither). The batches are
// taken with a compare-exchange that adds only what fits, so every batch below the final count is written; a chunk
// that does not fit goes to the triangles. The clear runs as many groups as the largest chunk's rect needs, and its
// threads loop over the whole rect.
bool appendPointBatches(SceneData *scene, uint row, uint count, bool thinned, uint4 rect)
{
    PointRasterHeader *header = scene->pointRasterPtr;
    uint batches        = (count + POINT_BATCH_POINTS - 1) / POINT_BATCH_POINTS;
    uint first          = 0;
    InterlockedAdd(header->rasterGroups, 0u, first);
    for (;;)
    {
        if (first + batches > scene->pointBatchCapacity)
            return false;
        uint seen       = 0;
        InterlockedCompareExchange(header->rasterGroups, first, first + batches, seen);
        if (seen == first)
            break;
        first           = seen;
    }
    for (uint b = 0; b < batches; ++b)
    {
        uint start      = b * POINT_BATCH_POINTS;
        scene->pointBatchesPtr[first + b] = uint2(row, start | min(POINT_BATCH_POINTS, count - start) << 16 | (thinned ? POINT_BATCH_THINNED : 0));
    }
    InterlockedMin(header->rectMinX, rect.x);
    InterlockedMin(header->rectMinY, rect.y);
    InterlockedMax(header->rectMaxX, rect.z);
    InterlockedMax(header->rectMaxY, rect.w);
    uint pixels         = (rect.z - rect.x + 1) * (rect.w - rect.y + 1);
    InterlockedMax(header->clearGroups, min((pixels + POINT_CLEAR_GROUP_SIZE - 1) / POINT_CLEAR_GROUP_SIZE, POINT_MAX_CLEAR_GROUPS));
    return true;
}
// clang-format on
