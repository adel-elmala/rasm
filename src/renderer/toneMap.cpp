#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    // The pass reads the camera's exposure and bloom strength when it runs, so changing them needs no new graph. Its
    // pipeline is made when the pass first runs, and a shader error retries every frame, so a shader fixed on disk is
    // picked up while the application runs. Until then the pass draws nothing and the frame shows the clear color.
    bool Engine::addToneMapPass(RenderGraph &graph, const RenderTargetHandle &source, CameraHandle camera, const RenderTargetHandle &bloom)
    {
        if (!isLive(camera))
        {
            spdlog::error("addToneMapPass needs a valid camera handle.");
            return false;
        }
        if (!source.isValid() || ctx.getRenderTarget(source).colorCount == 0)
        {
            spdlog::error("addToneMapPass needs a render target with a color attachment.");
            return false;
        }

        if (ctx.getRenderTarget(source).samples > 1)
        {
            spdlog::error("addToneMapPass samples one value per pixel: resolve a multisampled target first (addMsaaResolvePass).");
            return false;
        }
        if (bloom.isValid() && ctx.getRenderTarget(bloom).colorCount == 0)
        {
            spdlog::error("addToneMapPass needs a bloom target from addBloomPasses.");
            return false;
        }

        graph.addPass(
            "Tone Map",
            [source, bloom](PassBuilder &builder)
            {
                builder.read(source);
                if (bloom.isValid())
                    builder.read(bloom);
                builder.writeSwapchain();
            },
            [this, source, camera, bloom](RenderContext &)
            {
                if (!toneMapPipeline.isValid())
                {
                    toneMapPipeline = createPipeline("toneMap", readFile(pathFromUtf8(config.shaderDir) / "passes/toneMap.slang"));
                    if (!toneMapPipeline.isValid())
                    {
                        spdlog::error("Failed to create the tone-map pipeline.");
                        return;
                    }
                }
                // A debug view shows the target's values as they are, or its counts as a heat color, made the first
                // time a view is chosen.
                // The view the camera's scene views drew this frame: a view that drew shaded is tone mapped.
                PipelineHandle pipeline = toneMapPipeline;
                const DebugView shown = getDrawnDebugView(camera);
                if (shown != DebugView::NONE)
                {
                    const bool heat = shown == DebugView::OVERDRAW || shown == DebugView::LIGHT_COUNT;
                    PipelineHandle &debugPipeline = heat ? debugHeatPipeline : debugDisplayPipeline;
                    if (!debugPipeline.isValid())
                    {
                        const Format colorFormats[MAX_COLOR_ATTACHMENTS] = {ctx.getSwapchainImageFormat()};
                        debugPipeline = createGraphicsPipeline(heat ? "toneMapDebugHeat" : "toneMapDebug", readFile(pathFromUtf8(config.shaderDir) / "passes/toneMap.slang"), colorFormats,
                                                               Format::UNKNOWN, "vertMain", heat ? "fragDebugHeat" : "fragDebug", BlendMode::NONE, true);
                        if (!debugPipeline.isValid())
                            spdlog::error("Failed to create the debug display pipeline; the debug view is tone mapped.");
                    }
                    if (debugPipeline.isValid())
                        pipeline = debugPipeline;
                }
                const auto commandBuffer = frameResources[frameCount].commandBuffer;
                const Camera &cameraData = registry.cameras[camera.index];
                // The bloom passes ran this frame (addBloomPasses), so the target holds this frame's bloom.
                const bool bloomed = bloom.isValid() && bloomRuns(camera);
                const gfx::ToneMapPushConstants pushConstants{.source = getBindlessTextureIndex(getRenderTargetTexture(source)),
                                                              .exposure = cameraData.exposure,
                                                              .bloom = bloomed ? getBindlessTextureIndex(getRenderTargetTexture(bloom)) : INVALID_TEXTURE_INDEX,
                                                              .bloomStrength = cameraData.bloomStrength};
                ctx.bindPipeline(commandBuffer, pipeline);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
                ctx.draw(commandBuffer, 3);
            });
        return true;
    }
}
