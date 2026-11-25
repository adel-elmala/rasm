// clang-format off
#include "core.slang"
#include "points.slang"

// The compute path for small points (Schuetz, Kerbl and Wimmer 2021, "Rendering Point Clouds with Compute Shaders"). The
// cull pass sends each chunk whose points are POINT_RASTER_MAX_SIZE pixels or less here instead of to the point run's
// triangles (isRasterChunk, appendPointBatches): a batch of up to POINT_BATCH_POINTS of its points for each group of
// rasterMain. A thread projects a point as the triangle path's vertex stage does and writes the pixels whose centre
// its square holds with a 64-bit atomic max of (depth << 32 | instance row << 16 | point): in reversed-Z the nearest
// point wins, and on a tie the later one, as the triangles' GREATER_OR_EQUAL test. resolveFrag then writes each
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

// The rect's words to 0: no point, as depth 0 is the far plane at infinity. Up to POINT_MAX_CLEAR_GROUPS groups over
// the view, each thread a pixel or more; the pixels outside the rect are left as they are, as nothing reads them.
[shader("compute")]
[numthreads(POINT_CLEAR_GROUP_SIZE, 1, 1)]
void clearMain(uint3 threadId : SV_DispatchThreadID)
{
    SceneData *scene    = pc.scene;
    PointRasterHeader *header = scene->pointRasterPtr;
    uint4 rect          = uint4(header->rectMinX, header->rectMinY, header->rectMaxX, header->rectMaxY);
    uint pixels         = scene->viewportWidth * scene->viewportHeight;
    uint stride         = header->clearGroups * POINT_CLEAR_GROUP_SIZE;
    for (uint i = threadId.x; i < pixels; i += stride)
    {
        uint2 pixel     = uint2(i % scene->viewportWidth, i / scene->viewportWidth);
        if (all(pixel >= rect.xy) && all(pixel <= rect.zw))
            scene->pointPixelsPtr[i] = 0;
    }
}

// The pixels of a point's square: those whose centre is within half its diameter of the point's centre, edges
// included, as the triangle path's fragment test. The centre is snapped to the rasterizer's 1/256 pixel grid, where
// the triangle's corners land (their offsets from the centre are whole multiples of half a pixel), so both paths take
// the same pixels. At most 2 x 2 for a point of POINT_RASTER_MAX_SIZE.
void rasterPoint(SceneData *scene, float4 clip, float diameter, uint id)
{
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
    float2 high         = min(min(floor(center + reach - 0.5), low + 1.0), size - 1.0);
    uint64_t word       = (uint64_t(asuint(depth)) << 32) | uint64_t(id);
    for (float y = low.y; y <= high.y; y += 1.0)
    {
        for (float x = low.x; x <= high.x; x += 1.0)
        {
            // No read first to skip a losing point's atomic: the atomic returns nothing, so the thread does not wait
            // for it, and a read that it waits on cost more (measured, RTX 3060).
            InterlockedMax(scene->pointPixelsPtr[uint(y) * scene->viewportWidth + uint(x)], word);
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
    uint count              = batch.y >> 16;
    InstanceData *instance  = scene->instancesPtr + row;
    uint base               = uint(scene->geometryPtr[instance->geometryIndex].vertexOffset);
    PointData style         = (scene->materialsPtr + instance->materialIndex)->points;
    PointChunk chunk        = loadPointChunk(scene, base);
    float4x4 world          = getWorldMatrix(scene, instance);
    for (uint i = threadId.x; i < count; i += POINT_RASTER_GROUP_SIZE)
    {
        uint point          = first + i;
        uint2 packed        = scene->pointsPtr[base + POINT_CHUNK_HEADER_UNITS + point];
        if (!isPointShown(style, chunk, packed.y >> 16))
            continue;
        float3 local        = chunk.boxMin + float3(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF) * chunk.boxStep;
        float3 position     = mul(world, float4(local, 1.0)).xyz;
        float4 clip         = mul(scene->projection, mul(scene->view, float4(position, 1.0)));
        rasterPoint(scene, clip, getPointDiameter(scene, style, clip.w), row << 16 | point);
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

// The pixel's point, with its color and the motion of its centre as the triangle path gives them, at the depth the
// raster pass kept. The depth test against the opaque draws decides per sample, so a mesh in front still covers it.
[shader("fragment")]
ResolveOutput resolveFrag(ResolveVertex input)
{
    SceneData *scene        = pc.scene;
    uint2 pixel             = uint2(input.position.xy);
    uint64_t word           = scene->pointPixelsPtr[pixel.y * scene->viewportWidth + pixel.x];
    uint depthBits          = uint(word >> 32);
    if (depthBits == 0)
        discard;
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
        color *= gTextures[style.colormap].SampleLevel(getColormapCoordinate(colormapAt), 0.0);
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
    output.depth            = asfloat(depthBits);
    return output;
}
// clang-format on
