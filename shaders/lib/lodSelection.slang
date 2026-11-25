// clang-format off
#pragma once

#include "core.slang"
#include "frustumCulling.slang"
#include "points.slang"

// The coarsest LOD whose error, seen from the LOD camera, stays under lodErrorPixels. The LOD camera is the view's
// own, or for a shadow map the camera of the view that reads it (prepareSceneView), so both pick the same LOD. The
// error is measured at the point of the bounding sphere nearest the camera, so a camera inside the sphere always gets
// LOD 0. A forced LOD (F2) wins, limited to the mesh's coarsest one.
uint selectLod(SceneData *scene, GeometryData *geometry, Sphere sphere)
{
    uint lodCount           = clamp(geometry->lodCount, 1, MAX_MESH_LODS);
    if (scene->forcedLod >= 0)
        return min(uint(scene->forcedLod), lodCount - 1);
    if (lodCount <= 1 || scene->lodErrorPixels <= 0.0)
        return 0;

    // Pixels per world unit at view depth 1, divided by the view depth. It is the depth, not the distance: off the
    // screen's centre an object is nearer in depth than in distance. An orthographic camera has no depth plane, and
    // the same pixels per unit everywhere.
    float pixelsPerUnit     = scene->lodPixelsPerUnit;
    if (any(scene->lodDepthPlane != float4(0.0)))
    {
        float depth         = dot(scene->lodDepthPlane, float4(sphere.center, 1.0)) - sphere.radius;
        if (depth <= 0.0)
            return 0;
        pixelsPerUnit      /= depth;
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

GeometryData *getDrawGeometry(SceneData *scene, DrawInfo draw)
{
    return scene->geometryPtr + scene->instancesPtr[draw.firstInstance].geometryIndex;
}

// A point cloud chunk's draw of its first count points (lib/points.slang): six indices each.
DrawInfo getPointDraw(DrawInfo draw, uint count)
{
    draw.indexCount         = POINT_INDICES * count;
    return draw;
}

// The draw with the index range of one of its LODs.
DrawInfo getLodDraw(GeometryData *geometry, DrawInfo draw, uint lod)
{
    draw.firstIndex         = geometry->lods[lod].firstIndex;
    draw.indexCount         = geometry->lods[lod].indexCount;
    return draw;
}
// clang-format on
