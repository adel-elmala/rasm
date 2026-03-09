#include "spdlog/spdlog.h"
#include <unordered_map>
#include <queue>

#include "../include/rasm/core/renderGraph.h"

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
        if (compiled) return;

        // 1. Map resources to their producers
        std::vector<ResourceMetadata> resourceMeta(resources.size());
        for (uint32_t i = 0, nPasses = static_cast<uint32_t>(passes.size()); i < nPasses; ++i)
        {
            for (auto outHandle : passes[i].outputs)
            {
                resourceMeta[outHandle.index].producerIdx = i;
            }
        }

        // 2. Build the Adjacency List (Edges)
        for (uint32_t i = 0, nPasses = static_cast<uint32_t>(passes.size()); i < nPasses; ++i)
        {
            for (auto inHandle : passes[i].inputs)
            {
                uint32_t producerIdx = resourceMeta[inHandle.index].producerIdx;
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
            throw std::runtime_error("Frame Graph contains a cycle!");
        }

        // 5. Store the final execution order
        executionOrder = std::move(sortedIndices);
        compiled = true;
        spdlog::info("Successfully compiled {} passes.", executionOrder.size());
    }

    void RenderGraph::execute()
    {
        if (!compiled) compile();

        auto ctx = RenderContext{};
        for (const auto &passIdx : executionOrder)
        {
            passes[passIdx].execute(ctx);
        }
    }

    // Internal helpers for the Builder
    ResourceHandle RenderGraph::internalCreate(ResourceDesc desc) {
        resources.push_back(desc);
        return {resources.size() - 1, 1};
    }

    void RenderGraph::internalRead(uint32_t passIdx, ResourceHandle h) { passes[passIdx].inputs.push_back(h); }

    void RenderGraph::internalWrite(uint32_t passIdx, ResourceHandle h) { passes[passIdx].outputs.push_back(h); }

    ResourceHandle PassBuilder::createTexture(ResourceDesc desc) { return graph.internalCreate(desc); }

    void PassBuilder::read(ResourceHandle handle) { graph.internalRead(currentPass, handle); }

    void PassBuilder::write(ResourceHandle handle) { graph.internalWrite(currentPass, handle); }
}