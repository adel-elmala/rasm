#include "renderer/pointRaster.h"

#include "rasm/renderer.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/gpuScene.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace rasm
{
    // A batch per POINT_BATCH_POINTS points of each chunk of the run, up to the raster dispatch's group limit; past it a
    // chunk goes to the triangles (appendPointBatches in shaders/lib/points.slang).
    uint32_t getPointBatchCapacity(uint32_t chunks)
    {
        return static_cast<uint32_t>(std::min(static_cast<uint64_t>(chunks) * gfx::POINT_BATCHES_PER_CHUNK, static_cast<uint64_t>(gfx::POINT_MAX_BATCHES)));
    }

    // The header, the batches (a uint2 each) and a 64-bit word per pixel, which the batches' size keeps 8-byte aligned.
    PointRasterLayout getPointRasterLayout(uint32_t batchCapacity, uint32_t width, uint32_t height)
    {
        PointRasterLayout layout;
        layout.batches = sizeof(gfx::PointRasterHeader);
        layout.pixels = layout.batches + static_cast<uint64_t>(batchCapacity) * 2 * sizeof(uint32_t);
        layout.size = layout.pixels + static_cast<uint64_t>(width) * height * sizeof(uint64_t);
        return layout;
    }

    void Renderer::setComputePoints(bool on)
    {
        if (on && !ctx.hasBufferInt64Atomics())
            spdlog::warn("setComputePoints: this GPU has no 64-bit buffer atomics (shaderBufferInt64Atomics); every point is drawn by triangles.");
        config.computePoints = on;
    }

    bool Renderer::getComputePoints() const
    {
        return config.computePoints;
    }

    // Whether this frame's culls send the view's small points to the compute path, and the buffer they write. A shaded,
    // culled view with a point run, no debug view (it shows the triangles' overdraw and the like) and the buffers, which
    // createSceneView made where the GPU has the atomics. A shader error retries every frame, as the other pipelines'.
    void Renderer::preparePointRaster(uint32_t viewIndex, gfx::SceneData &sceneData)
    {
        SceneView &view = gpuScene->views[viewIndex];
        view.rasterPoints = false;
        const auto frame = static_cast<uint32_t>(frameCount);
        const bool culled = gpuScene->cullCountPipeline.isValid() && gpuScene->cullWritePipeline.isValid();
        if (!config.computePoints || view.depthOnly || !culled || view.debugView != DebugView::NONE || sceneData.pointRun == gfx::MAX_DRAW_RUNS || !view.pointRaster[frame].isValid())
            return;

        if (!gpuScene->pointResetPipeline.isValid() || !gpuScene->pointClearPipeline.isValid() || !gpuScene->pointRasterPipeline.isValid())
        {
            const std::string source = readFile(pathFromUtf8(config.shaderDir) / "passes/pointRaster.slang");
            if (!gpuScene->pointResetPipeline.isValid())
                gpuScene->pointResetPipeline = createComputePipeline("pointReset", source, "resetMain");
            if (!gpuScene->pointClearPipeline.isValid())
                gpuScene->pointClearPipeline = createComputePipeline("pointClear", source, "clearMain");
            if (!gpuScene->pointRasterPipeline.isValid())
                gpuScene->pointRasterPipeline = createComputePipeline("pointRaster", source, "rasterMain");
            if (!gpuScene->pointResetPipeline.isValid() || !gpuScene->pointClearPipeline.isValid() || !gpuScene->pointRasterPipeline.isValid())
            {
                spdlog::error("Failed to create the point raster pipelines; every point is drawn by triangles.");
                return;
            }
        }
        PipelineHandle &resolvePipeline = getShadedPipelines(*gpuScene, ctx, view).pointResolvePipeline;
        if (!resolvePipeline.isValid())
        {
            resolvePipeline = createScenePipeline("pointResolve", "passes/pointRaster.slang", getMotionDefine(ctx, view), view.renderTarget, "resolveVert", "resolveFrag", BlendMode::NONE, true);
            if (!resolvePipeline.isValid())
            {
                spdlog::error("Failed to create the point resolve pipeline; every point is drawn by triangles.");
                return;
            }
        }

        // Only this slot's last frame used the buffer, and the fence wait covered it.
        const auto &drawList = gpuScene->drawLists[view.scene.index];
        const uint32_t capacity = getPointBatchCapacity(drawList.runs[sceneData.pointRun].count);
        const PointRasterLayout layout = getPointRasterLayout(capacity, sceneData.viewportWidth, sceneData.viewportHeight);
        if (view.pointRasterBytes[frame] < layout.size)
        {
            if (!ctx.resizeBuffer(view.pointRaster[frame], layout.size))
            {
                spdlog::error("Failed to grow a point raster buffer to {} bytes; every point is drawn by triangles.", layout.size);
                return;
            }
            view.pointRasterBytes[frame] = layout.size;
        }
        const uint64_t address = ctx.getBufferDeviceAddress(view.pointRaster[frame]);
        sceneData.pointRasterPtr = address;
        sceneData.pointBatchesPtr = address + layout.batches;
        sceneData.pointPixelsPtr = address + layout.pixels;
        sceneData.pointBatchCapacity = capacity;
        view.rasterPoints = true;
    }

    // Before the view's first cull: no batches, the clear and the resolve off. The culls turn them on.
    void Renderer::recordPointReset(uint32_t viewIndex)
    {
        if (!gpuScene->views[viewIndex].rasterPoints)
            return;
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        ctx.bindPipeline(commandBuffer, gpuScene->pointResetPipeline);
        ctx.pushConstants(commandBuffer, gpuScene->pointResetPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, 1);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
    }

    // After the view's last cull, which wrote the batches: the pixels cleared, then every batch's points written. Both
    // dispatches are indirect, so a frame with no chunk on the compute path runs no group.
    void Renderer::recordPointRaster(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.rasterPoints)
            return;
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, frame, viewIndex)};
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuScene->pointClearPipeline);
        ctx.pushConstants(commandBuffer, gpuScene->pointClearPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatchIndirect(commandBuffer, view.pointRaster[frame], offsetof(gfx::PointRasterHeader, clearGroups));
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuScene->pointRasterPipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->pointRasterPipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, gpuScene->pointRasterPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatchIndirect(commandBuffer, view.pointRaster[frame], offsetof(gfx::PointRasterHeader, rasterGroups));
    }

    // In the view's last draw pass, before the sky: a quad over the rect the compute path's chunks reach, which writes
    // each pixel's point. Its vertex stage collapses it while no chunk took the compute path. (An indirect draw of a
    // count in the header drew nothing on lavapipe, where the indirect dispatches work.)
    void Renderer::drawPointResolve(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.rasterPoints)
            return;
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const PipelineHandle &pipeline = getShadedPipelines(*gpuScene, ctx, view).pointResolvePipeline;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, frame, viewIndex)};
        ctx.bindPipeline(commandBuffer, pipeline);
        ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        ctx.draw(commandBuffer, 6);
    }
}
