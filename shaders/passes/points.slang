// clang-format off
#include "core.slang"
#include "points.slang"
#if defined(RASM_PICK)
#include "pick.slang"
#endif

// The point run's pipeline (gpuScene.cpp): every point a triangle around its square, pulled from the point pool, unlit,
// cut to its shape here. One pipeline for every shape: a point writes its depth with SV_DepthLessEqual, so the early depth
// test still culls, and a SPHERE point pushes its rim back from the triangle, which lies at the sphere's front. With several
// samples per pixel (RASM_ALPHA_TO_COVERAGE) an edge covers samples by its distance instead of discarding, but for a
// point of POINT_PIXELS_MAX_SIZE pixels or less: it covers the whole pixels whose centre it holds, as the compute path
// (pointRaster.slang) writes it, so the two paths draw the same pixels.

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

struct PointVertex
{
    float4 clipPosition                 : SV_POSITION;
    float2 corner                       : TEXCOORD0;    // -1 to 1 across the point's square, to 3 at the triangle's corners
    nointerpolation float4 color        : COLOR;
    nointerpolation float3 sphere       : TEXCOORD1;    // the sphere's centre depth and radius in view units, and the shape (getPointShape)
#if defined(RASM_MOTION_VECTORS)
    nointerpolation float4 currentClip  : TEXCOORD2;    // the point's centre, without jitter
    nointerpolation float4 previousClip : TEXCOORD3;
#endif
#if defined(RASM_PICK)
    nointerpolation float3 position     : POSITION;     // the centre, in render space
    nointerpolation uint pickRow;
    nointerpolation uint pickFeature;
    nointerpolation uint materialIndex;
#endif
};

// SV_VertexID is the raw index (Slang subtracts BaseVertex): point p's corner c is 3p + c, of a triangle around the
// square from (-1, -1) to (1, 1) that the fragment stage cuts to the point's shape.
[shader("vertex")]
PointVertex vertMain(uint vertexID : SV_VertexID)
{
    SceneData *scene        = pc.scene;
    DrawInfo *draw          = scene->drawInfoPtr + getDrawIndex();
    InstanceData *instance  = scene->instancesPtr + draw->firstInstance;
    uint base               = uint(scene->geometryPtr[instance->geometryIndex].vertexOffset);
    PointData style         = (scene->materialsPtr + instance->materialIndex)->points;
    PointChunk chunk        = loadPointChunk(scene, base);
    uint point              = vertexID / POINT_INDICES;
    uint vertex             = vertexID - point * POINT_INDICES;
    float2 corner           = float2(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0);
    uint2 packed            = scene->pointsPtr[base + POINT_CHUNK_HEADER_UNITS + point];
    float3 local            = chunk.boxMin + float3(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF) * chunk.boxStep;

    float4x4 world          = getWorldMatrix(scene, instance);
    float3 position         = mul(world, float4(local, 1.0)).xyz;
    float4 viewPosition     = mul(scene->view, float4(position, 1.0));
    float4 clip             = mul(scene->projection, viewPosition);
    // A fixed size reads neither the spacing nor the finer share.
    bool adaptive           = (style.flags & POINT_ADAPTIVE) != 0;
    float spacing           = adaptive ? getWorldSpacing(chunk.spacing, world) : 0.0;
    float finer             = adaptive ? getPointFinerShare(instance->meshletVisibility, local) : 0.0;
    float diameter          = getPointDiameter(scene, style, spacing, finer, clip.w);
    float pixelWorld        = 2.0 * max(abs(clip.w), 1e-6) / (abs(scene->projection[1][1]) * float(scene->viewportHeight));
    float radius            = 0.5 * diameter * pixelWorld;
    float shape             = getPointShape(style, diameter);
#if defined(RASM_ALPHA_TO_COVERAGE)
    // Where the view also has the compute path, whose chunks switch paths as they move on screen, such a square covers
    // every sample of the pixels whose centre it holds, as the compute path writes them: its triangle reaches half a
    // pixel past the square, so that no sample of those pixels lies outside it. That costs about a fifth more for these
    // points (twice the triangle's area), so a view drawn by triangles only keeps the triangle, which leaves out the
    // samples of a pixel's edge beyond the square's left, bottom and diagonal sides.
    if (shape == POINT_SHAPE_PIXELS && scene->pointRasterPtr != nullptr)
        corner             *= 1.0 + 1.0 / diameter;
#endif

    PointVertex output;
    output.sphere           = float3(-viewPosition.z, radius, shape);
    if (shape == POINT_SHAPE_SPHERE)
        clip                = mul(scene->projection, viewPosition + float4(0.0, 0.0, radius, 0.0));
    clip.xy                += corner * diameter / float2(scene->viewportWidth, scene->viewportHeight) * clip.w;
    output.clipPosition     = clip;
    output.corner           = corner;
    float colormapAt        = getPointColormapPosition(style, chunk, packed.y >> 16);
    output.color            = getPointBaseColor(scene, style, chunk, base, point);
    if (colormapAt >= 0.0)
        output.color *= gTextures[style.colormap].SampleLevel(getColormapCoordinate(colormapAt), 0.0);
    if (colormapAt == POINT_OUT_OF_RANGE)
        output.color = float4(0.0);
    // An out-of-range scalar without clamp draws nothing.
    if (output.color.a <= 0.0)
        output.clipPosition = float4(0.0, 0.0, -1.0, 1.0);
#if defined(RASM_MOTION_VECTORS)
    output.currentClip      = removeJitter(scene, mul(scene->projection, viewPosition));
    output.previousClip     = mul(scene->previousViewProjection, float4(getPreviousWorldPosition(scene, instance, local), 1.0));
#endif
#if defined(RASM_PICK)
    output.position         = position;
    output.pickRow          = draw->firstInstance;
    output.pickFeature      = point;
    output.materialIndex    = instance->materialIndex;
    if ((getPickSkip() & PICK_SKIP_POINTS) != 0)
        output.clipPosition = float4(0.0, 0.0, -1.0, 1.0);
#endif
    return output;
}

