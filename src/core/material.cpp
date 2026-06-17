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

    std::pair<std::vector<char>, std::vector<char>> Material::getShaderSources(const MaterialHandle &handle, MaterialTemplate type)
    {
        switch (type)
        {
        case MaterialTemplate::BASIC:
        case MaterialTemplate::PBR:
        case MaterialTemplate::UNLIT:
        {
            std::string vertPath = "assets/shaders/build/basic_vert.spv";
            std::string fragPath = "assets/shaders/build/basic_frag.spv";

            std::ifstream vertFile(vertPath, std::ios::binary | std::ios::ate);
            if (!vertFile)
            {
                spdlog::error("Failed to open vertex shader file: {}", vertPath);
                return {};
            }
            std::ifstream fragFile(fragPath, std::ios::binary | std::ios::ate);
            if (!fragFile)
            {
                spdlog::error("Failed to open fragment shader file: {}", fragPath);
                return {};
            }

            std::streamsize vertSize = vertFile.tellg();
            vertFile.seekg(0, std::ios::beg);
            std::vector<char> vertBuffer(vertSize);

            if (!vertFile.read(vertBuffer.data(), vertSize))
            {
                spdlog::error("Failed to read vertex shader file: {}", vertPath);
                return {};
            }

            std::streamsize fragSize = fragFile.tellg();
            fragFile.seekg(0, std::ios::beg);
            std::vector<char> fragBuffer(fragSize);

            if (!fragFile.read(fragBuffer.data(), fragSize))
            {
                spdlog::error("Failed to read fragment shader file: {}", fragPath);
                return {};
            }
            return {vertBuffer, fragBuffer};
        }
        case MaterialTemplate::SHADER:
        {

            return {};
            break;
        }
        default:
        {
            spdlog::error("Unknown material template type for shader retrieval.");
            return {};
        }
        }
    }

}
