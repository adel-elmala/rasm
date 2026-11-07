#include "rasm/render/renderGraph.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>

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

        // 0. Barriers come only from what the passes declare, so reject what they can't have declared right.
        // Both fail the compile like a cycle does, so the frames draw no passes.
        bool valid = true;
        for (const auto &pass : passes)
        {
            // Its color would be sampled while it is an attachment.
            if (pass.outputRenderTarget.isValid() && std::find(pass.inputRenderTargets.begin(), pass.inputRenderTargets.end(), pass.outputRenderTarget) != pass.inputRenderTargets.end())
            {
                spdlog::error("Pass '{}' reads the render target it draws to.", pass.name);
                valid = false;
            }
            // Compute writes only buffers, so a compute pass that declares no write is missing its buffers.
            if (pass.compute && pass.outputBuffers.empty())
            {
                spdlog::error("Compute pass '{}' declares no buffer write; declare every buffer it uses.", pass.name);
                valid = false;
            }
        }
        if (!valid)
        {
            executionOrder.clear();
            return;
        }

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

        auto commandBuffer = engine->getCurrentFrameResources().commandBuffer;
        for (const auto &passIdx : executionOrder)
        {
            auto &pass = passes[passIdx];

            // Barriers are not allowed inside a render pass, so all of them go before the pass begins. beginPass
            // moves the attachments it draws to, and endPass leaves a render target's color SAMPLED for its readers.
            PipelineStage before = {};
            PipelineStage after = {};
            if (bufferBarrier(pass, before, after))
                ctx.memoryBarrier(commandBuffer, before, after);
            for (auto texture : pass.inputTextures)
                ctx.transitionImageLayout(commandBuffer, texture, TextureUsage::SAMPLED);

            if (pass.compute)
            {
                const uint32_t timer = engine->beginGpuTimer(commandBuffer, pass.name);
                pass.execute(ctx);
                engine->endGpuTimer(commandBuffer, timer);
                continue;
            }
            engine->setRenderTarget(pass.outputRenderTarget);
            engine->beginPass(pass.name);
            pass.execute(ctx);
            engine->endPass();
            engine->resetRenderTarget();
        }
        engine->endFrame();
    }

    // Shaders reach buffers through device addresses, so one global barrier covers all of a pass's buffers.
    // The state carries over to the next frame: a write waits for the reads of the frame before, and a read
    // waits for a write at the end of the frame before. Barriers cover earlier submissions on the queue.
    bool RenderGraph::bufferBarrier(const Pass &pass, PipelineStage &before, PipelineStage &after)
    {
        const PipelineStage stage = pass.compute ? PipelineStage::COMPUTE : PipelineStage::GRAPHICS;
        before = {};
        after = {};

        // A write is made visible to each stage once, so the next read in the same stage needs no barrier.
        for (auto buffer : pass.inputBuffers)
        {
            auto &access = bufferAccess[buffer];
            if (access.writeStages != PipelineStage{} && !hasStage(access.readStages, stage))
            {
                before = before | access.writeStages;
                after = after | stage;
            }
            access.readStages = access.readStages | stage;
        }

        // A write waits for every use since the last write, reads included.
        for (auto buffer : pass.outputBuffers)
        {
            auto &access = bufferAccess[buffer];
            const PipelineStage used = access.writeStages | access.readStages;
            if (used != PipelineStage{})
            {
                before = before | used;
                after = after | stage;
            }
            access = {.writeStages = stage};
        }
        return after != PipelineStage{};
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