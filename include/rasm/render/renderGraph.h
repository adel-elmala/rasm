// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/render/resource.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

namespace rasm
{
    class Engine;
    class RenderContext;

    struct FrameBuffers
    {
        BufferHandle                        slots[MAX_FRAMES_IN_FLIGHT];
    };

    struct Pass
    {
        std::string                         name;
        std::vector<TextureHandle>          inputTextures;
        std::vector<BufferHandle>           inputBuffers;
        std::vector<BufferHandle>           previousBuffers;
        std::vector<RenderTargetHandle>     inputRenderTargets;
        std::vector<RenderTargetHandle>     previousRenderTargets;
        std::vector<RenderTargetHandle>     depthRenderTargets;
        std::vector<TextureHandle>          outputTextures;
        std::vector<BufferHandle>           outputBuffers;
        std::vector<FrameBuffers>           frameInputBuffers;
        std::vector<FrameBuffers>           frameOutputBuffers;
        RenderTargetHandle                  outputRenderTarget;
        std::function<void(RenderContext&)> execute;
        AttachmentOps                       attachmentOps;
        bool                                compute             = false;
        bool                                async               = false;
        bool                                writesSwapchain     = false;
        bool                                readsSwapchainDepth = false;

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
        void                    read(const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]);
        void                    readPrevious(RenderTargetHandle handle);
        void                    readPrevious(BufferHandle handle);
        void                    readDepth(RenderTargetHandle handle);
        void                    readSwapchainDepth();
        void                    write(TextureHandle handle);
        void                    write(BufferHandle handle);
        void                    write(RenderTargetHandle rt);
        void                    write(const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]);
        void                    writeSwapchain();

    private:
        RenderGraph&    graph;
        uint32_t        currentPass;
    };

    class RenderGraph
    {
        friend class PassBuilder;
        friend class Engine;
    public:
                           RenderGraph(Engine* owner) : engine(owner) {}
                           RenderGraph(const RenderGraph&) = delete;
                           RenderGraph(RenderGraph&& other) noexcept;
                           ~RenderGraph();
        RenderGraph&       operator=(const RenderGraph&) = delete;
        RenderGraph&       operator=(RenderGraph&& other) noexcept;
        void               addPass(std::string name,
                                   const std::function<void(PassBuilder&)>& setup,
                                   std::function<void(RenderContext&)> execute);
        void               addComputePass(std::string name,
                                          const std::function<void(PassBuilder&)>& setup,
                                          std::function<void(RenderContext&)> execute);
        void               addAsyncComputePass(std::string name,
                                               const std::function<void(PassBuilder&)>& setup,
                                               std::function<void(RenderContext&)> execute);
        RenderTargetHandle createRenderTarget(const RenderTargetDesc& desc);

        void               compile();
        void               execute(CameraHandle camera);

    protected:
        struct QueueUse
        {
            PipelineStage   writeStages = {};
            PipelineStage   readStages  = {};
            uint64_t        value       = 0;
        };

        struct Access
        {
            QueueUse        queues[QUEUE_COUNT];
        };

        void                analyzeDependencies();
        bool                bufferBarrier(const Pass& pass, QueueType queue, PipelineStage& before, PipelineStage& after);
        void                waitForOtherQueue(const Access& access, QueueType queue, bool writes);
        void                textureWaits(const Pass& pass, QueueType queue);
        void                destroyTransients();
        void                releaseSceneViews();
        bool                placeTransients();
        bool                transientLifetimes(const std::vector<uint32_t>& order);
        void                assignAttachmentOps(const std::vector<uint32_t>& order);
        TextureHandle       createTexture(const TextureDesc& desc);
        BufferHandle        createBuffer(const BufferDesc& desc);
        void                read(uint32_t passIdx, TextureHandle h);
        void                read(uint32_t passIdx, BufferHandle h);
        void                read(uint32_t passIdx, RenderTargetHandle h);
        void                read(uint32_t passIdx, const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]);
        void                readPrevious(uint32_t passIdx, RenderTargetHandle h);
        void                readPrevious(uint32_t passIdx, BufferHandle h);
        void                readDepth(uint32_t passIdx, RenderTargetHandle h);
        void                readSwapchainDepth(uint32_t passIdx);
        void                write(uint32_t passIdx, TextureHandle h);
        void                write(uint32_t passIdx, BufferHandle h);
        void                write(uint32_t passIdx, RenderTargetHandle h);
        void                write(uint32_t passIdx, const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]);
        void                writeSwapchain(uint32_t passIdx);

        std::vector<Pass>                                           passes;
        std::vector<uint32_t>                                       executionOrder; // Sorted pass indices
        std::unordered_map<BufferHandle, Access, HandleHash>        bufferAccess;
        std::unordered_map<TextureHandle, Access, HandleHash>       textureAccess;
        std::vector<RenderTargetHandle>                             transientTargets;
        std::vector<uint32_t>                                       transientFirstPasses;
        std::vector<uint32_t>                                       transientLastPasses;
        bool                                                        transientsPlaced = false;
        std::vector<uint32_t>                                       sceneViews;     // Engine::addScenePasses's views
        Engine*                                                     engine   = nullptr;
        bool                                                        compiled = false;
    };
}
// clang-format on