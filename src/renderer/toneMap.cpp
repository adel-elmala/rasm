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
    // The output's rect is fitted to the swapchain or the target when the pass runs, so it follows their size.
    bool Engine::addToneMapPass(RenderGraph &graph, const RenderTargetHandle &source, CameraHandle camera, const RenderTargetHandle &bloom, const ViewOutput &output)
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
        const RenderTargetHandle target = output.target;
        if (output.window.isValid() && (target.isValid() || !isLive(output.window)))
        {
            spdlog::error("addToneMapPass: the output names a window that is invalid or removed, or a window and a target.");
            return false;
        }
        if (target.isValid())
        {
            const RenderTarget &targetData = ctx.getRenderTarget(target);
            if (targetData.colorCount != 1 || targetData.depthFormat != Format::UNKNOWN || targetData.samples != 1 || ctx.getRenderTargetDesc(target).storage || target == source || target == bloom)
            {
                spdlog::error("addToneMapPass draws into a target with one color, no depth and one sample, not a storage one and not one it reads.");
                return false;
            }
        }

        graph.addPass(
            "Tone Map",
            [source, bloom, target, window = output.window](PassBuilder &builder)
            {
                builder.read(source);
                if (bloom.isValid())
                    builder.read(bloom);
                if (target.isValid())
                    builder.write(target);
                else
                    builder.writeSwapchain(window);
            },
            [this, source, camera, bloom, target, window = output.window, rect = output.rect](RenderContext &)
            {
                // The view the camera's scene views drew this frame: a view that drew shaded is tone mapped, a debug
                // view shows its values as they are, or its counts as a heat color.
                const Format format = target.isValid() ? ctx.getRenderTarget(target).colorFormats[0] : ctx.getSwapchainImageFormat(window);
                const PipelineHandle pipeline = getToneMapPipeline(format, getDrawnDebugView(camera));
                if (!pipeline.isValid())
                    return;
                const auto commandBuffer = frameResources[frameCount].commandBuffer;
                const Camera &cameraData = registry.cameras[camera.index];
                // The bloom passes ran this frame (addBloomPasses), so the target holds this frame's bloom.
                const bool bloomed = bloom.isValid() && bloomRuns(camera);
                const gfx::ToneMapPushConstants pushConstants{.source = getBindlessTextureIndex(getRenderTargetTexture(source)),
                                                              .exposure = cameraData.exposure,
                                                              .bloom = bloomed ? getBindlessTextureIndex(getRenderTargetTexture(bloom)) : INVALID_TEXTURE_INDEX,
                                                              .bloomStrength = cameraData.bloomStrength};
                const RenderTarget *targetData = target.isValid() ? &ctx.getRenderTarget(target) : nullptr;
                const Swapchain &swapchain = frameOf(window).swapchain;
                const ViewPixels pixels = getViewPixels(rect, targetData ? targetData->width : swapchain.width, targetData ? targetData->height : swapchain.height);
                ctx.setViewport(commandBuffer, static_cast<float>(pixels.x), static_cast<float>(pixels.y), static_cast<float>(pixels.width), static_cast<float>(pixels.height));
                ctx.setScissor(commandBuffer, static_cast<int32_t>(pixels.x), static_cast<int32_t>(pixels.y), pixels.width, pixels.height);
                ctx.bindPipeline(commandBuffer, pipeline);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
                ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
                ctx.draw(commandBuffer, 3);
            });
        return true;
    }

    // The tone map's pipeline for an output format, or a debug view's, each made the first time it is needed. A debug
    // pipeline that fails leaves the view tone mapped.
    PipelineHandle Engine::getToneMapPipeline(Format format, DebugView shown)
    {
        ToneMapPipelines *entry = nullptr;
        for (ToneMapPipelines &pipelines : toneMapPipelines)
        {
            if (pipelines.format == format)
            {
                entry = &pipelines;
                break;
            }
            if (!entry && pipelines.format == Format::UNKNOWN)
                entry = &pipelines;
        }
        if (!entry)
        {
            spdlog::error("The tone map draws into at most {} formats (MAX_TONE_MAP_FORMATS); this output's is not one of them.", MAX_TONE_MAP_FORMATS);
            return {};
        }
        entry->format = format;
        const Format colorFormats[MAX_COLOR_ATTACHMENTS] = {format};
        if (shown != DebugView::NONE)
        {
            const bool heat = shown == DebugView::OVERDRAW || shown == DebugView::LIGHT_COUNT;
            PipelineHandle &debugPipeline = heat ? entry->debugHeat : entry->debugDisplay;
            if (!debugPipeline.isValid())
            {
                debugPipeline = createGraphicsPipeline(heat ? "toneMapDebugHeat" : "toneMapDebug", readFile(pathFromUtf8(config.shaderDir) / "passes/toneMap.slang"), colorFormats,
                                                       Format::UNKNOWN, "vertMain", heat ? "fragDebugHeat" : "fragDebug", BlendMode::NONE, true);
                if (!debugPipeline.isValid())
                    spdlog::error("Failed to create the debug display pipeline; the debug view is tone mapped.");
            }
            if (debugPipeline.isValid())
                return debugPipeline;
        }
        if (!entry->toneMap.isValid())
        {
            entry->toneMap = createGraphicsPipeline("toneMap", readFile(pathFromUtf8(config.shaderDir) / "passes/toneMap.slang"), colorFormats, Format::UNKNOWN, "vertMain", "fragMain",
                                                    BlendMode::NONE, true);
            if (!entry->toneMap.isValid())
                spdlog::error("Failed to create the tone-map pipeline.");
        }
        return entry->toneMap;
    }
}
