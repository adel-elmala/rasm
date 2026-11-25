// clang-format off
#pragma once

#include "rasm/types.h"

namespace rasm
{
    enum class MaterialType
    {
        PBR,
        UNLIT,
        TERRAIN,        // a terrain tile: the shared grid lifted by Material::terrain's heights, shaded as PBR (rasm/terrain.h)

        COUNT,
    };

    enum class PbrSlot
    {
        ALBEDO,
        METALLIC,
        ROUGHNESS,
        NORMAL,
        OCCLUSION,
        EMISSIVE,

        COUNT,
    };

    enum class PbrParam : uint32_t
    {
        ALBEDO              = 0,      // vec4
        METALLIC            = 4,      // float
        ROUGHNESS           = 5,      // float
        NORMAL_SCALE        = 6,      // float
        OCCLUSION_STRENGTH  = 7,      // float
        EMISSIVE_FACTOR     = 8,      // vec3
        EMISSIVE_STRENGTH   = 11,     // float

        COUNT = 12,
    };

    enum class AlphaMode : uint32_t
    {
        NONE,
        MASK,
        BLEND,
    };

    struct TextureTransform
    {
        glm::vec2      offset       = glm::vec2(0.0f);
        float          rotation     = 0.0f;
        glm::vec2      scale        = glm::vec2(1.0f);
    };

    // A TERRAIN material's heights. A height in metres is heightOffset + heightScale * sample, where an R16_UNORM sample is
    // its integer (0 to 65535) and an R32_SFLOAT sample its value. Neighbouring tiles get bit-equal heights on their
    // shared edge when heightScale is a power of two and heightOffset a multiple of it (rasm/terrain.h).
    struct TerrainMaterial
    {
        TextureHandle  heights;                     // R16_UNORM or R32_SFLOAT, (cells + 3) x (cells + 3): the grid's points and a border of one
        float          heightScale  = 1.0f;
        float          heightOffset = 0.0f;
        float          minHeight    = 0.0f;         // metres: the lowest and highest heights, the tile's box (getTerrainTransform)
        float          maxHeight    = 0.0f;
        float          skirt        = 0.0f;         // metres the skirt hangs below the tile's edges, to hide cracks between levels
        double         sphereRadius = 0.0;          // 0: flat; else the tile bends down by d^2 / 2R at d metres from its centre
    };

    constexpr uint32_t MAX_TEXTURE_SLOTS = static_cast<uint32_t>(PbrSlot::COUNT);
    constexpr uint32_t MAX_PARAM_SLOTS   = static_cast<uint32_t>(PbrParam::COUNT);

    struct Material
    {
        const char*      name{};
        MaterialHandle   handle{};
        MaterialType     type{};
        TextureHandle    textures[MAX_TEXTURE_SLOTS];
        uint32_t         texCoords[MAX_TEXTURE_SLOTS];
        TextureTransform textureTransforms[MAX_TEXTURE_SLOTS];
        float            params[MAX_PARAM_SLOTS];
        bool             doubleSided{};
        AlphaMode        alphaMode{};
        float            alphaCutoff  = 0.5f;
        TerrainMaterial  terrain      = {};         // TERRAIN only; textures[ALBEDO] is the imagery, textureTransforms[ALBEDO] its part of the image
    };

}
// clang-format on