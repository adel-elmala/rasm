#include "rasm/render/renderGraph.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <unordered_map>
#include <queue>

namespace rasm
{
    void RenderGraph::addPass(std::string name, std::function<void(PassBuilder &)> setup, std::function<void(RenderContext &)> execute)
    {
        uint32_t passIdx = static_cast<uint32_t>(passes.size());
        Pass p = {};
        p.name = std::move(name);
        p.execute = std::move(execute);
        passes.push_back(p);

        PassBuilder builder(*this, passIdx);
        setup(builder);
        compiled = false; // Mark graph as dirty
    }

    void RenderGraph::addComputePass(std::string name, std::function<void(PassBuilder &)> setup, std::function<void(RenderContext &)> execute)
    {
        addPass(std::move(name), std::move(setup), std::move(execute));
        auto &pass = passes.back();
        pass.compute = true;
        // A compute pass runs outside any render pass, so it has no attachments to write.
        if (pass.outputRenderTarget.isValid())
        {
            spdlog::error("Compute pass '{}' cannot write a render target; the write is ignored.", pass.name);
            pass.outputRenderTarget = {};
        }
    }

    void RenderGraph::compile()
    {
        if (compiled)
            return;

        // Reset in-degrees and successors for all passes
        for (auto &pass : passes)
        {
            pass.inDegree = 0;
            pass.successors.clear();
        }

        // 1. Map resources to their producers
        std::unordered_map<TextureHandle, uint32_t, HandleHash> texturesWriters;
        std::unordered_map<BufferHandle, uint32_t, HandleHash> buffersWriters;
        std::unordered_map<RenderTargetHandle, uint32_t, HandleHash> renderTargetsWriters;
        for (uint32_t i = 0; i < passes.size(); ++i)
        {
            auto &pass = passes[i];
            for (auto outHandle : pass.outputTextures)
            {
                texturesWriters[outHandle] = i;
            }

            for (auto outHandle : pass.outputBuffers)
            {
                buffersWriters[outHandle] = i;
            }
            if (pass.outputRenderTarget.isValid())
            {
                renderTargetsWriters[pass.outputRenderTarget] = i;
            }
        }

        // 2. Build the Adjacency List (Edges)
        for (uint32_t i = 0; i < passes.size(); ++i)
        {
            for (auto inHandle : passes[i].inputTextures)
            {
                uint32_t producerIdx = texturesWriters.find(inHandle) != texturesWriters.end() ? texturesWriters[inHandle] : static_cast<uint32_t>(-1);
                if (producerIdx != static_cast<uint32_t>(-1) && producerIdx != i)
                {
                    // There is a dependency: Producer -> Current Pass
                    passes[producerIdx].successors.push_back(i);
                    passes[i].inDegree++;
                }
            }

            for (auto inHandle : passes[i].inputRenderTargets)
            {
                uint32_t producerIdx = renderTargetsWriters.find(inHandle) != renderTargetsWriters.end() ? renderTargetsWriters[inHandle] : static_cast<uint32_t>(-1);
                if (producerIdx != static_cast<uint32_t>(-1) && producerIdx != i)
                {
                    // There is a dependency: Producer -> Current Pass
                    passes[producerIdx].successors.push_back(i);
                    passes[i].inDegree++;
                }
            }

            for (auto inHandle : passes[i].inputBuffers)
            {
                uint32_t producerIdx = buffersWriters.find(inHandle) != buffersWriters.end() ? buffersWriters[inHandle] : static_cast<uint32_t>(-1);
                if (producerIdx != static_cast<uint32_t>(-1) && producerIdx != i)
                {
                    // There is a dependency: Producer -> Current Pass
                    passes[producerIdx].successors.push_back(i);
                    passes[i].inDegree++;
                }
            }
        }

        // 3. Kahn's Algorithm (Topological Sort)
        std::queue<uint32_t> zeroInDegreeQueue;
        for (uint32_t i = 0; i < passes.size(); ++i)
        {
            if (passes[i].inDegree == 0)
            {
                zeroInDegreeQueue.push(i);
            }
        }

        std::vector<uint32_t> sortedIndices;
        while (!zeroInDegreeQueue.empty())
        {
            uint32_t curr = zeroInDegreeQueue.front();
            zeroInDegreeQueue.pop();
            sortedIndices.push_back(curr);

            for (uint32_t successor : passes[curr].successors)
            {
                passes[successor].inDegree--;
                if (passes[successor].inDegree == 0)
                {
                    zeroInDegreeQueue.push(successor);
                }
            }
        }

        // 4. Safety Check: Circular Dependencies
        if (sortedIndices.size() != passes.size())
        {
            spdlog::error("Frame Graph contains a cycle!");
            // execute() keeps running frames so the window stays responsive, but they draw no passes.
            executionOrder.clear();
            compiled = false;
            return;
        }

        // 5. Store the final execution order
        executionOrder = std::move(sortedIndices);
        compiled = true;
        spdlog::info("Successfully compiled {} passes.", executionOrder.size());
    }

