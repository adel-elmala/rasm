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
        std::unordered_map<ResourceHandle, uint32_t, HandleHash> resourceToProducer;
        for (uint32_t i = 0, nPasses = static_cast<uint32_t>(passes.size()); i < nPasses; ++i)
        {
            auto &pass = passes[i];
            for (auto outHandle : pass.outputs)
            {
                resourceToProducer[outHandle] = i;
            }
        }

        // 2. Build the Adjacency List (Edges)
        for (uint32_t i = 0, nPasses = static_cast<uint32_t>(passes.size()); i < nPasses; ++i)
        {
            for (auto inHandle : passes[i].inputs)
            {
                uint32_t producerIdx = resourceToProducer[inHandle];
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
            engine->beginPass();
            passes[passIdx].execute(ctx);
            engine->endPass();
        }
        engine->endFrame();
    }

    // Internal helpers for the Builder
    ResourceHandle RenderGraph::internalCreate(ResourceDesc desc)
    {
        auto handle = engine->createResource(desc);
        return handle;
    }

    void RenderGraph::internalRead(uint32_t passIdx, ResourceHandle h) { passes[passIdx].inputs.push_back(h); }

    void RenderGraph::internalWrite(uint32_t passIdx, ResourceHandle h) { passes[passIdx].outputs.push_back(h); }

    ResourceHandle PassBuilder::createResource(ResourceDesc desc) { return graph.internalCreate(desc); }

    void PassBuilder::read(ResourceHandle handle) { graph.internalRead(currentPass, handle); }

    void PassBuilder::write(ResourceHandle handle) { graph.internalWrite(currentPass, handle); }
}