#pragma once

#include <string>
#include <vector>
#include <functional>

#include "spdlog/spdlog.h"

#include "types.h"

namespace rasm
{
    // The "Context" provided during execution to get real physical resources
    struct RenderContext {
        void DrawMesh(std::string name) { spdlog::info("  [GPU] Drawing: {}", name); }
    };

    struct ResourceDesc
    {
        std::string name;
        enum class Type
        {
            Texture,
            Buffer,
            // Add more types as needed
        } type;

        union {
            // Texture-specific data
            struct
            {
                uint32_t width;
                uint32_t height;
                uint32_t format; // e.g., RGBA8, RGBA16F
            } texture;

            // Buffer-specific data
            struct
            {
                uint64_t size;
                uint64_t stride;
            } buffer;
        };
    };

    using ResourceHandle = Handle<ResourceDesc>;

    struct Pass
    {
        std::string name;
        std::vector<ResourceHandle> inputs;
        std::vector<ResourceHandle> outputs;
        std::function<void(RenderContext&)> execute;

        // For Sorting
        uint32_t inDegree = 0; 
        std::vector<uint32_t> successors; // Pass indices that depend on this one
    };

    struct ResourceMetadata {
        uint32_t producerIdx = -1; // Which pass writes this resource?
    };

    class RenderGraph;
    class PassBuilder {
    public:
        PassBuilder(RenderGraph& g, uint32_t passIdx) : graph(g), currentPass(passIdx) {}

        ResourceHandle createTexture(ResourceDesc desc);
        void read(ResourceHandle handle);
        void write(ResourceHandle handle);

    private:
        RenderGraph& graph;
        uint32_t currentPass;
    };

    class RenderGraph
    {
        friend class PassBuilder;
    public:
        RenderGraph() = default;
        ~RenderGraph() = default;

        void addPass(std::string name, 
                 std::function<void(PassBuilder&)> setup, 
                 std::function<void(RenderContext&)> execute);

        void compile();
        void execute();
    protected:
        void analyzeDependencies();
        ResourceHandle internalCreate(ResourceDesc desc);
        void internalRead(uint32_t passIdx, ResourceHandle h);
        void internalWrite(uint32_t passIdx, ResourceHandle h);


        std::vector<Pass> passes;
        std::vector<uint32_t> executionOrder; // Sorted pass indices
        std::vector<ResourceDesc> resources;
        bool compiled = false;
    };
}   