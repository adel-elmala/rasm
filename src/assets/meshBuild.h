// clang-format off
#pragma once

#include "assets/meshData.h"
#include "rasm/mesh.h"

#include <vector>

namespace rasm
{

    // What buildMesh made: the geometry with its LODs and meshlets, and a vector mesh's labels, a point cloud chunk's order.
    struct MeshBuild::Data
    {
        ObjRaw                      raw;
        std::vector<MeshLabel>      labels;
        std::vector<uint16_t>       pointOrder;
    };

}
// clang-format on
