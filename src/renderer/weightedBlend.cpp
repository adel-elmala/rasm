#include "renderer/weightedBlend.h"

#include "rasm/renderer.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/glyphCull.h"
#include "renderer/gpuScene.h"
#include "renderer/pipelineWarmup.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace rasm
{
    namespace
    {
        // With perspective the span's ends snap to steps of sqrt(2) in depth; orthographic to an eighth of the span. An
        // end moves out at once and in only past two steps, so a draw that comes and goes at the edge of the view, or a
        // glyph chunk culled and kept, leaves the weights as they are.
        constexpr float PERSPECTIVE_STEP = 0.5f;
        constexpr float ORTHOGRAPHIC_STEPS = 8.0f;
        constexpr float HYSTERESIS_STEPS = 2.0f;
        constexpr uint32_t EMPTY_RANGE[2] = {0xFFFFFFFFu, 0u};
    }

    namespace
    {
        float toMeasure(float depth, bool orthographic)
        {
            return orthographic ? depth : std::log2(std::max(depth, 1e-6f));
        }

        float fromMeasure(float measure, bool orthographic)
        {
            return orthographic ? measure : std::exp2(measure);
        }

        float snapDown(float value, float step)
        {
            return std::floor(value / step) * step;
        }

        float snapUp(float value, float step)
        {
            return std::ceil(value / step) * step;
        }
    }

    RenderTargetDesc getWeightedTargetDesc(const RenderTargetDesc &sceneTarget)
    {
        const bool motion = sceneTarget.colorFormats[1] != Format::UNKNOWN;
        return {.name = sceneTarget.name + " weighted",
                .width = sceneTarget.width,
                .height = sceneTarget.height,
                .colorFormats = {WEIGHTED_ACCUM_FORMAT, motion ? WEIGHTED_MOTION_FORMAT : WEIGHTED_COVERAGE_FORMAT},
                .samples = sceneTarget.samples};
    }

    // A multisampled target is composited per sample where the GPU can run a fragment per sample.
    const char *getCompositeDefines(uint32_t samples, bool sampleRateShading)
    {
        if (samples <= 1)
            return "";
        return sampleRateShading ? "#define RASM_MULTISAMPLED 1\n#define RASM_PER_SAMPLE 1\n" : "#define RASM_MULTISAMPLED 1\n";
    }

    uint32_t getDepthBits(float depth)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &depth, sizeof(bits));
        return (bits & 0x80000000u) != 0 ? ~bits : bits | 0x80000000u;
    }

    float fromDepthBits(uint32_t bits)
    {
        const uint32_t raw = (bits & 0x80000000u) != 0 ? bits & 0x7FFFFFFFu : ~bits;
        float depth = 0.0f;
        std::memcpy(&depth, &raw, sizeof(depth));
        return depth;
    }

    void stepWeightedRange(WeightedRange &range, float nearest, float farthest, bool orthographic, float seconds)
    {
        if (!(nearest <= farthest) || !std::isfinite(nearest) || !std::isfinite(farthest))
            return;
        if (range.valid && range.orthographic != orthographic)
            range = {};
        const float a = toMeasure(nearest, orthographic);
        const float b = toMeasure(farthest, orthographic);
        const float width = range.valid ? range.targetFarthest - range.targetNearest : b - a;
        const float step = orthographic ? std::max(width, b - a) / ORTHOGRAPHIC_STEPS : PERSPECTIVE_STEP;
        if (!(step > 0.0f))
            return;
        if (!range.valid)
        {
            range = {.targetNearest = snapDown(a, step), .targetFarthest = snapUp(b, step), .orthographic = orthographic, .valid = true};
            range.nearest = range.targetNearest;
            range.farthest = range.targetFarthest;
            return;
        }
        if (a < range.targetNearest || a > range.targetNearest + HYSTERESIS_STEPS * step)
            range.targetNearest = snapDown(a, step);
        if (b > range.targetFarthest || b < range.targetFarthest - HYSTERESIS_STEPS * step)
            range.targetFarthest = snapUp(b, step);
        const float share = 1.0f - std::exp(-std::max(seconds, 0.0f) / WEIGHTED_RANGE_SECONDS);
        range.nearest += (range.targetNearest - range.nearest) * share;
        range.farthest += (range.targetFarthest - range.farthest) * share;
    }

    glm::vec2 getWeightedDepths(const WeightedRange &range)
    {
        if (!range.valid)
            return glm::vec2(0.0f);
        return glm::vec2(fromMeasure(range.nearest, range.orthographic), fromMeasure(range.farthest, range.orthographic));
    }

    // What a shaded view's weighted and additive draws need this frame: the runs culling leaves for the late phase, the
    // pipelines, the weighted target at the scene target's size and samples, and the span of the weights, from the
    // cull's words of this frame slot's last frame. A debug view draws both runs in the Transparent pass instead
    // (drawTransparent). A shader error retries every frame, as the other pipelines'.
    void Renderer::prepareWeightedBlend(uint32_t viewIndex, gfx::SceneData &sceneData)
    {
        SceneView &view = gpuScene->views[viewIndex];
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto &drawList = gpuScene->drawLists[view.scene.index];
        sceneData.weightedRun = gfx::MAX_DRAW_RUNS;
        sceneData.additiveRun = gfx::MAX_DRAW_RUNS;
        sceneData.weightedRangePtr = 0;
        sceneData.weightedDepths = glm::vec2(0.0f);
        sceneData.weightedAccum = INVALID_TEXTURE_INDEX;
        sceneData.weightedCoverage = INVALID_TEXTURE_INDEX;
        sceneData.weightedSamples = 1;
        // A depth-only view names its additive run only so that culling leaves it out: it casts no shadow.
        if (view.depthOnly)
        {
            sceneData.additiveRun = view.additiveRun;
            return;
        }
        // Culling keeps them for the late phase, like the blend run, or for the only one.
        const bool late = view.late || view.hizLevelCount == 0;
        sceneData.weightedRun = late ? view.weightedRun : gfx::MAX_DRAW_RUNS;
        sceneData.additiveRun = late ? view.additiveRun : gfx::MAX_DRAW_RUNS;
        if (view.debugView != DebugView::NONE)
        {
            if (view.weightedRange[frame].isValid())
                ctx.fillBuffer(view.weightedRange[frame], EMPTY_RANGE, sizeof(EMPTY_RANGE));
            return;
        }

        // The strings, the path and the target's desc are made only while something is missing: a steady frame makes no
        // heap allocation (P27).
        ShadedPipelines &shaded = getShadedPipelines(*gpuScene, ctx, view);
        const RenderTarget &sceneTarget = ctx.getRenderTarget(view.renderTarget);
        if (view.additiveRun < gfx::MAX_DRAW_RUNS && !shaded.additivePipeline.isValid())
        {
            shaded.additivePipeline = createScenePipeline("rasmAdditive", "passes/rasm.slang", getMotionDefine(ctx, view) + std::string(WEIGHTED_ADDITIVE_DEFINES), view.renderTarget, "vertMain",
                                                          "fragMain", BlendMode::ADDITIVE_COLOR, false);
            if (!shaded.additivePipeline.isValid())
                spdlog::error("Failed to create the additive pipeline; the scene is drawn without its additive materials.");
        }
        view.drawAdditive = view.additiveRun < gfx::MAX_DRAW_RUNS && shaded.additivePipeline.isValid();

        const bool weightedRun = view.weightedRun < gfx::MAX_DRAW_RUNS;
        const bool weightedGlyphs = view.drawGlyphs && drawList.weightedGlyphChunks > 0;
        // A frame that measures nothing still empties its slot's words, so the next weighted frame of the slot reads only
        // what its own cull wrote.
        if (!weightedRun && !weightedGlyphs)
        {
            if (view.weightedRange[frame].isValid())
                ctx.fillBuffer(view.weightedRange[frame], EMPTY_RANGE, sizeof(EMPTY_RANGE));
            return;
        }
        // With motion (TAA) the coverage shares a target with the motion sums, as its alpha: two targets, not three.
        const bool motion = sceneTarget.colorCount > 1;
        const Format colorFormats[MAX_COLOR_ATTACHMENTS] = {WEIGHTED_ACCUM_FORMAT, motion ? WEIGHTED_MOTION_FORMAT : WEIGHTED_COVERAGE_FORMAT};
        const BlendMode blendMode = motion ? BlendMode::WEIGHTED_MOTION : BlendMode::WEIGHTED;
        if (!shaded.weightedPipeline.isValid() || (weightedGlyphs && !shaded.weightedGlyphPipeline.isValid()) || !shaded.compositePipeline.isValid())
        {
            const std::string defines = getMotionDefine(ctx, view) + std::string(WEIGHTED_DEFINES);
            const std::string rasm = readFile(pathFromUtf8(config.shaderDir) / "passes/rasm.slang");
            if (!shaded.weightedPipeline.isValid())
                shaded.weightedPipeline = createGraphicsPipeline("rasmWeighted", getShaderVariant(defines, rasm), colorFormats, sceneTarget.depthFormat, "vertMain", "fragMain", blendMode, false,
                                                                 sceneTarget.samples);
            if (weightedGlyphs && !shaded.weightedGlyphPipeline.isValid())
                shaded.weightedGlyphPipeline = createGraphicsPipeline("rasmWeightedGlyphs", getShaderVariant(defines + "#define RASM_GLYPHS 1\n", rasm), colorFormats, sceneTarget.depthFormat,
                                                                      "vertMain", "fragMain", blendMode, false, sceneTarget.samples);
            if (!shaded.compositePipeline.isValid())
                shaded.compositePipeline = createScenePipeline("weightedComposite", "passes/weightedComposite.slang", getMotionDefine(ctx, view) + std::string(getCompositeDefines(sceneTarget.samples, ctx.hasSampleRateShading())),
                                                               view.renderTarget, "vertMain", "fragMain", BlendMode::PREMULTIPLIED_ALPHA, false, false, false);
            if (!shaded.weightedPipeline.isValid() || !shaded.compositePipeline.isValid())
            {
                spdlog::error("Failed to create the weighted blend pipelines; the scene is drawn without its weighted materials.");
                return;
            }
        }
        if (sceneTarget.samples > 1 && !ctx.hasSampleRateShading() && !gpuScene->sampleShadingLogged)
        {
            spdlog::warn("This GPU has no sampleRateShading feature: weighted see-through surfaces are composited by each pixel's mean of its samples.");
            gpuScene->sampleShadingLogged = true;
        }

        // Made at the view's first weighted frame, and made again when the scene target changes size or samples; the old
        // one goes once the frames in flight are done with it.
        bool stale = false;
        if (view.weightedTarget.isValid())
        {
            const RenderTarget &weighted = ctx.getRenderTarget(view.weightedTarget);
            stale = weighted.width != sceneTarget.width || weighted.height != sceneTarget.height || weighted.samples != sceneTarget.samples || weighted.colorFormats[1] != colorFormats[1];
        }
        if (stale)
        {
            destroyRenderTarget(view.weightedTarget);
            view.weightedTarget = {};
        }
        if (!view.weightedTarget.isValid())
        {
            RenderTargetDesc desc = getWeightedTargetDesc(ctx.getRenderTargetDesc(view.renderTarget));
            desc.width = sceneTarget.width;
            desc.height = sceneTarget.height;
            view.weightedTarget = ctx.createRenderTarget(desc);
            if (!view.weightedTarget.isValid())
            {
                spdlog::error("Failed to create a weighted blend target of {} x {}; the scene is drawn without its weighted materials.", desc.width, desc.height);
                return;
            }
        }
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT && !view.weightedRange[i].isValid(); ++i)
        {
            view.weightedRange[i] = ctx.createBuffer({.name = "Weighted Range " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = sizeof(EMPTY_RANGE), .hostVisible = true, .hostReadable = true});
            if (!view.weightedRange[i].isValid() || !ctx.fillBuffer(view.weightedRange[i], EMPTY_RANGE, sizeof(EMPTY_RANGE)))
            {
                spdlog::error("Failed to create a weighted blend range buffer; the scene is drawn without its weighted materials.");
                return;
            }
        }

        // This slot's frame before wrote its words, and the fence wait covered it.
        uint32_t words[2] = {EMPTY_RANGE[0], EMPTY_RANGE[1]};
        ctx.readBuffer(view.weightedRange[frame], words, sizeof(words));
        ctx.fillBuffer(view.weightedRange[frame], EMPTY_RANGE, sizeof(EMPTY_RANGE));
        const bool orthographic = sceneData.projection[3][3] != 0.0f;
        if (words[0] <= words[1])
            stepWeightedRange(view.weightedDepths, fromDepthBits(words[0]), fromDepthBits(words[1]), orthographic, deltaTime);
        if (view.weightedDepths.valid && view.weightedDepths.orthographic != orthographic)
            view.weightedDepths = {};

        const RenderTarget &weighted = ctx.getRenderTarget(view.weightedTarget);
        sceneData.weightedRangePtr = ctx.getBufferDeviceAddress(view.weightedRange[frame]);
        sceneData.weightedDepths = getWeightedDepths(view.weightedDepths);
        sceneData.weightedAccum = getBindlessTextureIndex(weighted.attachments[frame].color[0]);
        sceneData.weightedCoverage = getBindlessTextureIndex(weighted.attachments[frame].color[1]);
        sceneData.weightedSamples = weighted.samples;
        view.drawWeightedGlyphs = weightedGlyphs && shaded.weightedGlyphPipeline.isValid();
        view.drawWeighted = weightedRun || view.drawWeightedGlyphs;
    }

    // At the end of the view's last draw pass, after its sky: the pass's rendering stops, the weighted draws go into the
    // weighted target against the pass's depth (tested, not written), and the pass goes on where it was. The textures
    // are the view's own, used only here and by the composite on this queue, so their barriers are recorded here.
    void Renderer::drawWeighted(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready || !view.drawWeighted)
            return;
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "Weighted");
        const RenderTarget &sceneTarget = ctx.getRenderTarget(view.renderTarget);
        const RenderTarget &weighted = ctx.getRenderTarget(view.weightedTarget);
        const RenderTargetAttachments &sceneAttachments = sceneTarget.attachments[frame];
        const RenderTargetAttachments &weightedAttachments = weighted.attachments[frame];
        ctx.endRendering(commandBuffer);
        for (uint32_t i = 0; i < weighted.colorCount; ++i)
            ctx.transitionImageLayout(commandBuffer, weightedAttachments.color[i], TextureUsage::COLOR_ATTACHMENT, true);
        ctx.transitionImageLayout(commandBuffer, sceneAttachments.depth, TextureUsage::DEPTH_STENCIL_ATTACHMENT, false);
        ctx.beginRendering(commandBuffer, weightedAttachments.color, weighted.colorCount, sceneAttachments.depth, glm::vec4(0.0f), {.loadDepth = true, .storeDepth = true}, view.face);
        ctx.setViewport(commandBuffer, 0.0f, 0.0f, static_cast<float>(sceneTarget.width), static_cast<float>(sceneTarget.height));
        ctx.setScissor(commandBuffer, 0, 0, sceneTarget.width, sceneTarget.height);
        const ShadedPipelines &shaded = getShadedPipelines(*gpuScene, ctx, view);
        if (view.weightedRun < gfx::MAX_DRAW_RUNS)
            drawBlendRun(viewIndex, view.weightedRun, shaded.weightedPipeline);
        if (view.drawWeightedGlyphs)
        {
            const GlyphCullLayout layout = getGlyphCullLayout(view.glyphChunkCapacity[frame], view.glyphBatchCapacity[frame]);
            CullMode cullMode = gpuScene->drawLists[view.scene.index].glyphCullMode;
            if (view.mirrored && cullMode != CullMode::NONE)
                cullMode = CullMode::FRONT;
            drawGlyphs(getSceneDataAddress(ctx, *gpuScene, frame, viewIndex), view.glyphCull[frame], 0, layout.weightedCommands, layout.commandCapacity, true, shaded.weightedGlyphPipeline, cullMode, 0);
        }
        ctx.endRendering(commandBuffer);
        for (uint32_t i = 0; i < weighted.colorCount; ++i)
            ctx.transitionImageLayout(commandBuffer, weightedAttachments.color[i], TextureUsage::SAMPLED);
        ctx.beginRendering(commandBuffer, sceneAttachments.color, sceneTarget.colorCount, sceneAttachments.depth, sceneTarget.clearColor, {.loadColor = true, .loadDepth = true, .storeDepth = true},
                           view.face);
        ctx.setViewport(commandBuffer, 0.0f, 0.0f, static_cast<float>(sceneTarget.width), static_cast<float>(sceneTarget.height));
        ctx.setScissor(commandBuffer, 0, 0, sceneTarget.width, sceneTarget.height);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        endGpuTimer(commandBuffer, timer);
    }

    // In the Transparent pass, after the grid: the weighted surfaces' mean color over what is behind them, by their
    // coverage (passes/weightedComposite.slang).
    void Renderer::drawWeightedComposite(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready || !view.drawWeighted)
            return;
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "OIT Composite");
        const PipelineHandle pipeline = getShadedPipelines(*gpuScene, ctx, view).compositePipeline;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        ctx.bindPipeline(commandBuffer, pipeline);
        ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        ctx.draw(commandBuffer, 3);
        endGpuTimer(commandBuffer, timer);
    }

    // After the composite: the additive run's light, in any order, over everything but the sorted draws.
    void Renderer::drawAdditive(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready || !view.drawAdditive)
            return;
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "Additive");
        drawBlendRun(viewIndex, view.additiveRun, getShadedPipelines(*gpuScene, ctx, view).additivePipeline);
        endGpuTimer(commandBuffer, timer);
    }
}
