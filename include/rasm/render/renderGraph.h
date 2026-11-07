// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/render/resource.h"

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace rasm
{
    class Engine;
    class RenderContext;

    struct Pass
    {
        std::string                         name;
        std::vector<TextureHandle>          inputTextures;
        std::vector<BufferHandle>           inputBuffers;
        std::vector<RenderTargetHandle>     inputRenderTargets;
        std::vector<TextureHandle>          outputTextures;
        std::vector<BufferHandle>           outputBuffers;
        RenderTargetHandle                  outputRenderTarget;
        std::function<void(RenderContext&)> execute;

        // For Sorting
        uint32_t                inDegree = 0;
        std::vector<uint32_t>   successors; // Pass indices that depend on this one
    };

    struct ResourceMetadata
    {
        uint32_t producerIdx = static_cast<uint32_t>(-1); // Which pass writes this resource?
    };

    class RenderGraph;

    class PassBuilder
    {
    public:
                                PassBuilder(RenderGraph& g, uint32_t passIdx) : graph(g), currentPass(passIdx) {}
        TextureHandle           createTexture(const TextureDesc& desc);
        BufferHandle            createBuffer(const BufferDesc& desc);
        RenderTargetHandle      createRenderTarget(const RenderTargetDesc& desc);
        void                    read(TextureHandle handle);
        void                    read(BufferHandle handle);
        void                    read(RenderTargetHandle handle);
        void                    write(TextureHandle handle);
        void                    write(BufferHandle handle);
        void                    write(RenderTargetHandle rt);

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
        void execute(CameraHandle camera);

    protected:
        void                analyzeDependencies();
        TextureHandle       createTexture(const TextureDesc& desc);
        BufferHandle        createBuffer(const BufferDesc& desc);
        RenderTargetHandle  createRenderTarget(const RenderTargetDesc& desc);
        void                read(uint32_t passIdx, TextureHandle h);
        void                read(uint32_t passIdx, BufferHandle h);
        void                read(uint32_t passIdx, RenderTargetHandle h);
        void                write(uint32_t passIdx, TextureHandle h);
        void                write(uint32_t passIdx, BufferHandle h);
        void                write(uint32_t passIdx, RenderTargetHandle h);

        std::vector<Pass>           passes;
        std::vector<uint32_t>       executionOrder; // Sorted pass indices
        Engine*                     engine   = nullptr;
        bool                        compiled = false;
    };
}
// clang-format on