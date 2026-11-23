// clang-format off
#pragma once

#include "rasm/mesh.h"

#include <cstdint>

namespace rasm
{
    constexpr uint32_t  MESHLET_MAX_VERTICES    = 64;
    constexpr uint32_t  MESHLET_MAX_TRIANGLES   = 124;
    constexpr uint32_t  MIN_MESHLET_TRIANGLES   = 1024;     // about 8 meshlets, cull.slang's MESHLET_MIN_DRAW: a smaller sub-mesh is culled and drawn whole

    void                buildMeshlets(ObjRaw& raw);
}
// clang-format on
