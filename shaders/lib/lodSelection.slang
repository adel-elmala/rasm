// clang-format off
#pragma once

#include "core.slang"
#include "frustumCulling.slang"

// The coarsest LOD whose error, seen from the camera, stays under lodErrorPixels. The error is measured at the
// point of the bounding sphere nearest the camera, so a camera inside the sphere always gets LOD 0. A forced LOD
// (F2) wins, limited to the mesh's coarsest one.
uint selectLod(SceneData *scene, DrawInfo draw, Sphere sphere)
{
    GeometryData *geometry  = scene->geometryPtr + scene->instancesPtr[draw.firstInstance].geometryIndex;
    uint lodCount           = clamp(geometry->lodCount, 1, MAX_MESH_LODS);
    if (scene->forcedLod >= 0)
        return min(uint(scene->forcedLod), lodCount - 1);
    if (lodCount <= 1 || scene->lodErrorPixels <= 0.0)
        return 0;

    // Pixels per world unit: projection[1][1] maps a unit at depth w to 2 / w NDC, which is viewportHeight / 2
    // pixels per NDC unit. An orthographic projection (projection[3][3] == 1) has w = 1 everywhere.
    float pixelsPerUnit     = abs(scene->projection[1][1]) * 0.5 * float(scene->viewportHeight);
    if (scene->projection[3][3] == 0.0)
    {
        float distance      = length(sphere.center - scene->cameraWorldPosition) - sphere.radius;
        if (distance <= 0.0)
            return 0;
        pixelsPerUnit      /= distance;
    }

    uint lod                = 0;
    float limit             = scene->lodErrorPixels / (pixelsPerUnit * sphere.scale);
    for (uint i = 1; i < lodCount; ++i)
    {
        if (geometry->lods[i].error <= limit)
            lod = i;
    }
    return lod;
}

// The draw with the index range of one of its LODs.
DrawInfo getLodDraw(SceneData *scene, DrawInfo draw, uint lod)
{
    GeometryData *geometry  = scene->geometryPtr + scene->instancesPtr[draw.firstInstance].geometryIndex;
    draw.firstIndex         = geometry->lods[lod].firstIndex;
    draw.indexCount         = geometry->lods[lod].indexCount;
    return draw;
}
// clang-format on
