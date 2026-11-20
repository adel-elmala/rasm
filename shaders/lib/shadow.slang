// clang-format off
#pragma once

#include "core.slang"

// The faces of a cube in Vulkan's order (+X, -X, +Y, -Y, +Z, -Z): the axis a face looks along, and the directions in
// which its texel coordinates s and t grow (Vulkan's cube map face table). Must match localShadows.cpp.
static const float3 CUBE_FORWARD[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
static const float3 CUBE_S[6]       = { float3(0, 0, -1), float3(0, 0, 1), float3(1, 0, 0), float3(1, 0, 0), float3(1, 0, 0), float3(-1, 0, 0) };
static const float3 CUBE_T[6]       = { float3(0, -1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1), float3(0, -1, 0), float3(0, -1, 0) };

// The face a direction from the cube's centre falls on: the one of its biggest axis.
uint getCubeFace(float3 d)
{
    float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
        return d.x >= 0.0 ? 0 : 1;
    if (a.y >= a.z)
        return d.y >= 0.0 ? 2 : 3;
    return d.z >= 0.0 ? 4 : 5;
}

// A 3x3 PCF with bilinear weights reads 4x4 texels, as four 2x2 gathers. These are the weights of gather (qx, qy)'s
// texels, in a gather's order: (x, y + 1), (x + 1, y + 1), (x + 1, y), (x, y). f is the lookup's place in its texel.
float4 getPcfWeights(float2 f, int qx, int qy)
{
    // Column (and row) weights of the 4x4 texels: the bilinear weights of 3x3 taps one texel apart, added up.
    float4 wx               = float4(1.0 - f.x, 1.0, 1.0, f.x);
    float4 wy               = float4(1.0 - f.y, 1.0, 1.0, f.y);
    float x0                = wx[2 * qx];
    float x1                = wx[2 * qx + 1];
    float y0                = wy[2 * qy];
    float y1                = wy[2 * qy + 1];
    return float4(x0 * y1, x1 * y1, x1 * y0, x0 * y0) / 9.0;
}

// The reversed-Z depth a cube face's projection gives a point at depth along the face's axis: clip z / clip w at view
// z = -depth.
float getCubeDepth(float4x4 faceProjection, float depth)
{
    return (faceProjection[2][3] - faceProjection[2][2] * depth) / depth;
}

// The lookup leaves the surface by about one texel of the map, in world units, so the surface does not shadow
// itself: along its normal, more where the light grazes it, and toward the light. A texel covers 2 / (row 0's length
// * size) at clip w 1: everywhere for an orthographic map, and at depth 1 from the light for a perspective one.
float3 offsetLookup(float3 worldPosition, float3 normal, float3 toLight, float NoL, float texelWorld)
{
    return worldPosition + normal * texelWorld * (1.0 + 2.0 * (1.0 - NoL)) + toLight * texelWorld;
}

// How much of the light reaches worldPosition in a 2D map (a cascade or a spot light's): 1 lit, 0 in shadow, -1
// outside the map. The map was drawn with reversed-Z, so a texel nearer the light holds a larger depth.
float sampleShadowMap(Sampler2D shadowMap, float4x4 shadowMatrix, float3 worldPosition, float3 normal, float3 toLight)
{
    uint width, height;
    shadowMap.GetDimensions(width, height);

    float NoL               = saturate(dot(normal, toLight));
    float w                 = mul(shadowMatrix, float4(worldPosition, 1.0)).w;
    float texelWorld        = 2.0 * w / (length(shadowMatrix[0].xyz) * float(width));
    float4 clip             = mul(shadowMatrix, float4(offsetLookup(worldPosition, normal, toLight, NoL, texelWorld), 1.0));
    float3 ndc              = clip.xyz / clip.w;
    if (clip.w <= 0.0 || any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
        return -1.0;

    // Clip space is Y-up and v = 0 is the map's top row, as in every RASM target.
    float2 size             = float2(width, height);
    float2 texel            = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * size - 0.5;
    float2 base             = floor(texel);
    float lit               = 0.0;
    for (int qy = 0; qy < 2; ++qy)
    {
        for (int qx = 0; qx < 2; ++qx)
        {
            // The corner shared by texels base - 1 + 2q and base + 2q: the gather reads those 2x2, clamped at the
            // edges by the depth's sampler.
            float4 occluder = shadowMap.Gather((base + float2(2 * qx, 2 * qy)) / size);
            lit            += dot(float4(ndc.z >= occluder), getPcfWeights(texel - base, qx, qy));
        }
    }
    return lit;
}

// How much of a point light at lightPosition reaches worldPosition, from its cube: 1 lit, 0 in shadow. Every face
// has the same 90° projection, whose depth along the face's axis is the lookup's depth. The PCF works on the face of
// the lookup like a 2D map's, and its gathers turn the texels' corners back into directions, so a gather on a face's
// edge reads the next face's texels (Vulkan cube sampling is seamless). Those hold the depth along the next face's
// axis, so each gather compares with the lookup's depth along the axis of the face it reads.
float sampleShadowCube(SamplerCube shadowCube, float4x4 faceProjection, float3 lightPosition, float3 worldPosition, float3 normal)
{
    uint width, height;
    shadowCube.GetDimensions(width, height);

    float3 toLight          = normalize(lightPosition - worldPosition);
    float NoL               = saturate(dot(normal, toLight));
    float3 d                = worldPosition - lightPosition;
    float texelWorld        = 2.0 * max(abs(d.x), max(abs(d.y), abs(d.z))) / (faceProjection[0][0] * float(width));
    d                       = offsetLookup(worldPosition, normal, toLight, NoL, texelWorld) - lightPosition;

    uint face               = getCubeFace(d);
    float depth             = dot(d, CUBE_FORWARD[face]);
    float z                 = getCubeDepth(faceProjection, depth);
    if (z < 0.0 || z > 1.0)
        return 1.0;

    float size              = float(width);
    float2 st               = (float2(dot(d, CUBE_S[face]), dot(d, CUBE_T[face])) / depth) * 0.5 + 0.5;
    float2 texel            = st * size - 0.5;
    float2 base             = floor(texel);
    float lit               = 0.0;
    for (int qy = 0; qy < 2; ++qy)
    {
        for (int qx = 0; qx < 2; ++qx)
        {
            float2 corner   = (base + float2(2 * qx, 2 * qy)) / size;
            float3 r        = CUBE_FORWARD[face] + CUBE_S[face] * (corner.x * 2.0 - 1.0) + CUBE_T[face] * (corner.y * 2.0 - 1.0);
            float4 occluder = shadowCube.Gather(r);
            float gatherZ   = getCubeDepth(faceProjection, max(dot(d, CUBE_FORWARD[getCubeFace(r)]), 1e-6));
            lit            += dot(float4(gatherZ >= occluder), getPcfWeights(texel - base, qx, qy));
        }
    }
    return lit;
}

// How much of a light reaches worldPosition: 1 lit, 0 in shadow, and 1 for a light without a shadow. Every index into
// the textures is a constant element of the scene data, the same for every pixel of the draw.
// - A directional light's first cascade whose split is beyond the fragment's view depth holds it. Past the last split
//   it is lit, faded over the last tenth of the distance so the shadow does not end in a line. The cascade loop is
//   unrolled, so neighbouring pixels can pick different cascades.
// - A spot light has one perspective map; outside it the light is not shadowed.
// - A point light has a cube.
float sampleShadow(Sampler2D gTextures[], SamplerCube gCubeTextures[], SceneData *scene, Light light, float3 worldPosition, float3 normal)
{
    if (light.shadowMap < 0)
        return 1.0;
    uint first              = uint(light.shadowMap);

    if (light.type == LightType.POINT)
    {
        ShadowMap map       = scene->shadowMaps[first];
        return sampleShadowCube(gCubeTextures[map.texture], map.matrix, light.position, worldPosition, normal);
    }
    if (light.type == LightType.SPOT)
    {
        ShadowMap map       = scene->shadowMaps[first];
        float lit           = sampleShadowMap(gTextures[map.texture], map.matrix, worldPosition, normal, normalize(light.position - worldPosition));
        return lit < 0.0 ? 1.0 : lit;
    }

    float3 toLight          = normalize(-light.direction);
    float viewDepth         = -mul(scene->view, float4(worldPosition, 1.0)).z;
    uint count              = light.shadowMapCount;
    float distance          = scene->shadowMaps[first + count - 1].split;
    [unroll]
    for (uint i = 0; i < MAX_SHADOW_CASCADES; ++i)
    {
        if (i >= count || viewDepth > scene->shadowMaps[first + i].split)
            continue;
        ShadowMap map       = scene->shadowMaps[first + i];
        float lit           = sampleShadowMap(gTextures[map.texture], map.matrix, worldPosition, normal, toLight);
        // The offset can push the lookup off a cascade's edge; the next cascade is bigger and holds it.
        if (lit < 0.0)
            continue;
        return lerp(lit, 1.0, saturate((viewDepth - 0.9 * distance) / (0.1 * distance)));
    }
    return 1.0;
}
// clang-format on
