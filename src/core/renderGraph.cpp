#include "rasm/core/renderGraph.h"
#include "rasm/core/engine.h"

#include "spdlog/spdlog.h"

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
            compiled = false;
            return;
        }

        // 5. Store the final execution order
        executionOrder = std::move(sortedIndices);
        compiled = true;
        spdlog::info("Successfully compiled {} passes.", executionOrder.size());
    }

    void RenderGraph::execute()
    {
        if (!compiled)
            compile();

        auto &ctx = engine->getRenderContext();

        auto scene = engine->registery.scenes[engine->currentScene.index];
        auto camera = scene.cameras[0]; // TODO: handle multi-cameras

        engine->beginFrame(camera);
        for (const auto &passIdx : executionOrder)
        {
            auto &pass = passes[passIdx];
            engine->setRenderTarget(pass.outputRenderTarget);
            engine->beginPass();
            pass.execute(ctx);
            engine->endPass();
            engine->resetRenderTarget();
        }
        engine->endFrame();
    }

    TextureHandle RenderGraph::CreateTexture(ResourceDesc desc)
    {
        auto handle = engine->createTexture(desc);
        return handle;
    }

    BufferHandle RenderGraph::CreateBuffer(ResourceDesc desc)
    {
        auto handle = engine->createBuffer(desc);
        return handle;
    }

    RenderTargetHandle RenderGraph::CreateRenderTarget(ResourceDesc desc)
    {
        auto handle = engine->createRenderTarget(desc);
        return handle;
    }

    void RenderGraph::Read(uint32_t passIdx, TextureHandle h) { passes[passIdx].inputTextures.push_back(h); }

    void RenderGraph::Read(uint32_t passIdx, BufferHandle h) { passes[passIdx].inputBuffers.push_back(h); }

    void RenderGraph::Read(uint32_t passIdx, RenderTargetHandle h) { passes[passIdx].inputRenderTargets.push_back(h); }

    void RenderGraph::Write(uint32_t passIdx, TextureHandle h) { passes[passIdx].outputTextures.push_back(h); }

    void RenderGraph::Write(uint32_t passIdx, BufferHandle h) { passes[passIdx].outputBuffers.push_back(h); }

    void RenderGraph::Write(uint32_t passIdx, RenderTargetHandle h) { passes[passIdx].outputRenderTarget = h; }

    TextureHandle PassBuilder::createTexture(ResourceDesc desc)
    {
        if (desc.type != ResourceType::TEXTURE)
        {
            spdlog::error("Attempted to create a texture with a non-texture resource description.");
            return TextureHandle{};
        }

        return graph.CreateTexture(desc);
    }

    BufferHandle PassBuilder::createBuffer(ResourceDesc desc)
    {
        if (desc.type != ResourceType::BUFFER)
        {
            spdlog::error("Attempted to create a buffer with a non-buffer resource description.");
            return BufferHandle{};
        }

        return graph.CreateBuffer(desc);
    }

    RenderTargetHandle PassBuilder::createRenderTarget(ResourceDesc desc)
    {
        if (desc.type != ResourceType::RENDER_TARGET)
        {
            spdlog::error("Attempted to create a render target with a non-render target resource description.");
            return RenderTargetHandle{};
        }

        return graph.CreateRenderTarget(desc);
    }

    void PassBuilder::read(TextureHandle handle) { graph.Read(currentPass, handle); }

    void PassBuilder::read(BufferHandle handle) { graph.Read(currentPass, handle); }

    void PassBuilder::read(RenderTargetHandle handle) { graph.Read(currentPass, handle); }

    void PassBuilder::write(TextureHandle handle) { graph.Write(currentPass, handle); }

    void PassBuilder::write(BufferHandle handle) { graph.Write(currentPass, handle); }

    void PassBuilder::write(RenderTargetHandle handle) { graph.Write(currentPass, handle); }

}