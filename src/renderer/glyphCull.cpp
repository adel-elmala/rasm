#include "renderer/glyphCull.h"

#include "rasm/renderer.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/geometryPool.h"
#include "renderer/gpuScene.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace rasm
{
    // The header, a GlyphChunkSlot per chunk, a word per batch, a draw command per chunk and LOD twice (the chunks with
    // an opaque material, then the weighted ones: lib/glyphs.slang), then a word per glyph of the batches twice: the
    // cull's slots, then the list the draw reads.
    GlyphCullLayout getGlyphCullLayout(uint32_t chunkCapacity, uint32_t batchCapacity)
    {
        const uint64_t glyphs = static_cast<uint64_t>(batchCapacity) * gfx::GLYPH_BATCH_GLYPHS;
        GlyphCullLayout layout;
        layout.commandCapacity = chunkCapacity * MAX_MESH_LODS;
        layout.chunks = sizeof(gfx::GlyphCullHeader);
        layout.batches = layout.chunks + static_cast<uint64_t>(chunkCapacity) * sizeof(gfx::GlyphChunkSlot);
        layout.commands = layout.batches + static_cast<uint64_t>(batchCapacity) * sizeof(uint32_t);
        layout.weightedCommands = layout.commands + static_cast<uint64_t>(layout.commandCapacity) * sizeof(gfx::DrawInfo);
        layout.slots = layout.weightedCommands + static_cast<uint64_t>(layout.commandCapacity) * sizeof(gfx::DrawInfo);
        layout.list = layout.slots + glyphs * sizeof(uint32_t);
        layout.size = layout.list + glyphs * sizeof(uint32_t);
        return layout;
    }

    // A view's or a pick's SceneData names its glyph buffer's parts; address is the buffer's header.
    void writeGlyphCullPointers(gfx::SceneData &sceneData, uint64_t address, const GlyphCullLayout &layout, uint32_t chunkCapacity, uint32_t batchCapacity)
    {
        sceneData.glyphCullPtr = address;
        sceneData.glyphChunksPtr = address + layout.chunks;
        sceneData.glyphBatchesPtr = address + layout.batches;
        sceneData.glyphCommandsPtr = address + layout.commands;
        sceneData.glyphSlotsPtr = address + layout.slots;
        sceneData.glyphListPtr = address + layout.list;
        sceneData.glyphChunkCapacity = chunkCapacity;
        sceneData.glyphBatchCapacity = batchCapacity;
    }

    // Whether this frame culls the view's glyph chunks glyph by glyph and draws them, and the buffer that holds it: a
    // shaded, culled view whose scene has glyph chunks in its point run. A shader error retries every frame, as the
    // other pipelines'. A debug view draws them shaded, as it draws the points.
    void Renderer::prepareGlyphCull(uint32_t viewIndex, gfx::SceneData &sceneData)
    {
        SceneView &view = gpuScene->views[viewIndex];
        view.drawGlyphs = false;
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto &drawList = gpuScene->drawLists[view.scene.index];
        const bool culled = gpuScene->cullCountPipeline.isValid() && gpuScene->cullWritePipeline.isValid();
        if (view.depthOnly || !culled || drawList.glyphChunks == 0 || sceneData.pointRun == gfx::MAX_DRAW_RUNS || !view.glyphCull[frame].isValid())
            return;

        if (!gpuScene->glyphResetPipeline.isValid() || !gpuScene->glyphCountPipeline.isValid() || !gpuScene->glyphWritePipeline.isValid())
        {
            const std::string source = readFile(pathFromUtf8(config.shaderDir) / "passes/glyphCull.slang");
            if (!gpuScene->glyphResetPipeline.isValid())
                gpuScene->glyphResetPipeline = createComputePipeline("glyphReset", source, "resetMain");
            if (!gpuScene->glyphCountPipeline.isValid())
                gpuScene->glyphCountPipeline = createComputePipeline("glyphCount", source, "countMain");
            if (!gpuScene->glyphWritePipeline.isValid())
                gpuScene->glyphWritePipeline = createComputePipeline("glyphWrite", source, "writeMain");
            if (!gpuScene->glyphResetPipeline.isValid() || !gpuScene->glyphCountPipeline.isValid() || !gpuScene->glyphWritePipeline.isValid())
            {
                spdlog::error("Failed to create the glyph cull pipelines; the scene is drawn without its glyphs.");
                return;
            }
        }
        PipelineHandle &pipeline = getShadedPipelines(*gpuScene, ctx, view).glyphPipeline;
        if (!pipeline.isValid())
        {
            pipeline = createScenePipeline("glyphs", "passes/rasm.slang", getMotionDefine(ctx, view) + std::string("#define RASM_GLYPHS 1\n"), view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true);
            if (!pipeline.isValid())
            {
                spdlog::error("Failed to create the glyph pipeline; the scene is drawn without its glyphs.");
                return;
            }
        }

        // Only this slot's last frame used the buffer, and the fence wait covered it. It grows to twice what it held, so a
        // scene whose chunks come in a few at a time does not make it again at each new count.
        const uint32_t needChunks = std::min(drawList.glyphChunks, gfx::GLYPH_MAX_CHUNKS);
        const uint32_t needBatches = std::min(drawList.glyphBatches, gfx::GLYPH_MAX_BATCHES);
        uint32_t chunks = view.glyphChunkCapacity[frame];
        uint32_t batches = view.glyphBatchCapacity[frame];
        if (needChunks > chunks)
            chunks = std::min(std::max(needChunks, 2 * chunks), gfx::GLYPH_MAX_CHUNKS);
        if (needBatches > batches)
            batches = std::min(std::max(needBatches, 2 * batches), gfx::GLYPH_MAX_BATCHES);
        const GlyphCullLayout layout = getGlyphCullLayout(chunks, batches);
        if (view.glyphCullBytes[frame] < layout.size)
        {
            if (!ctx.resizeBuffer(view.glyphCull[frame], layout.size))
            {
                spdlog::error("Failed to grow a glyph cull buffer to {} bytes; the scene is drawn without its glyphs.", layout.size);
                return;
            }
            view.glyphCullBytes[frame] = layout.size;
        }
        view.glyphChunkCapacity[frame] = chunks;
        view.glyphBatchCapacity[frame] = batches;
        writeGlyphCullPointers(sceneData, ctx.getBufferDeviceAddress(view.glyphCull[frame]), layout, chunks, batches);
        view.drawGlyphs = true;
    }

    // Before the view's first cull: no chunk, no batch, no command. The culls append the chunks they keep.
    void Renderer::recordGlyphReset(uint64_t sceneAddress)
    {
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = sceneAddress};
        ctx.bindPipeline(commandBuffer, gpuScene->glyphResetPipeline);
        ctx.pushConstants(commandBuffer, gpuScene->glyphResetPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, 1);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
    }

    // After the last cull, which appended the chunks it kept and their batches: each glyph tested and given its LOD and
    // its place, then written to the list with its chunk's draw commands. Both dispatches are indirect, a group a batch,
    // so a frame with no glyph chunk in view runs no group.
    void Renderer::recordGlyphCull(uint64_t sceneAddress, BufferHandle buffer, uint64_t headerOffset)
    {
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "Glyph Cull");
        const gfx::PushConstants pushConstants{.scene = sceneAddress};
        const uint64_t groups = headerOffset + offsetof(gfx::GlyphCullHeader, groups);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuScene->glyphCountPipeline);
        ctx.pushConstants(commandBuffer, gpuScene->glyphCountPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatchIndirect(commandBuffer, buffer, groups);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuScene->glyphWritePipeline);
        ctx.pushConstants(commandBuffer, gpuScene->glyphWritePipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatchIndirect(commandBuffer, buffer, groups);
        endGpuTimer(commandBuffer, timer);
    }

    // In the view's last draw pass, before the point resolve and the sky: the visible glyphs, each chunk's LOD an
    // instanced draw of its shape's index range, all in one indirect draw with the count the cull wrote.
    void Renderer::drawViewGlyphs(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready || !view.drawGlyphs)
            return;
        const auto frame = static_cast<uint32_t>(frameCount);
        const GlyphCullLayout layout = getGlyphCullLayout(view.glyphChunkCapacity[frame], view.glyphBatchCapacity[frame]);
        CullMode cullMode = gpuScene->drawLists[view.scene.index].glyphCullMode;
        if (view.mirrored && cullMode != CullMode::NONE)
            cullMode = CullMode::FRONT;
        const uint64_t sceneAddress = getSceneDataAddress(ctx, *gpuScene, frame, viewIndex);
        const PipelineHandle pipeline = getShadedPipelines(*gpuScene, ctx, view).glyphPipeline;
        drawGlyphs(sceneAddress, view.glyphCull[frame], 0, layout.commands, layout.commandCapacity, false, pipeline, cullMode, 0);
        // A debug view draws the see-through (weighted) glyphs shaded here too, as it draws the others (D52).
        if (view.debugView != DebugView::NONE && gpuScene->drawLists[view.scene.index].weightedGlyphChunks > 0)
            drawGlyphs(sceneAddress, view.glyphCull[frame], 0, layout.weightedCommands, layout.commandCapacity, true, pipeline, cullMode, 0);
    }

    // The opaque chunks' commands, or the weighted chunks' (weighted), with the count the cull left in the header.
    void Renderer::drawGlyphs(uint64_t sceneAddress, BufferHandle buffer, uint64_t headerOffset, uint64_t commandsOffset, uint32_t commandCapacity, bool weighted, PipelineHandle pipeline, CullMode cullMode,
                              uint32_t argument)
    {
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "Glyphs");
        const gfx::PushConstants pushConstants{.scene = sceneAddress, .argument = argument};
        ctx.bindPipeline(commandBuffer, pipeline);
        ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.bindIndexBuffer(commandBuffer, geometryPool->indexBuffer(), 0, Format::U32_UINT);
        ctx.setCullMode(commandBuffer, cullMode);
        const uint64_t countOffset = weighted ? offsetof(gfx::GlyphCullHeader, weightedCommandCount) : offsetof(gfx::GlyphCullHeader, commandCount);
        ctx.drawIndexedIndirectCount(commandBuffer, buffer, headerOffset + commandsOffset, buffer, headerOffset + countOffset, commandCapacity, sizeof(gfx::DrawInfo));
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        endGpuTimer(commandBuffer, timer);
    }
}
