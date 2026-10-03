// clang-format off
#pragma once

#include "rasm/core/types.h"

namespace rasm
{
    enum class MaterialType
    {
        BASIC,
        PBR,
        UNLIT,
        SHADER,

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
        ALBEDO              = 0,      // vec3
        METALLIC            = 3,      // float
        ROUGHNESS           = 4,      // float
        NORMAL_SCALE        = 5,      // float
        OCCLUSION_STRENGTH  = 6,      // float
        EMISSIVE_FACTOR      = 7,      // vec3

        COUNT = 10,
    };

    constexpr uint32_t MAX_TEXTURE_SLOTS = static_cast<uint32_t>(PbrSlot::COUNT);
    constexpr uint32_t MAX_PARAM_SLOTS   = static_cast<uint32_t>(PbrParam::COUNT);

    struct Material
    {
        const char*    name{};
        MaterialHandle handle{};
        MaterialType   type{};
        TextureHandle  textures[MAX_TEXTURE_SLOTS];
        float          params[MAX_PARAM_SLOTS];
        ShaderHandle   shader{};
    };

}
// clang-format on