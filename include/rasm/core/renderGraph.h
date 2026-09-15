// clang-format off
#pragma once

#include <string>
#include <vector>
#include <functional>

#include "spdlog/spdlog.h"

#include "types.h"

namespace rasm
{
    class Engine;
    class RenderContext;

    struct Pass
    {
        std::string                         name;
        std::vector<ResourceHandle>         inputs;
        std::vector<ResourceHandle>         outputs;
        std::function<void(RenderContext&)> execute;

        // For Sorting
        uint32_t                inDegree = 0; 
        std::vector<uint32_t>   successors; // Pass indices that depend on this one
    };

    struct ResourceMetadata {
        uint32_t producerIdx = static_cast<uint32_t>(-1); // Which pass writes this resource?
    };

    class RenderGraph;
    
    class PassBuilder {
    public:
             PassBuilder(RenderGraph& g, uint32_t passIdx) : graph(g), currentPass(passIdx) {}
             ResourceHandle createResource(ResourceDesc desc);
        void read(ResourceHandle handle);
        void write(ResourceHandle handle);

    private:
        RenderGraph&    graph;
        uint32_t        currentPass;
    };

    class RenderGraph
    {
        friend class PassBuilder;
    public:
             RenderGraph(Engine* owner) : engine(owner) {}
             ~RenderGraph() = default;
        void addPass(std::string name,
                 std::function<void(PassBuilder&)> setup, 
                 std::function<void(RenderContext&)> execute);
        void compile();
        void execute();

    protected:
        void            analyzeDependencies();
        ResourceHandle  internalCreate(ResourceDesc desc);
        void            internalRead(uint32_t passIdx, ResourceHandle h);
        void            internalWrite(uint32_t passIdx, ResourceHandle h);
    
        std::vector<Pass>           passes;
        std::vector<uint32_t>       executionOrder; // Sorted pass indices
        Engine*                     engine   = nullptr;
        bool                        compiled = false;
    };
}
// clang-format on