// clang-format off
#pragma once

#include "rasm/grid.h"
#include "renderer/uberMaterialTypes.h"

namespace rasm
{

    bool                checkGrid(const Grid& grid);
    gfx::GridData       makeGridData(const Grid& grid, const glm::dvec3& origin);

}
// clang-format on
