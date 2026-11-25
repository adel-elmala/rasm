// clang-format off
#pragma once

#include "rasm/mesh.h"

#include <cstdint>
#include <vector>

namespace rasm
{

    // The shared grid of cells x cells: (cells + 1)^2 surface vertices at y = 1 and a skirt of y = 0 under each edge,
    // so the bounds are the unit box x, z in [-0.5, 0.5], y in [0, 1]. uv[0] is the point's (u, v), uv[1] its (i, j),
    // uv[2].x the cells. Each cell is two triangles split from (i, j + 1) to (i + 1, j), counter-clockwise from above.
    void                            buildTerrainGrid(uint32_t cells, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);

}
// clang-format on
