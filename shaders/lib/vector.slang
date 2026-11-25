// clang-format off
#pragma once

#include "core.slang"

// Vector features (rasm/vector.h): lines, polygons and points, drawn by a VECTOR material. A vertex keeps the feature's
// point (its anchor), and the vertex shader spreads it by the material's width, so a style changes with no rebuild:
//   pos         the anchor, in object space
//   normal      the ground's up at the anchor
//   tangent     xyz: the direction the offset is measured from (a line's segment); w: the feature id's bits
//   uv[0]       the offset, in half widths: x along the tangent, y across it
//   uv[1]       a line's distance along it in metres (x) and, at a join, its turn and outer side (y,
//               getScreenJoinOffset); a point's icon coordinates
//   uv[2]       x: the kind (VECTOR_KIND_*), y: how the fragment measures the distance to the edge (VECTOR_METRIC_*)

// VectorData::flags. Must match rasm::gfx::VECTOR_*.
static const uint   VECTOR_METRES       = 1 << 0;
static const uint   VECTOR_GROUND       = 1 << 1;
static const uint   VECTOR_OVERLAY      = 1 << 2;
static const uint   VECTOR_ICON         = 1 << 3;
static const uint   VECTOR_TEXT         = 1 << 4;
// Must match rasm::gfx::VECTOR_ORDER_SCENE.
static const int    VECTOR_ORDER_SCENE  = 128;

// uv[2].x. Must match the kinds src/scene/vector.cpp writes.
static const uint   VECTOR_KIND_FILL    = 1;
static const uint   VECTOR_KIND_LINE    = 2;
static const uint   VECTOR_KIND_POINT   = 3;
static const uint   VECTOR_KIND_GLYPH   = 4;    // a label's glyph: uv[0] its corner in ems from the anchor, uv[1] its atlas
                                                // coordinates, uv[2].y 1 on a halo's quad, tangent.x the bits of its
                                                // label's index in the mesh
// uv[2].y: the distance to the edge, in half widths, from the offset.
static const uint   VECTOR_METRIC_ROUND     = 0;    // its length: round caps, round joins, round points
static const uint   VECTOR_METRIC_BOX       = 1;    // the larger of |x| and |y|: square caps, square icons
static const uint   VECTOR_METRIC_ACROSS    = 2;    // |y|: a line's body, its miter and bevel joins
static const uint   VECTOR_METRIC_FILL      = 3;    // 0: a polygon covers its triangles whole

// A tested feature is drawn this much nearer, as a share of its distance, so it stays over the ground it was draped
// on: 25 cm at 1 km.
static const float  VECTOR_DEPTH_BIAS   = 1.0 / 4096.0;
// The offsets of a vertex stay within this many half widths (a miter is limited to 2, a square cap's corner is 1.42),
// and one more pixel of anti-aliasing: what culling pads a feature's sphere by.
static const float  VECTOR_MAX_OFFSET   = 2.0;
// Must match rasm::MAX_VECTOR_MITER.
static const float  MAX_VECTOR_MITER    = 2.0;
// Ems a label's text may reach from its anchor: culling pads a text draw's sphere by it. Must match
// rasm::MAX_LABEL_REACH.
static const float  VECTOR_LABEL_REACH  = 12.0;

struct VectorVertex
{
    float4  clipPosition;
    float3  anchor;         // render space: its motion is the vertex's
    float2  edge;           // the offset in half widths, the anti-aliasing pixel included: the edge is at 1
    float   metric;
    float   fade;           // coverage of a feature thinner than a pixel
    float   dash;           // a line's distance along it, in the unit of the dashes; 0 for the other kinds
    float   kind;           // VECTOR_KIND_*
};

static const float  VECTOR_PI           = 3.14159265358979;

float3 safeNormalize(float3 v, float3 fallback)
{
    float length2 = dot(v, v);
    return length2 > 1e-20 ? v * rsqrt(length2) : fallback;
}

