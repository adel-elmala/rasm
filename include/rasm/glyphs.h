// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/mesh.h"

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include <cstdint>

namespace rasm
{

    constexpr uint32_t MAX_GLYPH_CHUNK_GLYPHS = 1u << 16;

    struct GlyphChunkDesc
    {
        MeshHandle          shape;
        const glm::vec3*    positions       = nullptr;
        const glm::quat*    orientations    = nullptr;
        const glm::vec3*    scales          = nullptr;
        const uint32_t*     colors          = nullptr;
        const float*        scalars         = nullptr;
        uint32_t            count           = 0;
    };

    enum class GlyphShape : uint32_t
    {
        ARROW,
        SPHERE,
        CUBE,
    };

    MeshBuild           buildGlyphChunk(const GlyphChunkDesc& desc);
    MeshBuild           buildGlyphShape(GlyphShape shape, uint32_t segments = 16);
    glm::quat           getGlyphOrientation(const glm::vec3& direction);

}
// clang-format on
