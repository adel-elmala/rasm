// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/material.h"
#include "rasm/transform.h"

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include <cstdint>

namespace rasm
{

    // Cells per side of a terrain grid: 1 to MAX_TERRAIN_CELLS. A tile's heights have cells + 3 samples per side.
    constexpr uint32_t          MAX_TERRAIN_CELLS       = 256;

    // A terrain tile: a square of its own frame (x and z across, y up; on a globe its east-north-up frame), lifted by its
    // heights. Renderer::createTerrainTile makes its material and its entity on the shared grid of getTerrainMesh.
    struct TerrainTileDesc
    {
        glm::dvec3                  center          = glm::dvec3(0.0);  // world space: the middle of the square, at height 0 of its frame
        glm::quat                   orientation     = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // the frame's axes in world space
        double                      size            = 0.0;              // metres across, along x and z
        TerrainMaterial             terrain         = {};
        TextureHandle               imagery;                            // the base color; invalid: white
        TextureTransform            imageryRect     = {};               // where the tile is in the imagery (a parent's image: a quarter of it)
        float                       roughness       = 0.9f;
    };

    struct TerrainTile
    {
        EntityHandle                entity;                             // not in a scene yet
        MaterialHandle              material;
    };

    // A tile's height samples on the CPU, the same as its heights texture: width x width, row by row (row j is z).
    struct TerrainHeights
    {
        const uint16_t*             unorm16         = nullptr;          // R16_UNORM samples, or
        const float*                floats          = nullptr;          // R32_SFLOAT samples
        uint32_t                    width           = 0;                // cells + 3
    };

    // The tile's entity transform: its box from minHeight to maxHeight (lowered by the curvature at its corners), which
    // the shared grid's unit box fills, so culling sees the real bounds.
    Transform                       getTerrainTransform(const TerrainTileDesc& tile);
    // The height in metres in the tile's frame, before the curvature, at (u, v) from 0 to 1 across the tile: the rule the
    // shader draws (each cell two triangles, split from (0, 1) to (1, 0); a corner is heightOffset + heightScale *
    // sample in floats). Safe on any thread.
    double                          sampleTerrainHeight(const TerrainHeights& heights, const TerrainMaterial& terrain, double u, double v);
    // The world position of the drawn surface at (u, v), the curvature included: to place objects and cameras on the
    // ground, and to check a picked position.
    glm::dvec3                      getTerrainPosition(const TerrainTileDesc& tile, const TerrainHeights& heights, double u, double v);

}
// clang-format on
