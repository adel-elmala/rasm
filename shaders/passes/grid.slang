// clang-format off
// The camera's reference grid (src/renderer/grid.cpp): one triangle over the whole target, first in the Transparent
// pass. Each pixel's ray meets the grid's plane; the point's depth is tested against the opaque scene, and its lines
// blend over it. The lines come in levels of cellSize x subdivisions^k, and a pixel shows three levels next to each
// other: the finest that its cells are still wide enough for fades out, the next turns from major to minor, the one
// after fades in as major. Line coverage follows Ben Golus' "pristine grid" (2023): widths in pixels from the
// derivatives, lines under a pixel wide fade instead of thinning, and cells near a pixel wide fade to their average.

#include "core.slang"

struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 ndc : TEXCOORD0;
};

struct FragmentOutput
{
    float4 color  : SV_TARGET0;
#if defined(RASM_MOTION_VECTORS)
    float4 motion : SV_TARGET1;
#endif
    float depth   : SV_Depth;
};

// A level's cells fade out from this many pixels wide down to this over subdivisions.
static const float FADE_PIXELS      = 32.0;

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID)
{
    float2 uv       = float2((vertexID << 1) & 2, vertexID & 2);
    VSOutput output;
    // Clip space is Y-up, so the top of the target is y = 1.
    output.ndc      = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    output.pos      = float4(output.ndc, 0.0, 1.0);
    return output;
}

// The coverage of the lines at whole uv, widthPixels wide, where derivative is uv's change over a pixel.
float getLineCoverage(float2 uv, float2 derivative, float widthPixels)
{
    float2 targetWidth  = min(widthPixels * derivative, float2(1.0));
    bool2 invert        = targetWidth > 0.5;
    float2 width        = select(invert, 1.0 - targetWidth, targetWidth);
    float2 drawWidth    = clamp(width, derivative, float2(0.5));
    float2 smoothing    = max(derivative, float2(1e-6)) * 1.5;
    float2 distance     = abs(frac(uv) * 2.0 - 1.0);
    distance            = select(invert, distance, 1.0 - distance);
    float2 coverage     = smoothstep(drawWidth + smoothing, drawWidth - smoothing, distance);
    coverage           *= saturate(width / drawWidth);
    coverage            = lerp(coverage, width, saturate(derivative * 2.0 - 1.0));
    coverage            = select(invert, 1.0 - coverage, coverage);
    return lerp(coverage.x, 1.0, coverage.y);
}

uint getLevelSize(uint subdivisions, uint level)
{
    uint size = 1;
    for (uint i = 0; i < level; ++i)
        size *= subdivisions;
    return size;
}

// The pixel's place in a level's cells. The origin's whole cells modulo the level's size are exact, so the sum stays
// small next to the level's cells however far the origin is from the grid's.
float2 getLevelUv(GridData grid, float2 cells, uint size)
{
    float2 residue      = float2(float(grid.residueX % size), float(grid.residueZ % size));
    return (residue + float2(grid.fractionX, grid.fractionZ) + cells) / float(size);
}

// A line along an axis, centred at 0 cells: widthPixels wide, at least one pixel, fading when thinner.
float getAxisCoverage(float cells, float derivative, float widthPixels)
{
    float distance      = abs(cells) / max(derivative, 1e-20);
    return saturate(0.5 * max(widthPixels, 1.0) + 0.5 - distance) * min(widthPixels, 1.0);
}

float4 blendOver(float4 below, float3 color, float alpha)
{
    return float4(color * alpha, alpha) + below * (1.0 - alpha);
}

