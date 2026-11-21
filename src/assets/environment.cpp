#include "rasm/engine.h"
#include "renderer/environmentBake.h"

#include "glm/gtc/packing.hpp"
#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "stb_image.h"

#include <algorithm>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace rasm
{
    namespace
    {
        // The largest finite half float. Brighter texels (the sun's core) would become infinity.
        constexpr float MAX_HALF = 65504.0f;

        // Runs on a worker thread: it must not touch the engine. RGBA16F halves the memory of the floats stb_image
        // gives, and is what the bake reads.
        EnvironmentRaw decodeEnvironment(const std::string &path)
        {
            ZoneScoped;

            int width, height, channels;
            float *data = stbi_loadf(path.c_str(), &width, &height, &channels, 4);
            if (!data)
                return {};
            EnvironmentRaw raw{.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)};
            const size_t count = static_cast<size_t>(width) * height * 4;
            raw.pixels.resize(count);
            for (size_t i = 0; i < count; ++i)
                raw.pixels[i] = glm::packHalf1x16(std::clamp(data[i], 0.0f, MAX_HALF));
            stbi_image_free(data);
            return raw;
        }
    }

    EnvironmentHandle Engine::loadEnvironment(const std::string &assetPath)
    {
        const std::string path = resolveAssetPath(assetPath);
        const auto extension = path.substr(path.find_last_of('.') + 1);
        if (extension != "hdr")
        {
            spdlog::error("loadEnvironment reads Radiance .hdr images, not .{}: {}", extension, path);
            return {};
        }
        // Only the header is read here, so a missing or broken file fails at once.
        int width, height, channels;
        if (!stbi_info(path.c_str(), &width, &height, &channels) || !stbi_is_hdr(path.c_str()))
        {
            spdlog::error("Failed to read the environment image {}", path);
            return {};
        }

        const EnvironmentHandle handle = handleManager.getNextEnvironmentHandle();
        environments->environments.resize(handle.index + 1);
        environments->environments[handle.index] = Environment{.path = path};
        pendingEnvironments.push_back({handle, path, std::async(std::launch::async, decodeEnvironment, path)});
        return handle;
    }

    bool Engine::isLoaded(EnvironmentHandle environment) const
    {
        if (!environment.isValid() || environment.index >= environments->environments.size())
            return false;
        return isBakeUsable(environments->environments[environment.index].state);
    }

    // Makes the image and the three cubes, so the next frame can bake them. A failure leaves the environment without
    // light, as if the scene had none.
    void Engine::finishEnvironmentLoad(PendingEnvironmentLoad &load)
    {
        ZoneScoped;

        EnvironmentRaw raw = load.result.get();
        Environment &environment = environments->environments[load.handle.index];
        environment.state = BakeState::FAILED;
        if (raw.pixels.empty())
        {
            spdlog::error("Failed to decode the environment image {}", load.path);
            return;
        }

        environment.cubeSize = getEnvironmentCubeSize(raw.width);
        environment.specularSize = std::min(SPECULAR_CUBE_SIZE, environment.cubeSize);
        environment.source = ctx.createTexture({.name = load.path, .width = raw.width, .height = raw.height, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED});
        environment.cube = ctx.createTexture({.name = load.path + " cube", .width = environment.cubeSize, .height = environment.cubeSize, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED_STORAGE, .cube = true, .mipLevels = getMipCount(environment.cubeSize)});
        environment.specular = ctx.createTexture({.name = load.path + " specular", .width = environment.specularSize, .height = environment.specularSize, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED_STORAGE, .cube = true, .mipLevels = SPECULAR_MIPS});
        environment.irradiance = ctx.createTexture({.name = load.path + " irradiance", .width = IRRADIANCE_CUBE_SIZE, .height = IRRADIANCE_CUBE_SIZE, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED_STORAGE, .cube = true, .mipLevels = 1});
        const bool created = environment.source.isValid() && environment.cube.isValid() && environment.specular.isValid() && environment.irradiance.isValid() &&
                             ctx.fillTexture(environment.source, raw.pixels.data());
        if (created)
        {
            // The image wraps around in u, so it repeats; the cubes are seamless, and clamp keeps mips from wrapping.
            environment.sourceSlot = addBindlessTexture(bindlessDescriptorSet, environment.source, SamplerType::LINEAR_REPEAT);
            environment.cubeSlot = addBindlessTexture(bindlessDescriptorSet, environment.cube, SamplerType::LINEAR_CLAMP);
            environment.specularSlot = addBindlessTexture(bindlessDescriptorSet, environment.specular, SamplerType::LINEAR_CLAMP);
            environment.irradianceSlot = addBindlessTexture(bindlessDescriptorSet, environment.irradiance, SamplerType::LINEAR_CLAMP);
        }
        if (!created || environment.sourceSlot == UINT32_MAX || environment.cubeSlot == UINT32_MAX || environment.specularSlot == UINT32_MAX ||
            environment.irradianceSlot == UINT32_MAX)
        {
            spdlog::error("Failed to create the textures of environment {}", load.path);
            for (TextureHandle *texture : {&environment.source, &environment.cube, &environment.specular, &environment.irradiance})
            {
                if (texture->isValid())
                    ctx.destroyTexture(*texture);
                *texture = {};
            }
            return;
        }
        environment.state = BakeState::WAITING;
        spdlog::info("Loaded environment {} ({}x{}, cube {}).", load.path, raw.width, raw.height, environment.cubeSize);
    }
}