// Any direction across the ground: the one src/scene/vector.cpp's getGroundAxis picks.
float3 getGroundAxis(float3 up)
{
    return safeNormalize(cross(up, abs(up.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)), float3(1.0, 0.0, 0.0));
}

// The direction across a segment: cross(along, up), as the CPU computes it, with the same fallback.
float3 getAcross(float3 along, float3 up)
{
    return safeNormalize(cross(along, up), getGroundAxis(up));
}

// A clip w no smaller than the near plane's: a vertex behind the camera gets the sizes of one at the near plane, not
// sizes that grow without bound. An orthographic view's w is 1.
float getSafeW(SceneData *scene, float w)
{
    return scene->projection[3][3] == 0.0 ? max(w, scene->nearZ) : w;
}

// A clip position in pixels from the screen's centre (not flipped: only differences are used).
float2 toPixels(SceneData *scene, float4 clip)
{
    return clip.xy / getSafeW(scene, clip.w) * float2(scene->viewportWidth, scene->viewportHeight) * 0.5;
}

// World units per pixel at a point of the given clip w (a perspective view's depth; 1 for an orthographic one).
float getPixelWorld(SceneData *scene, float w)
{
    return 2.0 * getSafeW(scene, w) / (abs(scene->projection[1][1]) * float(scene->viewportHeight));
}

float getViewDepth(SceneData *scene, float3 position)
{
    return -mul(scene->view, float4(position, 1.0)).z;
}

// A 3D direction at the anchor, seen on screen; fallback when it points at the camera.
float2 getScreenDirection(SceneData *scene, float4x4 viewProjection, float3 anchor, float2 base, float3 direction, float step, float2 fallback)
{
    float4 ahead    = mul(viewProjection, float4(anchor + direction * step, 1.0));
    float2 delta    = toPixels(scene, ahead) - base;
    return ahead.w > 0.0 && dot(delta, delta) > 1e-8 ? normalize(delta) : fallback;
}

// A segment's along (xy) and across (zw) on screen: across is perpendicular to along, on the side of getAcross.
float4 getScreenFrame(SceneData *scene, float4x4 viewProjection, float3 anchor, float2 base, float3 along, float3 up, float step)
{
    float2 screenAlong  = getScreenDirection(scene, viewProjection, anchor, base, along, step, float2(1.0, 0.0));
    float2 screenAcross = float2(-screenAlong.y, screenAlong.x);
    float2 side         = getScreenDirection(scene, viewProjection, anchor, base, getAcross(along, up), step, screenAcross);
    return float4(screenAlong, dot(side, screenAcross) < 0.0 ? -screenAcross : screenAcross);
}

float wrapAngle(float angle)
{
    return angle - 2.0 * VECTOR_PI * floor((angle + VECTOR_PI) / (2.0 * VECTOR_PI));
}

// A join's offset turned from the ground's angles to the screen's. The CPU builds a join from the turn on the ground
// between this vertex's segment and the other one (uv[1].y: the turn in radians from this segment's outer normal to
// the other's, plus 8 times the outer side); across the screen the turn looks wider or narrower, so each rim offset
// keeps its share of the turn, and a miter's tip takes its length for the screen's turn (past MAX_VECTOR_MITER: the
// middle of the bevel).
float2 getScreenJoinOffset(SceneData *scene, float4x4 viewProjection, float3 anchor, float2 base, float3 along, float3 up, float step, float4 frame, float2 offset, float encodedTurn)
{
    float side          = encodedTurn > 0.0 ? 1.0 : -1.0;
    float turn          = encodedTurn - 8.0 * side;
    float start         = side > 0.0 ? 0.0 : VECTOR_PI;
    float length0       = length(offset);
    if (abs(turn) < 1e-4 || length0 < 1e-6)
        return offset;
    // The other segment, in this one's (along, across) coordinates, then in the world and on screen.
    float2 otherAcross  = float2(sin(start + turn), cos(start + turn)) * side;
    float3 otherAlong   = along * otherAcross.y - getAcross(along, up) * otherAcross.x;
    float4 otherFrame   = getScreenFrame(scene, viewProjection, anchor, base, otherAlong, up, step);
    float2 otherOuter   = otherFrame.zw * side;
    float screenTurn    = wrapAngle(atan2(dot(otherOuter, frame.xy), dot(otherOuter, frame.zw)) - start);
    // A turn that flips its direction on screen keeps none.
    if (screenTurn * turn < 0.0)
        screenTurn = 0.0;
    float angle         = start + wrapAngle(atan2(offset.x, offset.y) - start) * (screenTurn / turn);
    float length1       = length0;
    if (length0 > 1.0001)
    {
        float halfCosine = cos(0.5 * screenTurn);
        length1         = halfCosine > 1.0 / MAX_VECTOR_MITER ? 1.0 / halfCosine : halfCosine;
    }
    return float2(sin(angle), cos(angle)) * length1;
}

