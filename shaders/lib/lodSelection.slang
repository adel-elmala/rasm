// clang-format off
#pragma once

#include "core.slang"
#include "frustumCulling.slang"

// The draw with the index range of the coarsest LOD whose error, seen from the camera, stays under
// lodErrorPixels. The error is measured at the point of the bounding sphere nearest the camera, so a camera
// inside the sphere always gets LOD 0.
DrawInfo selectLod(SceneData *scene, DrawInfo draw, Sphere sphere)
{
    GeometryData *geometry  = scene->geometryPtr + scene->instancesPtr[draw.firstInstance].geometryIndex;
    if (geometry->lodCount <= 1 || scene->lodErrorPixels <= 0.0)
        return draw;

    // Pixels per world unit: projection[1][1] maps a unit at depth w to 2 / w NDC, which is viewportHeight / 2
    // pixels per NDC unit. An orthographic projection (projection[3][3] == 1) has w = 1 everywhere.
    float pixelsPerUnit     = abs(scene->projection[1][1]) * 0.5 * float(scene->viewportHeight);
    if (scene->projection[3][3] == 0.0)
    {
        float distance      = length(sphere.center - scene->cameraWorldPosition) - sphere.radius;
        if (distance <= 0.0)
            return draw;
        pixelsPerUnit      /= distance;
    }

    uint lod                = 0;
    float limit             = scene->lodErrorPixels / (pixelsPerUnit * sphere.scale);
    for (uint i = 1; i < min(geometry->lodCount, MAX_MESH_LODS); ++i)
    {
        if (geometry->lods[i].error <= limit)
            lod = i;
    }
    draw.firstIndex         = geometry->lods[lod].firstIndex;
    draw.indexCount         = geometry->lods[lod].indexCount;
    return draw;
}
// clang-format on
