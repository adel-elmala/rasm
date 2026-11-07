// clang-format off
#include "core.slang"

// One thread per draw of the scene's list. A draw whose bounding sphere lies outside a frustum plane is dropped.
// A visible draw is packed into its cull-mode run of drawInfoPtr, and drawCountsPtr[run] counts them, so
// drawScene draws each run with drawIndexedIndirectCount.
[shader("compute")]
[numthreads(64, 1, 1)]
void compMain(uint3 id : SV_DispatchThreadID)
{
    SceneData *scene = pc.scene;
    uint drawIndex = id.x;
    if (drawIndex >= scene->drawCount)
        return;

    DrawInfo draw           = scene->allDrawsPtr[drawIndex];
    InstanceData *instance  = scene->instancesPtr + draw.firstInstance;
    GeometryData *geometry  = scene->geometryPtr + instance->geometryIndex;

    // The sphere grows with the largest axis scale, so it still holds the mesh under a non-uniform scale.
    float4x4 world          = instance->worldMatrix;
    float3x3 world3         = (float3x3)world;
    float3 center           = mul(world, float4(geometry->boundsCenter, 1.0)).xyz;
    float scale             = max(max(length(mul(world3, float3(1.0, 0.0, 0.0))), length(mul(world3, float3(0.0, 1.0, 0.0)))),
                                  length(mul(world3, float3(0.0, 0.0, 1.0))));
    float radius            = geometry->boundsRadius * scale;

    for (uint i = 0; i < 6; ++i)
    {
        float4 plane = scene->frustumPlanes[i];
        if (dot(plane.xyz, center) + plane.w < -radius)
            return;
    }

    // The list is sorted by cull mode, so a draw's run is the last one that starts at or before it.
    uint run = 0;
    for (uint i = 1; i < scene->runCount; ++i)
    {
        if (drawIndex >= scene->runFirst[i])
            run = i;
    }

    uint slot;
    InterlockedAdd(scene->drawCountsPtr[run], 1, slot);
    scene->drawInfoPtr[scene->runFirst[run] + slot] = draw;
}
// clang-format on
