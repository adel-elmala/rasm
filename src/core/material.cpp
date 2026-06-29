#include "rasm/core/material.h"
#include "spdlog/spdlog.h"
#include <fstream>
#include <vector>

namespace rasm
{

    Material::Material() {}
    Material::Material(MaterialHandle materialHandle, MaterialTemplate type)
        : handle(materialHandle), type(type) {}
    Material::~Material() {}

    MaterialHandle Material::id() const
    {
        return handle;
    }

    void Material::setTexture(PbrSlot slot, TextureHandle texture)
    {
        const std::size_t index = toIndex(slot);
        if (index >= textures.size())
        {
            spdlog::error("PbrSlot enum value exceeds textures array size.");
            return;
        }

        textures[index] = texture;
    }
    void Material::setFloat(PbrParam param, float value)
    {
        const std::size_t index = toIndex(param);
        if (index >= scalarParams.size())
        {
            spdlog::error("PbrParam enum value exceeds scalarParams array size.");
            return;
        }

        scalarParams[index] = value;
    }

    std::string Material::getShaderSources(const MaterialHandle &handle, MaterialTemplate type)
    {
        switch (type)
        {
        case MaterialTemplate::BASIC:
        case MaterialTemplate::PBR:
        case MaterialTemplate::UNLIT:
        case MaterialTemplate::SHADER:
            return "./shaders/common/test.slang";
        default:
        {
            spdlog::error("Unknown material template type for shader retrieval.");
            return {};
        }
        }
    }

}
