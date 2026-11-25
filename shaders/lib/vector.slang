// clang-format off
#pragma once

#include "core.slang"

// Vector features (rasm/vector.h): lines, polygons and points, drawn by a VECTOR material. A vertex keeps the feature's
// point (its anchor), and the vertex shader spreads it by the material's width, so a style changes with no rebuild:
//   pos         the anchor, in object space
//   normal      the ground's up at the anchor
//   tangent     xyz: the direction the offset is measured from (a line's segment); w: the feature id
//   uv[0]       the offset, in half widths: x along the tangent, y across it
//   uv[1]       a line's distance along it in metres (x), or a point's icon coordinates
//   uv[2]       x: the kind (VECTOR_KIND_*), y: how the fragment measures the distance to the edge (VECTOR_METRIC_*)

// VectorData::flags. Must match rasm::gfx::VECTOR_*.
static const uint   VECTOR_METRES       = 1 << 0;
static const uint   VECTOR_GROUND       = 1 << 1;
static const uint   VECTOR_OVERLAY      = 1 << 2;
static const uint   VECTOR_ICON         = 1 << 3;
// Must match rasm::gfx::VECTOR_ORDER_SCENE.
static const int    VECTOR_ORDER_SCENE  = 128;

// uv[2].x. Must match the kinds src/scene/vector.cpp writes.
static const uint   VECTOR_KIND_FILL    = 1;
static const uint   VECTOR_KIND_LINE    = 2;
static const uint   VECTOR_KIND_POINT   = 3;
// uv[2].y: the distance to the edge, in half widths, from the offset.
static const uint   VECTOR_METRIC_ROUND     = 0;    // its length: round caps, round joins, round points
static const uint   VECTOR_METRIC_BOX       = 1;    // the larger of |x| and |y|: square caps, square icons
static const uint   VECTOR_METRIC_ACROSS    = 2;    // |y|: a line's body, its miter and bevel joins
static const uint   VECTOR_METRIC_FILL      = 3;    // 0: a polygon covers its triangles whole

// A tested feature is drawn this much nearer, as a share of its depth (reversed-Z: depth times 1 + bias), so it stays
// over the ground it was draped on: 25 cm at 1 km.
static const float  VECTOR_DEPTH_BIAS   = 1.0 / 4096.0;
// The offsets of a vertex stay within this many half widths (a miter is limited to 2, a square cap's corner is 1.42),
// and one more pixel of anti-aliasing: what culling pads a feature's sphere by.
static const float  VECTOR_MAX_OFFSET   = 2.0;

struct VectorVertex
{
    float4  clipPosition;
    float3  anchor;         // render space: its motion is the vertex's
    float2  edge;           // the offset in half widths, the anti-aliasing pixel included: the edge is at 1
    float   metric;
    float   fade;           // coverage of a feature thinner than a pixel
    float   dash;           // the distance along a line, in the unit of the dashes
};

float3 safeNormalize(float3 v, float3 fallback)
{
    float length2 = dot(v, v);
    return length2 > 1e-20 ? v * rsqrt(length2) : fallback;
}

// A clip position in pixels from the screen's centre (not flipped: only differences are used).
float2 toPixels(SceneData *scene, float4 clip)
{
    return clip.xy / max(clip.w, 1e-6) * float2(scene->viewportWidth, scene->viewportHeight) * 0.5;
}

// World units per pixel at a point of the given clip w (a perspective view's depth; 1 for an orthographic one).
float getPixelWorld(SceneData *scene, float w)
{
    return 2.0 * max(w, 1e-6) / (abs(scene->projection[1][1]) * float(scene->viewportHeight));
}

