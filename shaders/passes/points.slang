// clang-format off
#include "core.slang"
#include "points.slang"
#if defined(RASM_PICK)
#include "pick.slang"
#endif

// The point run's pipeline (gpuScene.cpp): every point a triangle around its square, pulled from the point pool, unlit,
// cut to its shape here. One pipeline for every shape: a point writes its depth with SV_DepthLessEqual, so the early depth
// test still culls, and a SPHERE point pushes its rim back from the triangle, which lies at the sphere's front. With several
// samples per pixel (RASM_ALPHA_TO_COVERAGE) a round edge covers samples by its distance instead of discarding.

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

struct PointVertex
{
    float4 clipPosition                 : SV_POSITION;
    float2 corner                       : TEXCOORD0;    // -1 to 1 across the point's square, to 3 at the triangle's corners
    nointerpolation float4 color        : COLOR;
    nointerpolation float3 sphere       : TEXCOORD1;    // the sphere's centre depth and radius in view units, and the shape: 0 square, 1 round, 2 sphere
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

float4 getPointColor(SceneData *scene, PointData style, PointChunk chunk, uint base, uint point, uint scalarStep)
{
    float4 color = style.color;
    if ((style.flags & POINT_SCALAR) != 0)
    {
        if ((chunk.flags & POINT_CHUNK_SCALARS) == 0 || style.colormap == INVALID_TEXTURE_INDEX)
            return color;
        float value = chunk.scalarMin + float(scalarStep) * chunk.scalarStep;
        if ((style.flags & POINT_LOG) != 0)
            value = value > 0.0 ? log(value) : -1e30;
        float span  = style.rangeMax - style.rangeMin;
        float t     = span != 0.0 ? (value - style.rangeMin) / span : 0.0;
        if ((style.flags & POINT_CLAMP) == 0 && !(t >= 0.0 && t <= 1.0))
            return float4(0.0);
        t           = saturate(t);
        return color * gTextures[style.colormap].SampleLevel(float2((t * 255.0 + 0.5) / 256.0, 0.5), 0.0);
    }
    if ((chunk.flags & POINT_CHUNK_COLORS) == 0)
        return color;
    uint2 pair      = scene->pointsPtr[base + chunk.colorOffset / 2 + point / 2];
    uint packed     = (point & 1) != 0 ? pair.y : pair.x;
    float4 decoded  = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    return color * float4(decodeSrgb(decoded.r), decodeSrgb(decoded.g), decodeSrgb(decoded.b), decoded.a);
}

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
    // A point is at least a pixel across, so it neither vanishes nor flickers; its size in the world at its depth.
    float pixelWorld        = 2.0 * max(abs(clip.w), 1e-6) / (abs(scene->projection[1][1]) * float(scene->viewportHeight));
    bool metres             = (style.flags & POINT_METRES) != 0;
    float diameter          = max(metres ? style.size / pixelWorld : style.size, 1.0);
    float radius            = 0.5 * diameter * pixelWorld;
    // A round point under 3 pixels is drawn square: its disc would miss the pixel centres it covers.
    float shape             = (style.flags & POINT_SPHERE) != 0 ? 2.0 : ((style.flags & POINT_ROUND) != 0 ? 1.0 : 0.0);
    shape                   = diameter < 3.0 ? 0.0 : shape;

    PointVertex output;
    output.sphere           = float3(-viewPosition.z, radius, shape);
    if (shape == 2.0)
        clip                = mul(scene->projection, viewPosition + float4(0.0, 0.0, radius, 0.0));
    clip.xy                += corner * diameter / float2(scene->viewportWidth, scene->viewportHeight) * clip.w;
    output.clipPosition     = clip;
    output.corner           = corner;
    output.color            = getPointColor(scene, style, chunk, base, point, packed.y >> 16);
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

// The view depth z of a SPHERE point's surface under the pixel, back through the projection to the depth buffer's
// value: (P22 (-z) + P23) / (P32 (-z) + P33), for a perspective and an orthographic projection alike.
float getPointDepth(SceneData *scene, PointVertex input, float distance2)
{
    if (input.sphere.z != 2.0)
        return input.clipPosition.z;
    float z         = input.sphere.x - input.sphere.y * sqrt(saturate(1.0 - distance2));
    float4x4 p      = scene->projection;
    return (p[2][2] * -z + p[2][3]) / (p[3][2] * -z + p[3][3]);
}

float getPointCoverage(PointVertex input, float distance2)
{
    float distance  = input.sphere.z == 0.0 ? max(abs(input.corner.x), abs(input.corner.y)) : sqrt(distance2);
#if defined(RASM_ALPHA_TO_COVERAGE)
    return saturate((1.0 - distance) / max(fwidth(distance), 1e-4) + 0.5);
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
