#include "rasm/core/material.h"
#include "rasm/core/engine.h"
#include "rasm/core/utils.h"

#include "spdlog/spdlog.h"

#include <fstream>
#include <vector>

namespace rasm
{

    MaterialHandle Engine::createMaterial(MaterialType type, TextureHandle textures[], ShaderHandle shader)
    {
        if (type == MaterialType::SHADER && !shader.isValid())
        {
            spdlog::error("Invalid shader handle provided for custom shader material.");
            return {};
        }

        MaterialHandle handle = getNextMaterialHandle();
        if (handle.index >= MAX_MATERIALS)
        {
            spdlog::error("Exceeded maximum number of materials.");
            return {};
        }

        auto material = Material{.handle = handle, .type = type, .textures = {}, .shader = shader};
        if (textures)
        {
            for (uint32_t i = 0; i < MAX_TEXTURE_SLOTS; ++i)
            {
                material.textures[i] = textures[i];
            }
        }

        switch (type)
        {
        case MaterialType::BASIC:
            material.name = "basic";
            material.shader = createShader(readFile("./shaders/common/basic.slang"));
            break;
        case MaterialType::PBR:
            material.name = "pbr";
            material.shader = createShader(readFile("./shaders/common/pbr.slang"));
            break;
        case MaterialType::UNLIT:
            material.name = "unlit";
            material.shader = createShader(readFile("./shaders/common/unlit.slang"));
            break;
        case MaterialType::SHADER:
            material.name = "shader";
            break;

        default:
            spdlog::error("Unknown material template type.");
            break;
        }

        registery.materials.resize(handle.index + 1);
        registery.materials[handle.index] = material;

        return handle;
    }

    ShaderHandle Engine::createShader(const std::string &shaderSource)
    {
        ShaderHandle handle = getNextShaderHandle();
        if (handle.index >= MAX_SHADERS)
        {
            spdlog::error("Exceeded maximum number of shaders.");
            return {};
        }

        registery.shaders.resize(handle.index + 1);
        registery.shaders[handle.index] = shaderSource;

        return handle;
    }
}
