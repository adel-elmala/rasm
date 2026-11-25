// clang-format off
#pragma once

#include "core.slang"

// A terrain tile (rasm/terrain.h): the shared grid of getTerrainMesh lifted by the tile's heights. The grid is the unit
// box x, z in [-0.5, 0.5], y in [0, 1]: y = 1 on the surface, 0 under the skirt. The entity's transform stretches it
// to the tile's box (getTerrainTransform), so culling sees the real bounds; here each vertex gets its height in that box.

// TerrainData::heightKind. Must match rasm::gfx::TERRAIN_HEIGHTS_*.
static const uint TERRAIN_HEIGHTS_UNORM16   = 0;    // a sample is its integer, 0 to 65535
static const uint TERRAIN_HEIGHTS_FLOAT     = 1;

struct TerrainVertex
{
    float3  position;   // object space, in the unit box
    float3  normal;     // object space: the normal matrix takes it to world space
};

// The sample's height in metres, the rule sampleTerrainHeight follows on the CPU: offset + scale * value, in floats.
float loadTerrainHeight(Sampler2D heights, TerrainData terrain, int2 sample)
{
    float value = heights.Load(int3(sample, 0)).r;
    if (terrain.heightKind == TERRAIN_HEIGHTS_UNORM16)
        value = round(value * 65535.0);
    return terrain.heightOffset + terrain.heightScale * value;
}

float3 getMatrixColumn(float4x4 m, int column)
{
    return float3(m[0][column], m[1][column], m[2][column]);
}

// vert is a grid vertex: uv[1] its point (i, j), uv[2].x the cells. Point (i, j) is sample (i + 1, j + 1); the border
// around the grid gives every point, the edges' too, the same four neighbours on both tiles of an edge, so the normals
// match there.
TerrainVertex getTerrainVertex(Sampler2D gTextures[], TerrainData terrain, float4x4 world, VertexFull vert)
{
    // Not a grid of getTerrainMesh (its vertices name no cells): the mesh as it is.
    if (vert.uv[2].x < 1.0)
    {
        TerrainVertex unchanged;
        unchanged.position  = vert.pos;
        unchanged.normal    = vert.normal;
        return unchanged;
    }
    float size      = length(getMatrixColumn(world, 0));
    float boxHeight = length(getMatrixColumn(world, 1));
    float cells     = vert.uv[2].x;
    float x         = vert.pos.x * size;    // metres from the centre
    float z         = vert.pos.z * size;
    float height    = 0.0;
    float slopeX    = 0.0;
    float slopeZ    = 0.0;
    if (terrain.heights != INVALID_TEXTURE_INDEX)
    {
        Sampler2D heights   = gTextures[terrain.heights];
        int2 sample         = int2(vert.uv[1]) + 1;
        float cell          = size / cells;
        height              = loadTerrainHeight(heights, terrain, sample);
        slopeX              = (loadTerrainHeight(heights, terrain, sample + int2(1, 0)) - loadTerrainHeight(heights, terrain, sample - int2(1, 0))) / (2.0 * cell);
        slopeZ              = (loadTerrainHeight(heights, terrain, sample + int2(0, 1)) - loadTerrainHeight(heights, terrain, sample - int2(0, 1))) / (2.0 * cell);
    }
    // The box's floor: the lowest height, lowered by the skirt and by the curvature at the corners (getTerrainTransform).
    float boxFloor = terrain.minHeight - terrain.skirt;
    if (terrain.sphereRadius > 0.0)
    {
        height  -= (x * x + z * z) / (2.0 * terrain.sphereRadius);
        slopeX  -= x / terrain.sphereRadius;
        slopeZ  -= z / terrain.sphereRadius;
        boxFloor -= size * size / (4.0 * terrain.sphereRadius);
    }
    height -= (1.0 - vert.pos.y) * terrain.skirt;

    TerrainVertex output;
    output.position = float3(vert.pos.x, (height - boxFloor) / boxHeight, vert.pos.z);
    // The normal in metres, (-slopeX, 1, -slopeZ), in the stretched box's coordinates.
    output.normal   = float3(-slopeX * size, boxHeight, -slopeZ * size);
    return output;
}

// clang-format on