// A vertex spread across the screen from an anchor on the ground: the depth of the ground under its pixel, found
// through the ground plane's screen Jacobian at the anchor. Below the anchor on screen the ground is nearer, so a
// marker or a line facing the screen would sink into it at the anchor's depth; the nearer of the two keeps it whole.
// 0 (the far plane in reversed-Z) when the plane is seen edge-on.
float getGroundDepth(SceneData *scene, float4x4 viewProjection, float3 anchor, float3 up, float4 anchorClip, float2 offset, float pixelWorld)
{
    float3 axisX    = safeNormalize(cross(up, abs(up.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)), float3(1.0, 0.0, 0.0));
    float3 axisZ    = cross(axisX, up);
    float step      = 8.0 * pixelWorld;
    float4 clipX    = mul(viewProjection, float4(anchor + axisX * step, 1.0));
    float4 clipZ    = mul(viewProjection, float4(anchor + axisZ * step, 1.0));
    if (clipX.w <= 0.0 || clipZ.w <= 0.0)
        return 0.0;
    float2 base     = toPixels(scene, anchorClip);
    float2 perX     = (toPixels(scene, clipX) - base) / step;
    float2 perZ     = (toPixels(scene, clipZ) - base) / step;
    float det       = perX.x * perZ.y - perX.y * perZ.x;
    if (abs(det) <= 1e-6 * (dot(perX, perX) + dot(perZ, perZ)))
        return 0.0;
    float a         = (offset.x * perZ.y - offset.y * perZ.x) / det;
    float b         = (perX.x * offset.y - perX.y * offset.x) / det;
    float4 ground   = mul(viewProjection, float4(anchor + axisX * a + axisZ * b, 1.0));
    return ground.w > 0.0 ? ground.z / ground.w : 0.0;
}

VectorVertex getVectorVertex(SceneData *scene, VectorData style, float4x4 world, VertexFull vert)
{
    float4x4 viewProjection = mul(scene->projection, scene->view);
    float3x3 world3         = (float3x3)world;
    float3 anchor           = mul(world, float4(vert.pos, 1.0)).xyz;
    float3 up               = safeNormalize(mul(world3, vert.normal), float3(0.0, 1.0, 0.0));
    float3 along            = safeNormalize(mul(world3, vert.tangent.xyz), float3(1.0, 0.0, 0.0));
    uint kind               = uint(vert.uv[2].x + 0.5);

    VectorVertex output;
    output.clipPosition     = mul(viewProjection, float4(anchor, 1.0));
    output.anchor           = anchor;
    output.edge             = float2(0.0);
    output.metric           = vert.uv[2].y;
    output.fade             = 1.0;
    output.dash             = vert.uv[1].x;
    if (kind == VECTOR_KIND_LINE || kind == VECTOR_KIND_POINT)
    {
        float pixelWorld    = getPixelWorld(scene, output.clipPosition.w);
        bool metres         = (style.flags & VECTOR_METRES) != 0;
        float widthPixels   = metres ? style.width / pixelWorld : style.width;
        // Never under a pixel: a thinner feature keeps one and fades by its coverage.
        output.fade         = saturate(widthPixels);
        float halfPixels    = 0.5 * max(widthPixels, 1.0);
        if (!metres)
            output.dash     = vert.uv[1].x / pixelWorld;
        if ((style.flags & VECTOR_GROUND) != 0)
        {
            // In the ground's plane: the half width in the world, and how many pixels it covers across the screen,
            // which is less where the ground is seen edge-on. Under half a pixel it widens to half a pixel and fades.
            float3 across       = safeNormalize(cross(along, up), float3(0.0, 0.0, 1.0));
            float halfWorld     = halfPixels * pixelWorld;
            float4 side         = mul(viewProjection, float4(anchor + across * halfWorld, 1.0));
            float onScreen      = side.w > 0.0 ? length(toPixels(scene, side) - toPixels(scene, output.clipPosition)) : halfPixels;
            float widen         = onScreen < 0.5 ? min(0.5 / max(onScreen, 1e-3), 64.0) : 1.0;
            output.fade        *= saturate(onScreen / 0.5);
            output.edge         = vert.uv[0] * (1.0 + 1.0 / max(onScreen * widen, 0.5));
            float3 offset       = (output.edge.x * along + output.edge.y * across) * halfWorld * widen;
            output.clipPosition = mul(viewProjection, float4(anchor + offset, 1.0));
        }
        else
        {
            // Across the screen: a point's offset follows the screen's axes, a line's its direction on screen.
            float2 screenAlong  = float2(1.0, 0.0);
            float2 screenAcross = float2(0.0, 1.0);
            if (kind == VECTOR_KIND_LINE)
            {
                float2 base     = toPixels(scene, output.clipPosition);
                float step      = 8.0 * pixelWorld;
                float4 ahead    = mul(viewProjection, float4(anchor + along * step, 1.0));
                float2 delta    = toPixels(scene, ahead) - base;
                if (ahead.w > 0.0 && dot(delta, delta) > 1e-8)
                    screenAlong = normalize(delta);
                screenAcross    = float2(-screenAlong.y, screenAlong.x);
                // The side the vertex's y points to: the ground's across direction, seen on screen.
                float4 side     = mul(viewProjection, float4(anchor + safeNormalize(cross(along, up), float3(0.0, 0.0, 1.0)) * step, 1.0));
                if (side.w > 0.0 && dot(toPixels(scene, side) - base, screenAcross) < 0.0)
                    screenAcross = -screenAcross;
            }
            output.edge         = vert.uv[0] * (1.0 + 1.0 / halfPixels);
            float2 offset       = (output.edge.x * screenAlong + output.edge.y * screenAcross) * halfPixels;
            float depth         = output.clipPosition.z / max(output.clipPosition.w, 1e-6);
            depth               = max(depth, getGroundDepth(scene, viewProjection, anchor, up, output.clipPosition, offset, pixelWorld));
            output.clipPosition.xy += offset / (float2(scene->viewportWidth, scene->viewportHeight) * 0.5) * output.clipPosition.w;
            output.clipPosition.z = depth * output.clipPosition.w;
        }
    }
    // Reversed-Z: a larger depth is nearer. An overlay is at the near plane, so it passes every test.
    if ((style.flags & VECTOR_OVERLAY) != 0)
        output.clipPosition.z = output.clipPosition.w;
    else
        output.clipPosition.z *= 1.0 + VECTOR_DEPTH_BIAS;
    return output;
}

