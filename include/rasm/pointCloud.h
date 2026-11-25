// clang-format off
#pragma once

#include "rasm/mesh.h"

#include "glm/glm.hpp"

#include <cstdint>

namespace rasm
{

    constexpr uint32_t MAX_POINT_CHUNK_POINTS = 1u << 16;

    struct PointCloudChunkDesc
    {
        const glm::vec3*    positions   = nullptr;      // offsets from the entity's origin
        const uint32_t*     colors      = nullptr;      // RGBA8, RGB sRGB-encoded (packVertexColor); null: none
        const float*        scalars     = nullptr;      // null: none
        uint32_t            count       = 0;
    };

    MeshBuild           buildPointCloudChunk(const PointCloudChunkDesc& desc);

}
// clang-format on
