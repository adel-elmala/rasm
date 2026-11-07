// clang-format off
#pragma once

#include "core.slang"

struct Sphere
{
    float3          center;
    float           radius;
};

// The draw's geometry bounding sphere in world space.
Sphere getWorldSphere(SceneData *scene, DrawInfo draw)
{
    InstanceData *instance  = scene->instancesPtr + draw.firstInstance;
    GeometryData *geometry  = scene->geometryPtr + instance->geometryIndex;

    // The radius grows by the matrix's largest stretch: the square root of the largest eigenvalue of
    // transpose(M) * M, whose entries are the dot products of M's columns. Its largest row sum of absolute
    // values bounds that eigenvalue (Gershgorin), exactly when the columns are orthogonal, and never too small
    // when a non-uniform scale is followed by a rotation.
    float4x4 world          = instance->worldMatrix;
    float3x3 world3         = (float3x3)world;
    float3 c0               = mul(world3, float3(1.0, 0.0, 0.0));
    float3 c1               = mul(world3, float3(0.0, 1.0, 0.0));
    float3 c2               = mul(world3, float3(0.0, 0.0, 1.0));
    float d01               = abs(dot(c0, c1));
    float d02               = abs(dot(c0, c2));
    float d12               = abs(dot(c1, c2));
    float stretch2          = max(max(dot(c0, c0) + d01 + d02, dot(c1, c1) + d01 + d12), dot(c2, c2) + d02 + d12);

    Sphere sphere;
    sphere.radius           = geometry->boundsRadius * sqrt(stretch2);
    sphere.center           = mul(world, float4(geometry->boundsCenter, 1.0)).xyz;
    return sphere;
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