// The depth of a SPHERE point's surface under the pixel (getSphereDepth), the others' at their centre.
float getPointDepth(SceneData *scene, PointVertex input, float distance2)
{
    if (input.sphere.z != POINT_SHAPE_SPHERE)
        return input.clipPosition.z;
    return getSphereDepth(scene->projection, input.sphere.x, input.sphere.y, distance2);
}

float getPointCoverage(PointVertex input, float distance2)
{
    bool square     = input.sphere.z == POINT_SHAPE_SQUARE || input.sphere.z == POINT_SHAPE_PIXELS;
    float distance  = square ? max(abs(input.corner.x), abs(input.corner.y)) : sqrt(distance2);
#if defined(RASM_ALPHA_TO_COVERAGE)
    float soft      = saturate((1.0 - distance) / max(fwidth(distance), 1e-4) + 0.5);
    return input.sphere.z == POINT_SHAPE_PIXELS ? (distance <= 1.0 ? 1.0 : 0.0) : soft;
#else
    return distance <= 1.0 ? 1.0 : 0.0;
#endif
}

#if !defined(RASM_PICK)
struct PointOutput
{
    float4 color    : SV_TARGET0;
#if defined(RASM_MOTION_VECTORS)
    float4 motion   : SV_TARGET1;
#endif
    float depth     : SV_DepthLessEqual;
};

[shader("fragment")]
PointOutput fragMain(PointVertex input)
{
    float distance2 = dot(input.corner, input.corner);
    float coverage  = getPointCoverage(input, distance2);
#if !defined(RASM_ALPHA_TO_COVERAGE)
    if (coverage <= 0.0)
        discard;
#endif
    PointOutput output;
#if defined(RASM_ALPHA_TO_COVERAGE)
    output.color    = float4(input.color.rgb, coverage);
#else
    output.color    = input.color;
#endif
#if defined(RASM_MOTION_VECTORS)
    output.motion   = float4(getMotion(input.currentClip, input.previousClip), 0.0, 0.0);
#endif
    output.depth    = getPointDepth(pc.scene, input, min(distance2, 1.0));
    return output;
}
#else
// The pick pass's targets, as rasm.slang's pickMain: the instance row + 1, the point's index in the chunk's order
// (readPicks turns it into the application's), PICK_HIT_* and the material row, then the centre and a normal to the
// camera.
struct PickOutput
{
    uint4 id        : SV_TARGET0;
    uint4 position  : SV_TARGET1;
    float depth     : SV_DepthLessEqual;
};

[shader("fragment")]
PickOutput pickMain(PointVertex input)
{
    float distance2 = dot(input.corner, input.corner);
    if (getPointCoverage(input, distance2) < 0.5)
        discard;
    // The view's rows are the camera's axes in render space, the third pointing back at the camera.
    float3 toCamera = pc.scene->projection[3][3] == 1.0 ? pc.scene->view[2].xyz : -input.position;
    PickOutput output;
    output.id       = uint4(input.pickRow + 1, input.pickFeature, PICK_HIT_FEATURE | PICK_HIT_POINT, input.materialIndex);
    output.position = uint4(asuint(input.position), packPickNormal(normalize(toCamera + float3(0.0, 1e-20, 0.0))));
    output.depth    = getPointDepth(pc.scene, input, min(distance2, 1.0));
    return output;
}
#endif
// clang-format on
