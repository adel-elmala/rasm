// clang-format off
#pragma once

#include <array>
#include <cstddef>

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
        NORMAL,
        ROUGHNESS,
        METALLIC,
        EMISSIVE,

        COUNT,
    };

    enum class PbrParam
    {
        ROUGHNESS,
        METALLIC,
        EMISSIVEINTENSITY,

        COUNT,
    };


    constexpr uint32_t MAX_TEXTURE_SLOTS = 8;

    struct Material
    {
        const char*    name{};
        MaterialHandle handle{};
        MaterialType   type{};
        TextureHandle  textures[MAX_TEXTURE_SLOTS];
        ShaderHandle   shader{};
    };

}
// clang-format on