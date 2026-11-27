// clang-format off
// Every view's cull and point raster run this one pipeline, an export's too: it reads the view's pixel scale.
#define RASM_PIXEL_SCALE 1
#include "core.slang"
#include "points.slang"

// The compute path for small points (Schuetz, Kerbl and Wimmer 2021, "Rendering Point Clouds with Compute Shaders"). The
// cull pass sends each chunk whose points are small enough (isRasterChunk: POINT_RASTER_MAX_SIZE at the chunk's centre
// and POINT_RASTER_MAX_NEAR at its nearest point in a perspective view, POINT_RASTER_MAX_ORTHO in an orthographic one,
// POINT_PIXELS_MAX_SIZE under MSAA) here instead of to the point run's triangles (appendPointBatches): a batch of up
// to POINT_BATCH_POINTS of its points for each group of rasterMain. A thread projects a point as the triangle path's
// vertex stage does and writes the pixels whose centre its shape holds with a 64-bit atomic max of (depth << 32 |
// instance row << 16 | point): in reversed-Z the nearest point wins, and on a tie the later one, as the triangles'
// GREATER_OR_EQUAL test. resolveFrag then writes each
// pixel's point into the scene's color, motion and depth, once per pixel, into all of its samples.
//
// Per frame and view (gpuScene.cpp, pointRaster.cpp): resetMain before the first cull, the culls add batches, then
// after the last cull clearMain and rasterMain (indirect: no group without a batch), and the resolve at the end of the
// last draw pass, before the sky, over the rect of the pixels those chunks reach (none without a batch).

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

static const uint   POINT_RASTER_GROUP_SIZE = 256;

[shader("compute")]
[numthreads(1, 1, 1)]
void resetMain()
{
    PointRasterHeader *header       = pc.scene->pointRasterPtr;
    header->rasterGroups            = 0;
    header->rasterY                 = 1;
    header->rasterZ                 = 1;
    header->clearGroups             = 0;
    header->clearY                  = 1;
    header->clearZ                  = 1;
    header->rectMinX                = 0xFFFFFFFF;
    header->rectMinY                = 0xFFFFFFFF;
    header->rectMaxX                = 0;
    header->rectMaxY                = 0;
}

// The rect's words to 0: no point, as depth 0 is the far plane at infinity. Each thread a pixel of the rect or more;
// the pixels outside it are left as they are, as nothing reads them.
[shader("compute")]
[numthreads(POINT_CLEAR_GROUP_SIZE, 1, 1)]
void clearMain(uint3 threadId : SV_DispatchThreadID)
{
    SceneData *scene    = pc.scene;
    PointRasterHeader *header = scene->pointRasterPtr;
    uint2 low           = uint2(header->rectMinX, header->rectMinY);
    uint width          = header->rectMaxX - low.x + 1;
    uint pixels         = width * (header->rectMaxY - low.y + 1);
    uint stride         = header->clearGroups * POINT_CLEAR_GROUP_SIZE;
    for (uint i = threadId.x; i < pixels; i += stride)
        scene->pointPixelsPtr[(low.y + i / width) * scene->viewportWidth + low.x + i % width] = 0;
}