// The point of the ground plane at the anchor under the pixel offset from it, found through the plane's screen
// Jacobian at the anchor. w = 0 when the plane is seen edge-on.
float4 getGroundPoint(SceneData *scene, float4x4 viewProjection, float3 anchor, float3 up, float2 base, float2 offset, float pixelWorld)
{
    float3 axisX    = getGroundAxis(up);
    float3 axisZ    = cross(axisX, up);
    float step      = 8.0 * pixelWorld;
    float4 clipX    = mul(viewProjection, float4(anchor + axisX * step, 1.0));
    float4 clipZ    = mul(viewProjection, float4(anchor + axisZ * step, 1.0));
    if (clipX.w <= 0.0 || clipZ.w <= 0.0)
        return float4(0.0);
    float2 perX     = (toPixels(scene, clipX) - base) / step;
    float2 perZ     = (toPixels(scene, clipZ) - base) / step;
    float det       = perX.x * perZ.y - perX.y * perZ.x;
    if (abs(det) <= 1e-6 * (dot(perX, perX) + dot(perZ, perZ)))
        return float4(0.0);
    float a         = (offset.x * perZ.y - offset.y * perZ.x) / det;
    float b         = (perX.x * offset.y - perX.y * offset.x) / det;
    return float4(anchor + axisX * a + axisZ * b, 1.0);
}

