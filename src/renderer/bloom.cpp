#include "renderer/bloom.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <memory>
#include <string>

namespace rasm
{
    namespace
    {
        // The chain's storage targets: down[i] is level i + 1, up[i] the up chain's level i + 1. The last level has no
        // up target: the up chain starts from it.
        struct BloomChain
        {
            RenderTargetHandle down[BLOOM_LEVELS];
            RenderTargetHandle up[BLOOM_LEVELS - 1];
        };
    }

    namespace
    {
        // numthreads of every entry point in shaders/passes/bloom.slang.
        constexpr uint32_t BLOOM_GROUP_SIZE = 8;

        void dispatchBloom(RenderContext &ctx, const CommandBufferHandle &commandBuffer, const PipelineHandle &pipeline, const DescriptorSetHandle &bindlessSet,
                           const gfx::BloomPushConstants &pushConstants, const RenderTarget &target)
        {
            ctx.bindPipeline(commandBuffer, pipeline);
            ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessSet, 0);
            ctx.pushConstants(commandBuffer, pipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, (target.width + BLOOM_GROUP_SIZE - 1) / BLOOM_GROUP_SIZE, (target.height + BLOOM_GROUP_SIZE - 1) / BLOOM_GROUP_SIZE);
        }
    }

    glm::uvec2 getBloomLevelSize(glm::uvec2 sourceSize, uint32_t level)
    {
        return glm::max(sourceSize >> glm::uvec2(level), glm::uvec2(1));
    }

    uint32_t getBloomLevelCount(const glm::uvec2 *levelSizes, uint32_t count)
    {
        uint32_t levels = 0;
        while (levels < count && glm::all(glm::greaterThanEqual(levelSizes[levels], glm::uvec2(BLOOM_MIN_LEVEL_SIZE))))
            ++levels;
        return std::max(levels, std::min(count, 1u));
    }

    bool Engine::bloomPipelinesReady() const
    {
        return bloomFirstDownPipeline.isValid() && bloomDownPipeline.isValid() && bloomUpPipeline.isValid();
    }

    // The bloom passes dispatch, and the tone map samples their result, only when this holds. The first bloom pass
    // makes the pipelines before it asks, and the strength does not change within a frame.
    bool Engine::bloomRuns(CameraHandle camera) const
    {
        return registry.cameras[camera.index].bloomStrength > 0.0f && bloomPipelinesReady();
    }

    // A shader error logs and retries the next frame, like the tone map, so a fix on disk is picked up.
    bool Engine::createBloomPipelines()
    {
        if (bloomPipelinesReady())
            return true;
        const std::string source = readFile(pathFromUtf8(config.shaderDir) / "passes/bloom.slang");
        if (!bloomFirstDownPipeline.isValid())
            bloomFirstDownPipeline = createComputePipeline("bloomFirstDownsample", source, "firstDownsampleMain");
        if (!bloomDownPipeline.isValid())
            bloomDownPipeline = createComputePipeline("bloomDownsample", source, "downsampleMain");
        if (!bloomUpPipeline.isValid())
            bloomUpPipeline = createComputePipeline("bloomUpsample", source, "upsampleMain");
        if (bloomPipelinesReady())
            return true;
        spdlog::error("Failed to create the bloom pipelines; the frame has no bloom until they compile.");
        return false;
    }

    // Down passes 1 to BLOOM_LEVELS halve the source again and again; up passes BLOOM_LEVELS - 1 to 1 walk back, each
    // adding the tent-blurred level below to its own level of the down chain. Up 1 divides by the levels it added, so
    // the bloom has the source's energy. A small target runs fewer levels (getBloomLevelCount), which the first pass
    // finds from the targets' sizes every frame, since a resize changes them without a new graph, and shares with the
    // others. Every target is transient storage, sized from the source, so the graph lays their memory out together.
    // The passes share the GPU timer "Bloom", and dispatch nothing while the camera's bloom strength is 0 (the graph
    // still records their barriers).
    RenderTargetHandle Engine::addBloomPasses(RenderGraph &graph, const RenderTargetHandle &source, CameraHandle camera)
    {
        if (!camera.isValid() || camera.index >= registry.cameras.size())
        {
            spdlog::error("addBloomPasses needs a valid camera handle.");
            return {};
        }
        if (!source.isValid() || ctx.getRenderTarget(source).colorCount == 0)
        {
            spdlog::error("addBloomPasses needs a render target with a color attachment.");
            return {};
        }
        if (ctx.getRenderTarget(source).samples > 1)
        {
            spdlog::error("addBloomPasses samples one value per pixel: resolve a multisampled target first (addMsaaResolvePass).");
            return {};
        }

        const RenderTargetDesc sourceDesc = ctx.getRenderTargetDesc(source);
        BloomChain chain;
        auto createLevel = [&](const std::string &name, uint32_t level)
        {
            RenderTargetDesc desc = {.name = name + std::to_string(level), .colorFormats = {SCENE_COLOR_FORMAT}, .storage = true};
            if (sourceDesc.swapchainScale > 0.0f)
                desc.swapchainScale = sourceDesc.swapchainScale / static_cast<float>(1u << level);
            else
            {
                const glm::uvec2 size = getBloomLevelSize({sourceDesc.width, sourceDesc.height}, level);
                desc.width = size.x;
                desc.height = size.y;
            }
            return graph.createRenderTarget(desc);
        };
        for (uint32_t i = 0; i < BLOOM_LEVELS; ++i)
        {
            chain.down[i] = createLevel("bloom_down_", i + 1);
            if (i + 1 < BLOOM_LEVELS)
                chain.up[i] = createLevel("bloom_up_", i + 1);
            if (!chain.down[i].isValid() || (i + 1 < BLOOM_LEVELS && !chain.up[i].isValid()))
            {
                spdlog::error("addBloomPasses could not create the bloom's render targets.");
                return {};
            }
        }

        // How many levels this frame's chain has, 0 when it does not run: Bloom Down 1 finds it, after it makes the
        // pipelines, and the other passes read it.
        const auto activeLevels = std::make_shared<uint32_t>(0);
        auto findLevels = [this, chain, camera]() -> uint32_t
        {
            createBloomPipelines();
            if (!bloomRuns(camera))
                return 0;
            glm::uvec2 sizes[BLOOM_LEVELS];
            for (uint32_t i = 0; i < BLOOM_LEVELS; ++i)
            {
                const RenderTarget &level = ctx.getRenderTarget(chain.down[i]);
                sizes[i] = {level.width, level.height};
            }
            return getBloomLevelCount(sizes, BLOOM_LEVELS);
        };

        for (uint32_t level = 1; level <= BLOOM_LEVELS; ++level)
        {
            const RenderTargetHandle input = level == 1 ? source : chain.down[level - 2];
            const RenderTargetHandle output = chain.down[level - 1];
            graph.addComputePass(
                "Bloom Down " + std::to_string(level),
                [input, output](PassBuilder &builder)
                {
                    builder.read(input);
                    builder.write(output);
                },
                [this, input, output, level, activeLevels, findLevels](RenderContext &)
                {
                    if (level == 1)
                        *activeLevels = findLevels();
                    if (level > *activeLevels)
                        return;
                    const gfx::BloomPushConstants pushConstants{.source = getBindlessTextureIndex(getRenderTargetTexture(input)),
                                                                .target = getRenderTargetStorageIndex(output),
                                                                .base = INVALID_TEXTURE_INDEX};
                    dispatchBloom(ctx, frameResources[frameCount].commandBuffer, level == 1 ? bloomFirstDownPipeline : bloomDownPipeline, bindlessDescriptorSet,
                                  pushConstants, ctx.getRenderTarget(output));
                });
            graph.passes.back().timerName = "Bloom";
        }

        // Up i adds level i + 1 of the up chain, or of the down chain when that is the frame's last level. With one
        // level, Up 1 only copies it.
        for (uint32_t level = BLOOM_LEVELS - 1; level >= 1; --level)
        {
            const RenderTargetHandle base = chain.down[level - 1];
            const RenderTargetHandle below = chain.down[level];
            const RenderTargetHandle upBelow = level + 1 < BLOOM_LEVELS ? chain.up[level] : RenderTargetHandle{};
            const RenderTargetHandle output = chain.up[level - 1];
            graph.addComputePass(
                "Bloom Up " + std::to_string(level),
                [base, below, upBelow, output](PassBuilder &builder)
                {
                    builder.read(base);
                    builder.read(below);
                    if (upBelow.isValid())
                        builder.read(upBelow);
                    builder.write(output);
                },
                [this, base, below, upBelow, output, level, activeLevels](RenderContext &)
                {
                    const uint32_t levels = *activeLevels;
                    if (levels == 0 || (level >= levels && level > 1))
                        return;
                    uint32_t lower = INVALID_TEXTURE_INDEX;
                    if (level < levels)
                        lower = getBindlessTextureIndex(getRenderTargetTexture(level + 1 == levels ? below : upBelow));
                    const gfx::BloomPushConstants pushConstants{.source = lower,
                                                                .target = getRenderTargetStorageIndex(output),
                                                                .base = getBindlessTextureIndex(getRenderTargetTexture(base)),
                                                                .scale = level == 1 ? 1.0f / static_cast<float>(levels) : 1.0f};
                    dispatchBloom(ctx, frameResources[frameCount].commandBuffer, bloomUpPipeline, bindlessDescriptorSet, pushConstants, ctx.getRenderTarget(output));
                });
            graph.passes.back().timerName = "Bloom";
        }
        return chain.up[0];
    }
}
