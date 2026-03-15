#pragma once

#include <array>
#include <cstddef>

#include "rasm/core/rasm.h"

namespace rasm
{
    enum class MaterialTemplate
    {
        BASIC,
        PBR,
        UNLIT,

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

    class Material
    {

    public:
                        Material();
        explicit        Material(MaterialHandle materialHandle);
                        ~Material();
        [[nodiscard]]   MaterialHandle id() const;
        void            setTexture(PbrSlot slot, TextureHandle texture);
        void            setFloat(PbrParam param, float value);

    private:
        std::array<TextureHandle, static_cast<std::size_t>(PbrSlot::COUNT)> textures{};
        std::array<float, static_cast<std::size_t>(PbrParam::COUNT)>        scalarParams{};
        MaterialHandle                                                      handle{};

        static constexpr std::size_t toIndex(PbrSlot slot)
        {
            return static_cast<std::size_t>(slot);
        }
        static constexpr std::size_t toIndex(PbrParam param)
        {
            return static_cast<std::size_t>(param);
        }
    };

}