[shader("fragment")]
FragmentOutput fragMain(VSOutput input)
{
    GridData grid       = pc.scene->grid;
    float4x4 projection = pc.scene->projection;
    // The pixel's ray in view space, as sky.slang finds it. An orthographic ray starts across the near plane; its
    // jitter is in the projection's offset, a perspective one's comes off the pixel's NDC.
    bool perspective    = projection[3][3] == 0.0;
    float3 viewOrigin   = float3(0.0);
    float3 viewRay      = float3(0.0, 0.0, -1.0);
    if (perspective)
    {
        float2 ndc      = input.ndc - pc.scene->jitter;
#if defined(RASM_EXPORT_VIEW)
        // An export tile's window: the projection's off-center terms (its jitter is in them too).
        ndc             = input.ndc + float2(projection[0][2], projection[1][2]);
#endif
        viewRay         = float3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0);
    }
    else
        viewOrigin      = float3((input.ndc.x - projection[0][3]) / projection[0][0], (input.ndc.y - projection[1][3]) / projection[1][1], 0.0);
    // Render space has the world's axes, and the view's origin at the camera: the view matrix is its turn alone.
    float3x3 toWorld    = transpose((float3x3)pc.scene->view);
    float3 origin       = mul(toWorld, viewOrigin);
    float3 direction    = mul(toWorld, viewRay);
    float facing        = dot(direction, grid.normal);
    float t             = -(grid.height + dot(origin, grid.normal)) / facing;
    float3 hit          = origin + direction * t;

    // Cells from the origin's foot on the plane, and their change over a pixel: before any discard, so every pixel of
    // a quad has them.
    float2 cells        = float2(dot(hit, grid.axisX), dot(hit, grid.axisZ)) / grid.cellSize;
    float2 dx           = ddx(cells);
    float2 dy           = ddy(cells);
    float2 derivative   = float2(length(float2(dx.x, dy.x)), length(float2(dx.y, dy.y)));

    float4 clip         = mul(projection, mul(pc.scene->view, float4(hit, 1.0)));
    float depth         = clip.z / clip.w;
    if (grid.levelCount == 0 || !(clip.w > 0.0) || !(depth >= 0.0 && depth <= 1.0) || !all(isfinite(derivative)))
        discard;

    // The level whose cells are FADE_PIXELS wide here, and the share of the next.
    uint subdivisions   = uint(grid.subdivisions + 0.5);
    float footprint     = max(derivative.x, derivative.y);
    float scale         = getPixelScale(pc.scene);
    float lod           = max(log(footprint * FADE_PIXELS * scale) / log(grid.subdivisions), 0.0);
    uint level          = min(uint(lod), grid.levelCount - 2);
    float blend         = min(lod - float(level), 1.0);
    uint sizeA          = getLevelSize(subdivisions, level);
    uint sizeB          = sizeA * subdivisions;
    uint sizeC          = sizeB * subdivisions;
    float a             = getLineCoverage(getLevelUv(grid, cells, sizeA), derivative / float(sizeA), grid.lineWidth * scale) * (1.0 - blend);
    float b             = getLineCoverage(getLevelUv(grid, cells, sizeB), derivative / float(sizeB), lerp(grid.majorLineWidth, grid.lineWidth, blend) * scale);
    float c             = getLineCoverage(getLevelUv(grid, cells, sizeC), derivative / float(sizeC), grid.majorLineWidth * scale) * blend;
    float4 middle       = lerp(grid.majorColor, grid.minorColor, blend);
    float4 color        = blendOver(float4(0.0), grid.minorColor.rgb, grid.minorColor.a * a);
    color               = blendOver(color, middle.rgb, middle.a * b);
    color               = blendOver(color, grid.majorColor.rgb, grid.majorColor.a * c);

    float2 place        = grid.originCells + cells;
    float axisWidth     = (grid.majorLineWidth + 0.5) * scale;
    color               = blendOver(color, grid.zAxisColor.rgb, grid.zAxisColor.a * getAxisCoverage(place.x, derivative.x, axisWidth));
    color               = blendOver(color, grid.xAxisColor.rgb, grid.xAxisColor.a * getAxisCoverage(place.y, derivative.y, axisWidth));

    // A bounded grid's edges, smoothed over a pixel.
    float fade          = 1.0;
    if (grid.extent.x > 0.0)
        fade           *= saturate((grid.extent.x - abs(place.x)) / max(derivative.x, 1e-20) + 0.5);
    if (grid.extent.y > 0.0)
        fade           *= saturate((grid.extent.y - abs(place.y)) / max(derivative.y, 1e-20) + 0.5);
    // Seen edge-on the plane is a line of moiré: it fades out over the last few degrees.
    fade               *= smoothstep(0.01, 0.06, abs(facing) / length(direction));
    if (perspective && grid.fadeDistance > 0.0)
        fade           *= smoothstep(grid.fadeDistance, 0.5 * grid.fadeDistance, t * length(direction));
    color              *= fade;
    if (color.a <= 1.0 / 1024.0)
        discard;

    FragmentOutput output;
    output.color        = color;
    output.depth        = depth;
#if defined(RASM_MOTION_VECTORS)
    // The plane stands still in the world: only the camera moves it. Premultiplied like the color, as the blend run's.
    float4 previousClip = mul(pc.scene->previousViewProjection, float4(hit, 1.0));
    output.motion       = float4(getMotion(removeJitter(pc.scene, clip), previousClip) * color.a, 0.0, color.a);
#endif
    return output;
}

// clang-format on
