#include "rasm/core/material.h"
#include "spdlog/spdlog.h"
#include <fstream>
#include <vector>

namespace rasm
{

    Material::Material() {}
    Material::Material(MaterialHandle materialHandle, MaterialTemplate type)
        : handle(materialHandle), type(type)
    {
        switch (type)
        {
        case MaterialTemplate::BASIC:
            shaderName = "basic";
            break;
        case MaterialTemplate::PBR:
            shaderName = "pbr";
            break;
        case MaterialTemplate::UNLIT:
            shaderName = "unlit";
            break;
        case MaterialTemplate::SHADER:
            shaderName = "shader";
            break;

        default:
            spdlog::error("Unknown material template type.");
            break;
        }
    }
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

    void Material::setShaderSource(const std::string &source)
    {
        shaderSource = source;
    }

    void Material::setShaderName(const std::string &name)
    {
        shaderName = name;
    }

    std::string Material::getShaderSources()
    {
        switch (this->type)
        {
        case MaterialTemplate::BASIC:
        case MaterialTemplate::PBR:
        case MaterialTemplate::UNLIT:
        {
            auto filePath = "./shaders/common/test.slang";
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                spdlog::error("Failed to open shader file: {}", filePath);
                return "";
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }
        case MaterialTemplate::SHADER:
            return shaderSource;
        default:
        {
            spdlog::error("Unknown material template type for shader retrieval.");
            return {};
        }
        }
    }

    std::string Material::getShaderName()
    {
        return shaderName;
    }

}
