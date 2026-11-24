#include "rasm/renderer.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    namespace
    {
        // numthreads of resolveMain in shaders/passes/msaaResolve.slang.
        constexpr uint32_t RESOLVE_GROUP_SIZE = 8;
    }

    // One compute pass that averages each pixel's samples of the source's color 0 into a single-sample storage target of
    // the same size, weighting each sample by 1 / (1 + its exposed luminance): a plain average lets one bright sample
    // outweigh the rest, and a pixel half sky and half roof tone-maps almost as bright as the sky. The pass reads the
    // camera's exposure when it runs. Its pipeline is made when it first runs; a shader error retries every frame.
    RenderTargetHandle Renderer::addMsaaResolvePass(RenderGraph &graph, const RenderTargetHandle &source, CameraHandle camera)
    {
        if (!isLive(camera))
        {
            spdlog::error("addMsaaResolvePass needs a valid camera handle.");
            return {};
        }
        if (!source.isValid() || ctx.getRenderTarget(source).colorCount == 0 || ctx.getRenderTarget(source).samples < 2)
        {
            spdlog::error("addMsaaResolvePass needs a render target with a color attachment and more than one sample per pixel.");
            return {};
        }

        const RenderTargetDesc sourceDesc = ctx.getRenderTargetDesc(source);
        const RenderTargetHandle resolved = graph.createRenderTarget({.name = sourceDesc.name + "_resolved",
                                                                      .width = sourceDesc.width,
                                                                      .height = sourceDesc.height,
                                                                      .swapchainScale = sourceDesc.swapchainScale,
                                                                      .colorFormats = {SCENE_COLOR_FORMAT},
                                                                      .storage = true});
        if (!resolved.isValid())
        {
            spdlog::error("addMsaaResolvePass could not create the resolved render target.");
            return {};
        }

        graph.addComputePass(
            "MSAA Resolve",
            [source, resolved](PassBuilder &builder)
            {
                builder.read(source);
                builder.write(resolved);
            },
            [this, source, resolved, camera](RenderContext &)
            {
                if (!msaaResolvePipeline.isValid())
                {
                    msaaResolvePipeline = createComputePipeline("msaaResolve", readFile(pathFromUtf8(config.shaderDir) / "passes/msaaResolve.slang"), "resolveMain");
                    if (!msaaResolvePipeline.isValid())
                    {
                        spdlog::error("Failed to create the MSAA resolve pipeline; the frame shows what the resolved target held.");
                        return;
                    }
                }
                const RenderTarget &target = ctx.getRenderTarget(resolved);
                const gfx::ResolvePushConstants pushConstants{.source = getBindlessTextureIndex(getRenderTargetTexture(source)),
                                                              .target = getRenderTargetStorageIndex(resolved),
                                                              .samples = ctx.getRenderTarget(source).samples,
                                                              .exposure = registry.cameras[camera.index].exposure};
                const auto commandBuffer = frameResources[frameCount].commandBuffer;
                ctx.bindPipeline(commandBuffer, msaaResolvePipeline);
                ctx.bindDescriptorSet(commandBuffer, msaaResolvePipeline, bindlessDescriptorSet, 0);
                ctx.pushConstants(commandBuffer, msaaResolvePipeline, &pushConstants, sizeof(pushConstants), 0);
                ctx.dispatch(commandBuffer, (target.width + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE, (target.height + RESOLVE_GROUP_SIZE - 1) / RESOLVE_GROUP_SIZE);
            });
        return resolved;
    }
}
