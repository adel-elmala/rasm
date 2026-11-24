#include "renderer/environmentBake.h"
#include "rasm/renderer.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace rasm
{
    namespace
    {
        // numthreads of every entry point in shaders/passes/environment.slang.
        constexpr uint32_t BAKE_GROUP_SIZE = 8;

        bool bakePipelinesReady(const Environments &environments, bool lut, bool environment)
        {
            return (!lut || environments.brdfPipeline.isValid()) &&
                   (!environment || (environments.cubePipeline.isValid() && environments.downsamplePipeline.isValid() &&
                                     environments.specularPipeline.isValid() && environments.irradiancePipeline.isValid()));
        }

        // A shader error logs and retries next frame, like the scene's pipelines, so a fix on disk is picked up. The LUT
        // needs only its own entry point, so an application without environments compiles only that one.
        bool createBakePipelines(Renderer &engine, Environments &environments, const std::string &shaderDir, bool lut, bool environment)
        {
            if (bakePipelinesReady(environments, lut, environment))
                return true;
            const std::string source = readFile(pathFromUtf8(shaderDir) / "passes/environment.slang");
            if (lut && !environments.brdfPipeline.isValid())
                environments.brdfPipeline = engine.createComputePipeline("brdfLut", source, "brdfMain");
            if (environment && !bakePipelinesReady(environments, false, true))
            {
                environments.cubePipeline = engine.createComputePipeline("environmentCube", source, "cubeMain");
                environments.downsamplePipeline = engine.createComputePipeline("environmentDownsample", source, "downsampleMain");
                environments.specularPipeline = engine.createComputePipeline("environmentSpecular", source, "specularMain");
                environments.irradiancePipeline = engine.createComputePipeline("environmentIrradiance", source, "irradianceMain");
            }
            if (bakePipelinesReady(environments, lut, environment))
                return true;
            spdlog::error("Failed to create the environment bake pipelines; the bake waits for them.");
            return false;
        }

        // One texel per thread, on every layer: 6 for a cube, 1 for the LUT.
        void dispatchBake(RenderContext &ctx, const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline, const DescriptorSetHandle &bindlessSet,
                          const gfx::EnvironmentPushConstants &pushConstants, uint32_t layers)
        {
            const uint32_t groups = (pushConstants.size + BAKE_GROUP_SIZE - 1) / BAKE_GROUP_SIZE;
            ctx.bindPipeline(commandBuffer, pipeline);
            ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessSet, 0);
            ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, groups, groups, layers);
        }

        // The cube from the image, its mips each from the one before, then the two filtered cubes from the cube. Every
        // texture starts discarded, so a bake recorded again after a dropped frame starts the same way.
        bool recordEnvironmentBake(RenderContext &ctx, const CommandBufferHandle &commandBuffer, const DescriptorSetHandle &bindlessSet, const Environments &environments,
                                   const Environment &environment)
        {
            const uint32_t cubeMips = getMipCount(environment.cubeSize);
            const uint32_t specularSlot = ENVIRONMENT_STORAGE_SLOT + cubeMips;
            const uint32_t irradianceSlot = specularSlot + SPECULAR_MIPS;
            bool ok = true;
            for (uint32_t mip = 0; mip < cubeMips; ++mip)
                ok = ctx.updateBindlessStorageImage(bindlessSet, ENVIRONMENT_STORAGE_SLOT + mip, environment.cube, mip) && ok;
            for (uint32_t mip = 0; mip < SPECULAR_MIPS; ++mip)
                ok = ctx.updateBindlessStorageImage(bindlessSet, specularSlot + mip, environment.specular, mip) && ok;
            ok = ctx.updateBindlessStorageImage(bindlessSet, irradianceSlot, environment.irradiance, 0) && ok;
            ok = ok && ctx.transitionImageLayout(commandBuffer, environment.cube, TextureUsage::STORAGE, true);
            if (!ok)
                return false;

            dispatchBake(ctx, commandBuffer, environments.cubePipeline, bindlessSet,
                         {.source = environment.sourceSlot, .target = ENVIRONMENT_STORAGE_SLOT, .size = environment.cubeSize}, CUBE_FACES);
            for (uint32_t mip = 1; mip < cubeMips; ++mip)
            {
                // Each mip reads the one the dispatch before wrote; the cube stays in STORAGE meanwhile.
                ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
                dispatchBake(ctx, commandBuffer, environments.downsamplePipeline, bindlessSet,
                             {.source = ENVIRONMENT_STORAGE_SLOT + mip - 1, .target = ENVIRONMENT_STORAGE_SLOT + mip, .size = environment.cubeSize >> mip}, CUBE_FACES);
            }

            ok = ctx.transitionImageLayout(commandBuffer, environment.cube, TextureUsage::SAMPLED) &&
                 ctx.transitionImageLayout(commandBuffer, environment.specular, TextureUsage::STORAGE, true) &&
                 ctx.transitionImageLayout(commandBuffer, environment.irradiance, TextureUsage::STORAGE, true);
            if (!ok)
                return false;
            for (uint32_t mip = 0; mip < SPECULAR_MIPS; ++mip)
            {
                const float roughness = static_cast<float>(mip) / static_cast<float>(SPECULAR_MIPS - 1);
                dispatchBake(ctx, commandBuffer, environments.specularPipeline, bindlessSet,
                             {.source = environment.cubeSlot, .target = specularSlot + mip, .roughness = roughness, .size = environment.specularSize >> mip}, CUBE_FACES);
            }
            dispatchBake(ctx, commandBuffer, environments.irradiancePipeline, bindlessSet,
                         {.source = environment.cubeSlot, .target = irradianceSlot, .size = IRRADIANCE_CUBE_SIZE}, CUBE_FACES);
            return ctx.transitionImageLayout(commandBuffer, environment.specular, TextureUsage::SAMPLED) &&
                   ctx.transitionImageLayout(commandBuffer, environment.irradiance, TextureUsage::SAMPLED);
        }
    }

    bool isBakeUsable(BakeState state)
    {
        return state == BakeState::RECORDED || state == BakeState::READY;
    }

    bool writeSceneEnvironment(const Environments &environments, const SceneEnvironment &environment, gfx::SceneData &sceneData)
    {
        sceneData.environmentSpecular = INVALID_TEXTURE_INDEX;
        sceneData.environmentIrradiance = INVALID_TEXTURE_INDEX;
        sceneData.environmentSky = INVALID_TEXTURE_INDEX;
        sceneData.brdfLut = INVALID_TEXTURE_INDEX;
        sceneData.environmentSpecularMips = 0;
        sceneData.environmentIntensity = 0.0f;
        sceneData.environmentRotation = glm::vec2(1.0f, 0.0f);

        const uint64_t index = environment.environment.index;
        if (!environment.environment.isValid() || index >= environments.environments.size() || !isBakeUsable(environments.brdfLutState))
            return false;
        const Environment &source = environments.environments[index];
        if (!isBakeUsable(source.state))
            return false;

        sceneData.environmentSpecular = source.specularSlot;
        sceneData.environmentSpecularMips = SPECULAR_MIPS;
        sceneData.environmentIrradiance = source.irradianceSlot;
        sceneData.brdfLut = environments.brdfLutSlot;
        sceneData.environmentIntensity = environment.intensity;
        sceneData.environmentRotation = glm::vec2(std::cos(environment.rotation), std::sin(environment.rotation));
        if (environment.sky)
            sceneData.environmentSky = source.cubeSlot;
        return environment.sky;
    }

    // The LUT depends on no environment, so it is made with the engine and baked in its first frame.
    bool Renderer::createEnvironments()
    {
        environments->brdfLut = ctx.createTexture({.name = "BRDF LUT", .width = BRDF_LUT_SIZE, .height = BRDF_LUT_SIZE, .format = Format::R16G16B16A16_SFLOAT, .usage = TextureUsage::SAMPLED_STORAGE});
        if (!environments->brdfLut.isValid())
        {
            spdlog::error("Failed to create the BRDF LUT.");
            return false;
        }
        environments->brdfLutSlot = addBindlessTexture(bindlessDescriptorSet, environments->brdfLut, SamplerType::LINEAR_CLAMP);
        return environments->brdfLutSlot != UINT32_MAX;
    }

    // Runs at the start of the frame, before any pass, into its first graphics batch. It bakes the LUT and at most one
    // environment per frame, since every bake writes the same storage slots, and only once the frames that last
    // wrote them are done. The textures end SAMPLED, in the states RenderContext tracks, so the passes after it and
    // the graph start from there.
    void Renderer::recordEnvironmentBakes()
    {
        ZoneScoped;

        Environments &state = *environments;
        const uint64_t completed = ctx.getFenceValue(frameFence);

        // A baked environment no longer needs its image once the frame that baked it is done.
        Environment *waiting = nullptr;
        for (Environment &environment : state.environments)
        {
            if (environment.state == BakeState::READY && environment.source.isValid() && completed >= environment.bakeValue)
            {
                ctx.destroyTexture(environment.source);
                environment.source = {};
            }
            if (!waiting && environment.state == BakeState::WAITING && ctx.isLoadUploadDone(environment.uploadValue))
                waiting = &environment;
        }

        const bool bakeLut = state.brdfLutState == BakeState::WAITING;
        if ((!bakeLut && !waiting) || completed < state.storageFreeValue || !createBakePipelines(*this, state, config.shaderDir, bakeLut, waiting != nullptr))
            return;

        const CommandBufferHandle commandBuffer = frameResources[frameCount].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "Environment Bake");
        if (bakeLut)
        {
            const bool ok = ctx.updateBindlessStorageImage(bindlessDescriptorSet, BRDF_LUT_STORAGE_SLOT, state.brdfLut, 0) &&
                            ctx.transitionImageLayout(commandBuffer, state.brdfLut, TextureUsage::STORAGE, true);
            if (ok)
                dispatchBake(ctx, commandBuffer, state.brdfPipeline, bindlessDescriptorSet, {.target = BRDF_LUT_STORAGE_SLOT, .size = BRDF_LUT_SIZE}, 1);
            state.brdfLutState = ok && ctx.transitionImageLayout(commandBuffer, state.brdfLut, TextureUsage::SAMPLED) ? BakeState::RECORDED : BakeState::FAILED;
            if (state.brdfLutState == BakeState::FAILED)
                spdlog::error("Failed to record the BRDF LUT; scenes have no environment light.");
        }
        if (waiting)
        {
            waiting->state = recordEnvironmentBake(ctx, commandBuffer, bindlessDescriptorSet, state, *waiting) ? BakeState::RECORDED : BakeState::FAILED;
            if (waiting->state == BakeState::FAILED)
                spdlog::error("Failed to record the bake of environment {}.", waiting->path);
        }
        endGpuTimer(commandBuffer, timer);
    }

    // Runs after the frame's submits. A bake whose commands went out left its last texture SAMPLED; one in a frame that
    // was dropped had its states put back (RenderContext::discardRecording) and waits for the next frame, which bakes
    // the same textures again.
    void Renderer::finishEnvironmentBakes()
    {
        Environments &state = *environments;
        const uint64_t frameValue = frameResources[frameCount].fenceValue;
        if (state.brdfLutState == BakeState::RECORDED)
        {
            const bool submitted = ctx.getTextureState(state.brdfLut) == TextureUsage::SAMPLED;
            state.brdfLutState = submitted ? BakeState::READY : BakeState::WAITING;
            if (submitted)
                state.storageFreeValue = std::max(state.storageFreeValue, frameValue);
        }
        // The frame may still write the storage slots for a bake whose environment was destroyed during it.
        if (state.bakeDestroyed)
        {
            state.storageFreeValue = frameValue;
            state.bakeDestroyed = false;
        }
        for (Environment &environment : state.environments)
        {
            if (environment.state != BakeState::RECORDED)
                continue;
            const bool submitted = ctx.getTextureState(environment.irradiance) == TextureUsage::SAMPLED;
            environment.state = submitted ? BakeState::READY : BakeState::WAITING;
            if (!submitted)
                continue;
            environment.bakeValue = frameValue;
            state.storageFreeValue = std::max(state.storageFreeValue, frameValue);
            spdlog::debug("Baked environment {}.", environment.path);
        }
    }
}