// firstLabel: the instance's first label slot (InstanceData::meshletVisibility of a mesh with labels), -1 for none.
VectorVertex getVectorVertex(SceneData *scene, VectorData style, float4x4 world, VertexFull vert, int firstLabel)
{
    float4x4 viewProjection = mul(scene->projection, scene->view);
    float3x3 world3         = (float3x3)world;
    float3 anchor           = mul(world, float4(vert.pos, 1.0)).xyz;
    float3 up               = safeNormalize(mul(world3, vert.normal), float3(0.0, 1.0, 0.0));
    float3 along            = safeNormalize(mul(world3, vert.tangent.xyz), getGroundAxis(up));
    uint kind               = uint(vert.uv[2].x + 0.5);

    VectorVertex output;
    output.clipPosition     = mul(viewProjection, float4(anchor, 1.0));
    output.anchor           = anchor;
    output.edge             = float2(0.0);
    output.metric           = vert.uv[2].y;
    output.fade             = 1.0;
    output.dash             = 0.0;
    output.kind             = float(kind);
    // The world point whose depth the vertex takes: the anchor, the vertex itself, or the ground under it.
    float3 depthPoint       = anchor;
    if (kind == VECTOR_KIND_GLYPH)
    {
        // A glyph of a label: its corner in ems times the text's size, across the screen's axes, and its label's alpha
        // from this view's placement (0: placed nowhere, the quad collapses). metric is 1 on the halo's quads, which
        // collapse without a halo; dash carries the halo in pixels.
        float alpha         = 1.0;
        if (scene->labelsPtr != nullptr && firstLabel >= 0)
            alpha           = scene->labelsPtr[uint(firstLabel) + asuint(vert.tangent.x)];
        float pixelWorld    = getPixelWorld(scene, output.clipPosition.w);
        bool metres         = (style.flags & VECTOR_METRES) != 0;
        float textPixels    = metres ? style.width / pixelWorld : style.width;
        float halo          = metres ? style.outlineWidth / pixelWorld : style.outlineWidth;
        if (alpha <= 0.0 || style.font == INVALID_TEXTURE_INDEX || (vert.uv[2].y > 0.5 && halo <= 0.0))
        {
            output.clipPosition = float4(0.0, 0.0, -1.0, 1.0);
            return output;
        }
        output.fade         = alpha;
        output.dash         = halo;
        float2 base         = toPixels(scene, output.clipPosition);
        float2 pixels       = vert.uv[0] * textPixels;
        float4 ground       = getGroundPoint(scene, viewProjection, anchor, up, base, pixels, pixelWorld);
        if (ground.w > 0.0 && getViewDepth(scene, ground.xyz) < getViewDepth(scene, anchor))
            depthPoint      = ground.xyz;
        output.clipPosition.xy += pixels / (float2(scene->viewportWidth, scene->viewportHeight) * 0.5) * getSafeW(scene, output.clipPosition.w);
        // Without TAA's jitter: a still label is drawn on the same pixels every frame, so the history stays sharp.
        output.clipPosition = removeJitter(scene, output.clipPosition);
    }
    else if (kind == VECTOR_KIND_LINE || kind == VECTOR_KIND_POINT)
    {
        float pixelWorld    = getPixelWorld(scene, output.clipPosition.w);
        bool metres         = (style.flags & VECTOR_METRES) != 0;
        float widthPixels   = metres ? style.width / pixelWorld : style.width;
        // Never under a pixel: a thinner feature keeps one and fades by its coverage.
        output.fade         = saturate(widthPixels);
        float halfPixels    = 0.5 * max(widthPixels, 1.0);
        if (kind == VECTOR_KIND_LINE)
            output.dash     = metres ? vert.uv[1].x : vert.uv[1].x / pixelWorld;
        if ((style.flags & VECTOR_GROUND) != 0)
        {
            // In the ground's plane: the half width in the world, and how many pixels it covers across the screen,
            // which is less where the ground is seen edge-on. Under half a pixel it widens to half a pixel and fades.
            float3 across       = getAcross(along, up);
            float halfWorld     = halfPixels * pixelWorld;
            float4 side         = mul(viewProjection, float4(anchor + across * halfWorld, 1.0));
            float onScreen      = side.w > 0.0 ? length(toPixels(scene, side) - toPixels(scene, output.clipPosition)) : halfPixels;
            float widen         = onScreen < 0.5 ? min(0.5 / max(onScreen, 1e-3), 64.0) : 1.0;
            output.fade        *= saturate(onScreen / 0.5);
            output.edge         = vert.uv[0] * (1.0 + 1.0 / max(onScreen * widen, 0.5));
            depthPoint          = anchor + (output.edge.x * along + output.edge.y * across) * halfWorld * widen;
            output.clipPosition = mul(viewProjection, float4(depthPoint, 1.0));
        }
        else
        {
            // Across the screen: a point's offset follows the screen's axes, a line's its direction on screen, and a
            // join's offset is turned to the screen's angles.
            float2 base         = toPixels(scene, output.clipPosition);
            float step          = 8.0 * pixelWorld;
            float4 frame        = float4(1.0, 0.0, 0.0, 1.0);
            float2 offset       = vert.uv[0];
            if (kind == VECTOR_KIND_LINE)
            {
                frame           = getScreenFrame(scene, viewProjection, anchor, base, along, up, step);
                if (vert.uv[1].y != 0.0)
                    offset      = getScreenJoinOffset(scene, viewProjection, anchor, base, along, up, step, frame, offset, vert.uv[1].y);
            }
            output.edge         = offset * (1.0 + 1.0 / halfPixels);
            float2 pixels       = (output.edge.x * frame.xy + output.edge.y * frame.zw) * halfPixels;
            // Below the anchor on screen the ground is nearer: a corner takes the ground's depth under it when that is
            // nearer, so a marker or a line facing the screen does not sink into the ground it stands on.
            float4 ground       = getGroundPoint(scene, viewProjection, anchor, up, base, pixels, pixelWorld);
            if (ground.w > 0.0 && getViewDepth(scene, ground.xyz) < getViewDepth(scene, anchor))
                depthPoint      = ground.xyz;
            output.clipPosition.xy += pixels / (float2(scene->viewportWidth, scene->viewportHeight) * 0.5) * getSafeW(scene, output.clipPosition.w);
        }
    }
    // Reversed-Z: a larger depth is nearer. An overlay is at the near plane, so it passes every test. A tested feature
    // takes the depth of its depth point moved toward the camera by VECTOR_DEPTH_BIAS of its distance, which holds for
    // any projection: perspective, orthographic, a finite far plane.
    if ((style.flags & VECTOR_OVERLAY) != 0)
    {
        output.clipPosition.z = output.clipPosition.w;
    }
    else
    {
        float distance      = max(abs(getViewDepth(scene, depthPoint)), 1e-3);
        float4 biased       = mul(viewProjection, float4(depthPoint + scene->view[2].xyz * (VECTOR_DEPTH_BIAS * distance), 1.0));
        if (biased.w != 0.0)
            output.clipPosition.z = biased.z / biased.w * output.clipPosition.w;
    }
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
float4 shadeVector(VectorData style, float4 vertexColor, float2 edge, float metric, float fade, float dash, uint kind)
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
    if (kind == VECTOR_KIND_LINE && style.dashLength > 0.0 && style.gapLength > 0.0)
    {
        float phase     = dash - floor(dash / period) * period;
        coverage       *= getVectorCoverage(abs(phase - 0.5 * style.dashLength), 0.5 * style.dashLength);
    }
    color.a            *= coverage;
    return color;
}

