#pragma once

#include <array>
#include <cstddef>

#include "rasm/core/rasm.h"

namespace rasm
{
    enum class MaterialTemplate
    {
        Basic,
        PBR,
        Unlit,

        Count,
    };

    enum class PbrSlot
    {
        Albedo,
        Normal,
        Roughness,
        Metallic,
        Emissive,

        Count,
    };

    enum class PbrParam
    {
        Roughness,
        Metallic,
        EmissiveIntensity,

        Count,
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
        std::array<TextureHandle, static_cast<std::size_t>(PbrSlot::Count)> textures{};
        std::array<float, static_cast<std::size_t>(PbrParam::Count)>        scalarParams{};
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
