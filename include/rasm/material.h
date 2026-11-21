// clang-format off
#pragma once

#include "rasm/types.h"

namespace rasm
{
    enum class MaterialType
    {
        PBR,
        UNLIT,

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
    };

}
// clang-format on