// The pixels of a point's shape (getPointShape), as the triangle path's fragment test takes them: those whose centre
// is within half its diameter of the point's centre, edges included, per axis for a square and by distance for a round
// or SPHERE point, whose depth then follows its surface. The centre is snapped to the rasterizer's 1/256 pixel grid,
// where the triangle's corners land, so both paths take the same pixels; a SPHERE point's centre is its front's, as
// its triangle's. A pixel whose centre lies on the shape's edge is taken as the triangle's rasterizer takes it: on the
// left and the right edge, and on the top but not the bottom one (the top-left rule, the triangle's bottom side being
// the square's), so a point of d pixels takes at most ceil(d) + 1 columns and ceil(d) rows.
void rasterPoint(SceneData *scene, float4 viewPosition, float diameter, float shape, uint id, uint thinned)
{
    float4 clip         = mul(scene->projection, viewPosition);
    float pixelWorld    = 2.0 * max(abs(clip.w), 1e-6) / (abs(scene->projection[1][1]) * float(scene->viewportHeight));
    float radius        = 0.5 * diameter * pixelWorld;
    if (shape == POINT_SHAPE_SPHERE)
        clip            = mul(scene->projection, viewPosition + float4(0.0, 0.0, radius, 0.0));
    if (clip.w <= 0.0)
        return;
    float depth         = clip.z / clip.w;
    // Past the near plane, or at the far plane's infinity, whose depth 0 is "no point".
    if (!(depth > 0.0 && depth <= 1.0))
        return;
    float2 size         = float2(scene->viewportWidth, scene->viewportHeight);
    float2 ndc          = clip.xy / clip.w;
    float2 center       = round(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * size * 256.0) / 256.0;
    float reach         = 0.5 * diameter;
    float2 low          = max(ceil(center - reach - 0.5), 0.0);
    float2 high         = min(min(floor(center + reach - 0.5), low + float2(ceil(diameter), ceil(diameter) - 1.0)), size - 1.0);
    bool square         = shape == POINT_SHAPE_SQUARE || shape == POINT_SHAPE_PIXELS;
    // The depth's lowest bit says whether the point's chunk was thinned by the LOD, for the resolve's hole fill: a
    // change of one unit in the last place, far below any depth two points can be told apart by.
    uint low32          = (asuint(depth) & ~1u) | thinned;
    for (float y = low.y; y <= high.y; y += 1.0)
    {
        for (float x = low.x; x <= high.x; x += 1.0)
        {
            uint bits       = low32;
            if (!square)
            {
                float2 corner   = (float2(x, y) + 0.5 - center) / reach;
                float distance2 = dot(corner, corner);
                if (distance2 > 1.0)
                    continue;
                if (shape == POINT_SHAPE_SPHERE)
                {
                    // Past a finite far plane the rim's depth is below 0, which as bits would beat every point.
                    float rim   = getSphereDepth(scene->projection, -viewPosition.z, radius, distance2);
                    if (!(rim > 0.0))
                        continue;
                    bits        = (asuint(rim) & ~1u) | thinned;
                }
            }
            // No read first to skip a losing point's atomic: the atomic returns nothing, so the thread does not wait
            // for it, and a read that it waits on cost more (measured, RTX 3060).
            InterlockedMax(scene->pointPixelsPtr[uint(y) * scene->viewportWidth + uint(x)], uint64_t(bits) << 32 | uint64_t(id));
        }
    }
}

// A group a batch. Its points are one chunk's, in one instance, so the chunk's header, style and matrix are read once;
// each point as points.slang's vertMain places it, to the same clip position.
[shader("compute")]
[numthreads(POINT_RASTER_GROUP_SIZE, 1, 1)]
void rasterMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    SceneData *scene        = pc.scene;
    uint2 batch             = scene->pointBatchesPtr[groupId.x];
    uint row                = batch.x;
    uint first              = batch.y & 0xFFFF;
    uint count              = (batch.y >> 16) & 0x7FFF;
    uint thinned            = (batch.y & POINT_BATCH_THINNED) != 0 ? 1 : 0;
    InstanceData *instance  = scene->instancesPtr + row;
    uint base               = uint(scene->geometryPtr[instance->geometryIndex].vertexOffset);
    PointData style         = (scene->materialsPtr + instance->materialIndex)->points;
    PointChunk chunk        = loadPointChunk(scene, base);
    float4x4 world          = getWorldMatrix(scene, instance);
    bool adaptive           = (style.flags & POINT_ADAPTIVE) != 0;
    float spacing           = adaptive ? getWorldSpacing(chunk.spacing, world) : 0.0;
    // Read once: the atomics below keep the compiler from moving loads through the instance out of the loop.
    int lod                 = instance->meshletVisibility;
    for (uint i = threadId.x; i < count; i += POINT_RASTER_GROUP_SIZE)
    {
        uint point          = first + i;
        uint2 packed        = scene->pointsPtr[base + POINT_CHUNK_HEADER_UNITS + point];
        // A point the triangles would collapse (a clear color, a scalar out of the range, a clear colormap entry)
        // writes nothing, so the point behind it wins, as by triangles. The batch's style is the group's own.
        float colormapAt    = getPointColormapPosition(style, chunk, packed.y >> 16);
        if (style.color.a <= 0.0 || colormapAt == POINT_OUT_OF_RANGE ||
            (colormapAt >= 0.0 && gTextures[style.colormap].SampleLevel(getColormapCoordinate(colormapAt), 0.0).a <= 0.0))
            continue;
        float3 local        = chunk.boxMin + float3(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF) * chunk.boxStep;
        float3 position     = mul(world, float4(local, 1.0)).xyz;
        float4 viewPosition = mul(scene->view, float4(position, 1.0));
        float w             = mul(scene->projection, viewPosition).w;
        // The fixed size is its own call, which compiles to the size alone.
        float diameter      = adaptive ? getPointDiameter(scene, style, spacing, getPointFinerShare(lod, local), w) : getPointDiameter(scene, style, 0.0, 0.0, w);
        rasterPoint(scene, viewPosition, diameter, getPointShape(style, diameter), row << 16 | point, thinned);
    }
}

