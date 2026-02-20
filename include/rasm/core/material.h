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
        Unlit
    };

    enum class PbrSlot
    {
        Albedo,
        Normal,
        Roughness,
        Metallic,
        Emissive
    };

    enum class PbrParam
    {
        Roughness,
        Metallic,
        EmissiveIntensity
    };

    class Material
    {
    private:
        MaterialHandle handle{};
        std::array<TextureHandle, 5> textures{};
        std::array<float, 3> scalarParams{};

        static constexpr std::size_t toIndex(PbrSlot slot) {
            return static_cast<std::size_t>(slot);
        }
        static constexpr std::size_t toIndex(PbrParam param) {
            return static_cast<std::size_t>(param);
        }

    public:
        Material();
        explicit Material(MaterialHandle materialHandle);
        ~Material();

        [[nodiscard]] MaterialHandle id() const;
        void setTexture(PbrSlot slot, TextureHandle texture);
        void setFloat(PbrParam param, float value);
    };

}
