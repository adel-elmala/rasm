// clang-format off
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

    class Material
    {

    public:
                        Material();
        explicit        Material(MaterialHandle materialHandle, MaterialTemplate type);
                        ~Material();
        [[nodiscard]]   MaterialHandle id() const;
        void            setTexture(PbrSlot slot, TextureHandle texture);
        void            setFloat(PbrParam param, float value);
        static std::string getShaderSources(const MaterialHandle& handle, MaterialTemplate type);

    private:
        std::array<TextureHandle, static_cast<std::size_t>(PbrSlot::COUNT)> textures{};
        std::array<float, static_cast<std::size_t>(PbrParam::COUNT)>        scalarParams{};
        MaterialHandle                                                      handle{};
        MaterialTemplate                                                    type{};

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
// clang-format on