struct ResolveVertex
{
    float4 position     : SV_POSITION;
};

// Two triangles over the rect, or none (all corners at one point) while no chunk took the compute path.
[shader("vertex")]
ResolveVertex resolveVert(uint vertexID : SV_VertexID)
{
    SceneData *scene    = pc.scene;
    PointRasterHeader *header = scene->pointRasterPtr;
    float2 low          = float2(header->rectMinX, header->rectMinY);
    float2 high         = float2(header->rectMaxX, header->rectMaxY) + 1.0;
    // The quad's corners 0, 1, 2 and 2, 1, 3, in pixels, then to clip space (y up).
    uint corner         = vertexID < 3 ? vertexID : (vertexID == 3 ? 2 : (vertexID == 4 ? 1 : 3));
    float2 pixel        = float2((corner & 1) != 0 ? high.x : low.x, (corner & 2) != 0 ? high.y : low.y);
    if (header->rectMinX > header->rectMaxX)
        pixel           = float2(0.0);
    float2 size         = float2(scene->viewportWidth, scene->viewportHeight);
    ResolveVertex output;
    output.position     = float4(pixel.x / size.x * 2.0 - 1.0, 1.0 - pixel.y / size.y * 2.0, 0.0, 1.0);
    return output;
}

struct ResolveOutput
{
    float4 color        : SV_TARGET0;
#if defined(RASM_MOTION_VECTORS)
    float4 motion       : SV_TARGET1;
#endif
    float depth         : SV_Depth;
};

// A pixel that no point reached, inside a surface of points: the LOD keeps about as many points as the pixels they
// cover, so a few pixels of a surface stay empty and would show what is behind as single dots. It takes the nearest of
// its 8 neighbours' points when at least POINT_FILL_MIN_NEIGHBOURS of them hold one, their depths are within
// POINT_FILL_DEPTH_TOLERANCE of each other (one surface around it), and that point's chunk was thinned by the LOD (the
// depth's lowest bit): a chunk that draws all its points shows the data's own gaps, as the triangles do. A pixel just
// past a silhouette has at most 5 such neighbours (3 along a straight edge), and one between two surfaces fails the
// depth test, so neither grows. Words outside the rect were not cleared this frame and count as empty.
static const uint   POINT_FILL_MIN_NEIGHBOURS   = 6;
static const float  POINT_FILL_DEPTH_TOLERANCE  = 0.05;

