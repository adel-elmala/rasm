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
        const uint32_t*     colors      = nullptr;      // RGB8 in RGBA8, sRGB-encoded (packVertexColor), alpha ignored; null: none
        const float*        scalars     = nullptr;      // null: none
        uint32_t            count       = 0;
        float               spacing     = 0.0f;         // between neighbouring points, for PointSizeMode::ADAPTIVE; 0: measured
    };

    struct PointChunkLod
    {
        float               drawn       = 1.0f;         // the share of its points drawn
        float               finer[8]    = {};           // per octant of its entity's space (x >= 0: 1, y >= 0: 2, z >= 0: 4): a finer chunk's drawn share there
    };

    MeshBuild           buildPointCloudChunk(const PointCloudChunkDesc& desc);

}
// clang-format on
