#pragma once

#include <stdint.h>

#include "rasm/core/rasm.h"

namespace rasm
{

    typedef uint32_t TextureHandle;

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
    public:
        Material();
        ~Material();

        void setTexture(PbrSlot slot, TextureHandle texture);
        void setFloat(PbrParam param, float value);
    };

}
