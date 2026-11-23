// clang-format off
#pragma once

#include "core.slang"
#include "frustumCulling.slang"

// A meshlet's back-face test: meshoptimizer's cone test (meshopt_computeMeshletBounds), done in the instance's own
// space. Which side of a triangle's plane a point is on does not change under an affine matrix, also one that scales
// unevenly or mirrors, so the camera is moved into object space instead of the cone into world space. The test holds
// only for runs culled by face (SceneData::coneRuns): a mirroring instance is drawn with FRONT culling, which drops the
// same triangles.
bool isMeshletBackFacing(SceneData *scene, InstanceData *instance, GeometryMeshlet meshlet)
{
    float4x4 world          = getWorldMatrix(scene, instance);
    // The normal matrix is the inverse transpose of the world matrix's 3x3, so its transpose is the inverse.
    float3x3 toObject       = transpose(getNormalMatrix(scene, instance));
    float4 viewPoint        = scene->cullViewPoint;
    if (viewPoint.w == 0.0)
    {
        // An orthographic view looks the same way from everywhere.
        float3 direction    = normalize(mul(toObject, viewPoint.xyz));
        return dot(direction, meshlet.coneAxis) >= meshlet.coneCutoff;
    }
    float3 camera           = mul(toObject, viewPoint.xyz - mul(world, float4(0.0, 0.0, 0.0, 1.0)).xyz);
    float3 toCenter         = meshlet.center - camera;
    return dot(toCenter, meshlet.coneAxis) >= meshlet.coneCutoff * length(toCenter) + meshlet.radius;
}

// A meshlet's place among an instance's visibility bits: the bit of meshlet m of the instance's LOD.
bool readMeshletBit(SceneData *scene, InstanceData *instance, uint meshlet)
{
    return (scene->meshletVisibilityPtr[uint(instance->meshletVisibility) + meshlet / 32] & (1u << (meshlet % 32))) != 0;
}
// clang-format on
