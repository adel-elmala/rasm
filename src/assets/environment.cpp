#include "rasm/renderer.h"
#include "renderer/environmentBake.h"
#include "renderer/loadHooks.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace rasm
{
    bool Renderer::isLoaded(EnvironmentHandle environment) const
    {
        if (!isLive(environment))
            return false;
        return isBakeUsable(environments->environments[environment.index].state);
    }

    // Scenes that use it have no environment from the next frame on. Its textures and their bindless slots are freed once
    // the frames in flight, which may still light with them or bake them, are done.
    void Renderer::destroyEnvironment(EnvironmentHandle environment)
    {
        if (!isLive(environment))
        {
            spdlog::error("destroyEnvironment: environment handle {{{}, {}}} is invalid or was destroyed.", environment.index, environment.generation);
            return;
        }
        for (Scene &scene : registry.scenes)
        {
            if (scene.environment.environment == environment)
                scene.environment.environment = {};
        }
        // The loader drops a load still in its pool.
        if (loads)
            loads->forget(environment);
        Environment &entry = environments->environments[environment.index];
        // A bake recorded in the frame being made still writes the storage slots: none is rewritten before it is done.
        if (entry.state == BakeState::RECORDED)
        {
            environments->bakeDestroyed = true;
            environments->storageFreeValue = UINT64_MAX;
        }
        for (TextureHandle texture : {entry.source, entry.cube, entry.specular, entry.irradiance})
        {
            if (ctx.isAlive(texture))
                ctx.destroyTexture(texture);
        }
        entry = {};
        entry.state = BakeState::FAILED;
        registry.environmentHandles.release(environment.index, environment.generation);
    }

    // Makes the image and the three cubes, so the next frame can bake them. A failure leaves the environment without
    // light, as if the scene had none.
    // An environment that waits for its image: the loader's, decoded on a worker (setEnvironmentImage).
    EnvironmentHandle Renderer::createEnvironment(const std::string &name)
    {
        const HandleSlot slot = registry.environmentHandles.allocate();
        const EnvironmentHandle handle{slot.index, slot.generation};
        environments->environments.resize(registry.environmentHandles.size());
        environments->environments[handle.index] = Environment{.path = name};
        return handle;
    }

    bool Renderer::setEnvironmentImage(EnvironmentHandle handle, const std::string &name, const EnvironmentRaw &raw)
    {
        ZoneScoped;

        if (!isLive(handle))
            return false;
        Environment &environment = environments->environments[handle.index];
        environment.state = BakeState::FAILED;
        if (raw.pixels.empty())
        {
            spdlog::error("Failed to decode the environment image {}", name);
            return false;
        }

        environment.cubeSize = getEnvironmentCubeSize(raw.width);
        environment.specularSize = std::min(SPECULAR_CUBE_SIZE, environment.cubeSize);
        environment.source = ctx.createTexture({.name = name, .width = raw.width, .height = raw.height, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED});
        environment.cube = ctx.createTexture({.name = name + " cube", .width = environment.cubeSize, .height = environment.cubeSize, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED_STORAGE, .cube = true, .mipLevels = getMipCount(environment.cubeSize)});
        environment.specular = ctx.createTexture({.name = name + " specular", .width = environment.specularSize, .height = environment.specularSize, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED_STORAGE, .cube = true, .mipLevels = SPECULAR_MIPS});
        environment.irradiance = ctx.createTexture({.name = name + " irradiance", .width = IRRADIANCE_CUBE_SIZE, .height = IRRADIANCE_CUBE_SIZE, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED_STORAGE, .cube = true, .mipLevels = 1});
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
            spdlog::error("Failed to create the textures of environment {}", name);
            for (TextureHandle *texture : {&environment.source, &environment.cube, &environment.specular, &environment.irradiance})
            {
                if (texture->isValid())
                    ctx.destroyTexture(*texture);
                *texture = {};
            }
            return false;
        }
        environment.state = BakeState::WAITING;
        spdlog::info("Loaded environment {} ({}x{}, cube {}).", name, raw.width, raw.height, environment.cubeSize);
        return true;
    }
}