    void RenderGraph::execute(CameraHandle camera)
    {
        ZoneScoped;

        if (!compiled)
            compile();

        auto &ctx = engine->getRenderContext();

        if (!engine->beginFrame(camera))
            return;

        // Shaders share buffers through device addresses, so a barrier goes before every compute pass and
        // before the first graphics pass after one. The frame starts after the previous frame's draws, and
        // a compute pass at the end of the frame is made visible to the next frame's draws.
        auto commandBuffer = engine->getCurrentFrameResources().commandBuffer;
        PipelineStage previous = PipelineStage::GRAPHICS;
        for (const auto &passIdx : executionOrder)
        {
            auto &pass = passes[passIdx];
            const PipelineStage stage = pass.compute ? PipelineStage::COMPUTE : PipelineStage::GRAPHICS;
            if (pass.compute || previous == PipelineStage::COMPUTE)
                ctx.memoryBarrier(commandBuffer, previous, stage);
            previous = stage;

            if (pass.compute)
            {
                pass.execute(ctx);
                continue;
            }
            engine->setRenderTarget(pass.outputRenderTarget);
            engine->beginPass();
            pass.execute(ctx);
            engine->endPass();
            engine->resetRenderTarget();
        }
        if (previous == PipelineStage::COMPUTE)
            ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::GRAPHICS);
        engine->endFrame();
    }

    TextureHandle RenderGraph::createTexture(const TextureDesc &desc)
    {
        auto handle = engine->createTexture(desc);
        return handle;
    }

    BufferHandle RenderGraph::createBuffer(const BufferDesc &desc)
    {
        auto handle = engine->createBuffer(desc);
        return handle;
    }

    RenderTargetHandle RenderGraph::createRenderTarget(const RenderTargetDesc &desc)
    {
        auto handle = engine->createRenderTarget(desc);
        return handle;
    }

    void RenderGraph::read(uint32_t passIdx, TextureHandle h) { passes[passIdx].inputTextures.push_back(h); }

    void RenderGraph::read(uint32_t passIdx, BufferHandle h) { passes[passIdx].inputBuffers.push_back(h); }

    void RenderGraph::read(uint32_t passIdx, RenderTargetHandle h) { passes[passIdx].inputRenderTargets.push_back(h); }

    void RenderGraph::write(uint32_t passIdx, TextureHandle h) { passes[passIdx].outputTextures.push_back(h); }

    void RenderGraph::write(uint32_t passIdx, BufferHandle h) { passes[passIdx].outputBuffers.push_back(h); }

    void RenderGraph::write(uint32_t passIdx, RenderTargetHandle h) { passes[passIdx].outputRenderTarget = h; }

    TextureHandle PassBuilder::createTexture(const TextureDesc &desc)
    {
        return graph.createTexture(desc);
    }

    BufferHandle PassBuilder::createBuffer(const BufferDesc &desc)
    {
        return graph.createBuffer(desc);
    }

    RenderTargetHandle PassBuilder::createRenderTarget(const RenderTargetDesc &desc)
    {
        return graph.createRenderTarget(desc);
    }

    void PassBuilder::read(TextureHandle handle) { graph.read(currentPass, handle); }

    void PassBuilder::read(BufferHandle handle) { graph.read(currentPass, handle); }

    void PassBuilder::read(RenderTargetHandle handle) { graph.read(currentPass, handle); }

    void PassBuilder::write(TextureHandle handle) { graph.write(currentPass, handle); }

    void PassBuilder::write(BufferHandle handle) { graph.write(currentPass, handle); }

    void PassBuilder::write(RenderTargetHandle handle) { graph.write(currentPass, handle); }

}