// The distance to the feature's edge, in half widths: 1 at the edge, past it outside.
float getVectorDistance(float2 edge, float metric)
{
    uint kind = uint(metric + 0.5);
    if (kind == VECTOR_METRIC_ROUND)
        return length(edge);
    if (kind == VECTOR_METRIC_BOX)
        return max(abs(edge.x), abs(edge.y));
    if (kind == VECTOR_METRIC_ACROSS)
        return abs(edge.y);
    return 0.0;
}

// The share of the pixel a value under its limit covers, over one pixel's change of it.
float getVectorCoverage(float value, float limit)
{
    float change = fwidth(value);
    if (change < 1e-6)
        return value <= limit ? 1.0 : 0.0;
    return saturate((limit - value) / change + 0.5);
}

// A fragment's color with its coverage in alpha: the outline band along the edge, the dashes, and the fade.
float4 shadeVector(VectorData style, float4 vertexColor, float2 edge, float metric, float fade, float dash)
{
    float4 color        = style.color * vertexColor;
    if (uint(metric + 0.5) == VECTOR_METRIC_FILL)
        return color;
    float distance      = getVectorDistance(edge, metric);
    float coverage      = getVectorCoverage(distance, 1.0) * fade;
    if (style.outlineWidth > 0.0 && style.width > 0.0)
    {
        float inner     = 1.0 - saturate(style.outlineWidth / style.width) * 2.0;
        color           = lerp(color, style.outlineColor, 1.0 - getVectorCoverage(distance, inner));
    }
    float period        = style.dashLength + style.gapLength;
    if (style.dashLength > 0.0 && style.gapLength > 0.0)
    {
        float phase     = dash - floor(dash / period) * period;
        coverage       *= getVectorCoverage(abs(phase - 0.5 * style.dashLength), 0.5 * style.dashLength);
    }
    color.a            *= coverage;
    return color;
}

// Culling's pad for a VECTOR draw: its offsets reach VECTOR_MAX_OFFSET half widths and a pixel past its anchors, in
// world units at the far side of its sphere for this view (perspective: the pixel grows with depth).
float getVectorPadding(SceneData *scene, VectorData style, float3 center, float radius)
{
    bool orthographic   = dot(scene->lodDepthPlane, scene->lodDepthPlane) == 0.0;
    float depth         = orthographic ? 1.0 : max(dot(scene->lodDepthPlane, float4(center, 1.0)) + radius, 0.0);
    float pixelWorld    = depth / max(scene->lodPixelsPerUnit, 1e-6);
    bool metres         = (style.flags & VECTOR_METRES) != 0;
    float halfWidths    = VECTOR_MAX_OFFSET * 0.5 * style.width;
    return (metres ? halfWidths : 0.0) + ((metres ? 0.0 : halfWidths) + VECTOR_MAX_OFFSET + 1.0) * pixelWorld;
}

// The order of a blended draw in the Transparent pass, 1 to 255: the scene's see-through draws at VECTOR_ORDER_SCENE.
uint getBlendOrder(SceneData *scene, InstanceData *instance)
{
    MaterialData *material = scene->materialsPtr + instance->materialIndex;
    if (material->materialId != MaterialId.VECTOR)
        return uint(VECTOR_ORDER_SCENE);
    return uint(clamp(VECTOR_ORDER_SCENE + material->vector.drawOrder, 1, 255));
}

// clang-format on