uint64_t getHoleFill(SceneData *scene, uint2 pixel)
{
    PointRasterHeader *header = scene->pointRasterPtr;
    int2 low                = int2(header->rectMinX, header->rectMinY);
    int2 high               = int2(header->rectMaxX, header->rectMaxY);
    uint64_t nearest        = 0;
    float farthest          = 2.0;
    uint filled             = 0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 neighbour  = int2(pixel) + int2(dx, dy);
            if ((dx == 0 && dy == 0) || any(neighbour < low) || any(neighbour > high))
                continue;
            uint64_t word   = scene->pointPixelsPtr[uint(neighbour.y) * scene->viewportWidth + uint(neighbour.x)];
            uint depthBits  = uint(word >> 32);
            if (depthBits == 0)
                continue;
            ++filled;
            nearest         = max(nearest, word);
            farthest        = min(farthest, asfloat(depthBits));
        }
    }
    float depth             = asfloat(uint(nearest >> 32));
    bool thinned            = (uint(nearest >> 32) & 1) != 0;
    return thinned && filled >= POINT_FILL_MIN_NEIGHBOURS && farthest >= depth * (1.0 - POINT_FILL_DEPTH_TOLERANCE) ? nearest : 0;
}

// The pixel's point, with its color and the motion of its centre as the triangle path gives them, at the depth the
// raster pass kept (an empty pixel inside a surface takes a neighbour's: getHoleFill). The depth test against the
// opaque draws decides per sample, so a mesh in front still covers it.
[shader("fragment")]
ResolveOutput resolveFrag(ResolveVertex input)
{
    SceneData *scene        = pc.scene;
    PointRasterHeader *header = scene->pointRasterPtr;
    uint2 pixel             = uint2(input.position.xy);
    // A helper lane of a quad on the rect's edge can lie outside it, where the words were not cleared this frame (and
    // past the last row, outside the buffer): it reads no word.
    bool inside             = all(pixel >= uint2(header->rectMinX, header->rectMinY)) && all(pixel <= uint2(header->rectMaxX, header->rectMaxY));
    uint64_t word           = inside ? scene->pointPixelsPtr[pixel.y * scene->viewportWidth + pixel.x] : 0;
    if (inside && (word >> 32) == 0)
        word                = getHoleFill(scene, pixel);
    uint depthBits          = uint(word >> 32);
    // discard demotes the lane to a helper, which runs on: without the return it would read the point of instance row
    // 0 (id 0), which may be no point chunk, and follow its words out of the point buffer (B39).
    if (depthBits == 0)
    {
        discard;
        return (ResolveOutput)0;
    }
    uint id                 = uint(word);
    InstanceData *instance  = scene->instancesPtr + (id >> 16);
    uint point              = id & 0xFFFF;
    uint base               = uint(scene->geometryPtr[instance->geometryIndex].vertexOffset);
    PointData style         = (scene->materialsPtr + instance->materialIndex)->points;
    PointChunk chunk        = loadPointChunk(scene, base);
    uint2 packed            = scene->pointsPtr[base + POINT_CHUNK_HEADER_UNITS + point];
    float colormapAt        = getPointColormapPosition(style, chunk, packed.y >> 16);
    float4 color            = getPointBaseColor(scene, style, chunk, base, point);
    if (colormapAt >= 0.0)
        color *= gTextures[NonUniformResourceIndex(style.colormap)].SampleLevel(getColormapCoordinate(colormapAt), 0.0);
    if (colormapAt == POINT_OUT_OF_RANGE)
        color = float4(0.0);
    if (color.a <= 0.0)
        discard;
    ResolveOutput output;
    output.color            = color;
#if defined(RASM_MOTION_VECTORS)
    float3 local            = chunk.boxMin + float3(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF) * chunk.boxStep;
    float3 position         = mul(getWorldMatrix(scene, instance), float4(local, 1.0)).xyz;
    float4 currentClip      = removeJitter(scene, mul(scene->projection, mul(scene->view, float4(position, 1.0))));
    float4 previousClip     = mul(scene->previousViewProjection, float4(getPreviousWorldPosition(scene, instance, local), 1.0));
    output.motion           = float4(getMotion(currentClip, previousClip), 0.0, 0.0);
#endif
    output.depth            = asfloat(depthBits & ~1u);
    return output;
}
// clang-format on