// A glyph's color with its coverage in alpha, from the atlas's distances (0.5 + distance / distanceRange), smooth over
// one pixel at any size. A label draws its halo's quads first (haloPass: the outline color, halo pixels past the edge,
// at most what the atlas stores), then its fill's, so a glyph's halo never covers its neighbour's fill.
float4 shadeGlyph(Sampler2D atlas, VectorData style, float4 vertexColor, float2 uv, float halo, float fade, bool haloPass)
{
    uint width;
    uint height;
    atlas.GetDimensions(width, height);
    float value         = atlas.Sample(uv).r;
    // Screen pixels per unit of stored distance, from how fast uv moves over the screen.
    float2 unitRange    = style.distanceRange / float2(width, height);
    float screenRange   = max(0.5 * dot(unitRange, 1.0 / max(fwidth(uv), float2(1e-8))), 1.0);
    float distance      = (value - 0.5) * screenRange;
    float reach         = haloPass ? clamp(halo, 0.0, 0.5 * screenRange - 1.0) : 0.0;
    float4 color        = haloPass ? style.outlineColor : style.color * vertexColor;
    color.a            *= saturate(distance + reach + 0.5) * fade;
    return color;
}

// Culling's pad for a VECTOR draw: its offsets reach VECTOR_MAX_OFFSET half widths (a label's VECTOR_LABEL_REACH ems and
// its halo) and a pixel past its anchors, in world units at the far side of its sphere for this view (perspective: the
// pixel grows with depth).
float getVectorPadding(SceneData *scene, VectorData style, float3 center, float radius)
{
    bool orthographic   = dot(scene->lodDepthPlane, scene->lodDepthPlane) == 0.0;
    float depth         = orthographic ? 1.0 : max(dot(scene->lodDepthPlane, float4(center, 1.0)) + radius, 0.0);
    float pixelWorld    = depth / max(scene->lodPixelsPerUnit, 1e-6);
    bool metres         = (style.flags & VECTOR_METRES) != 0;
    float halfWidths    = (style.flags & VECTOR_TEXT) != 0 ? VECTOR_LABEL_REACH * style.width + style.outlineWidth : VECTOR_MAX_OFFSET * 0.5 * style.width;
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
