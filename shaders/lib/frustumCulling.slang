// clang-format off
#pragma once

#include "core.slang"
#include "vector.slang"
#include "points.slang"
#include "glyphs.slang"

struct Sphere
{
    float3          center;
    float           radius;
    float           scale;          // the world matrix's largest stretch, bounded from above
};

// An object-space sphere of the instance in render space (core.slang). The radius grows by the matrix's largest
// stretch: the square root of the largest eigenvalue of transpose(M) * M, whose entries are the dot products of M's
// columns. Its largest row sum of absolute values bounds that eigenvalue (Gershgorin), exactly when the columns are
// orthogonal, and never too small when a non-uniform scale is followed by a rotation.
Sphere getInstanceSphere(SceneData *scene, InstanceData *instance, float3 center, float radius)
{
    float4x4 world          = getWorldMatrix(scene, instance);
    float3x3 world3         = (float3x3)world;
    float3 c0               = mul(world3, float3(1.0, 0.0, 0.0));
    float3 c1               = mul(world3, float3(0.0, 1.0, 0.0));
    float3 c2               = mul(world3, float3(0.0, 0.0, 1.0));
    float d01               = abs(dot(c0, c1));
    float d02               = abs(dot(c0, c2));
    float d12               = abs(dot(c1, c2));
    float stretch2          = max(max(dot(c0, c0) + d01 + d02, dot(c1, c1) + d01 + d12), dot(c2, c2) + d02 + d12);

    Sphere sphere;
    sphere.scale            = sqrt(stretch2);
    sphere.radius           = radius * sphere.scale;
    sphere.center           = mul(world, float4(center, 1.0)).xyz;
    return sphere;
}

// The draw's bounding sphere in render space: its geometry's, or a skinned sub-mesh's around its posed vertices, which
// the Animate pass writes each frame.
Sphere getWorldSphere(SceneData *scene, DrawInfo draw)
{
    InstanceData *instance  = scene->instancesPtr + draw.firstInstance;
    GeometryData *geometry  = scene->geometryPtr + instance->geometryIndex;
    float3 center           = geometry->boundsCenter;
    float radius            = geometry->boundsRadius;
    if (instance->animationRow >= 0)
    {
        InstanceTransform *transform = scene->animatedTransformsPtr + instance->animationRow;
        if (transform->boundsRadius >= 0.0)
        {
            center          = transform->boundsCenter;
            radius          = transform->boundsRadius;
        }
    }
    Sphere sphere           = getInstanceSphere(scene, instance, center, radius);
    // A vector feature spreads past its anchors by its width, in pixels of this view: the sphere grows by as much, so
    // nothing of it is culled at the screen's edge or behind a small occluder.
    MaterialData *material  = scene->materialsPtr + instance->materialIndex;
    if (material->materialId == MaterialId.VECTOR)
        sphere.radius      += getVectorPadding(scene, material->vector, sphere.center, sphere.radius);
    else if (material->materialId == MaterialId.POINTS)
        sphere.radius      += getPointPadding(scene, material->points, sphere.center, sphere.radius);
    else if (material->materialId == MaterialId.GLYPHS && geometry->lodCount == 0)
        sphere.radius      += getGlyphChunkPadding(scene, instance, geometry) * sphere.scale;
    return sphere;
}

// What culling does differently for a draw of a VECTOR material: a shadow map's view leaves it out (it casts no
// shadow), an overlay is never occluded, and it is drawn whole (its offsets would break meshlet bounds).
uint getVectorFlags(SceneData *scene, DrawInfo draw)
{
    InstanceData *instance  = scene->instancesPtr + draw.firstInstance;
    MaterialData *material  = scene->materialsPtr + instance->materialIndex;
    return material->materialId == MaterialId.VECTOR ? material->vector.flags | 0x80000000u : 0u;
}

// A sphere fully outside one of the frustum planes is culled.
bool isInFrustum(SceneData *scene, Sphere sphere)
{
    for (uint i = 0; i < 6; ++i)
    {
        float4 plane = scene->frustumPlanes[i];
        if (dot(plane.xyz, sphere.center) + plane.w < -sphere.radius)
            return false;
    }
    return true;
}
// clang-format on
