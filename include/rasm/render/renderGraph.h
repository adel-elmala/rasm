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
        std::vector<FrameBuffers>           framePreviousBuffers;       // the slot the frame before wrote
        std::vector<FrameBuffers>           frameOutputBuffers;
        RenderTargetHandle                  outputRenderTarget;
        uint32_t                            outputFace          = 0;    // of a cube target
        std::function<void(RenderContext&)> execute;
        std::string                         timerName;                  // compute passes next to each other with one name share a GPU timer
        AttachmentOps                       attachmentOps;
        bool                                compute             = false;
        bool                                async               = false;
        bool                                writesSwapchain     = false;
        WindowHandle                        window;                     // the swapchain's window: the main one when the graph has an engine
        bool                                encodedSwapchain    = false;    // draws the swapchain through its UNORM view (AttachmentOps::encodedColor): the UI pass

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
        void                    readPrevious(const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]);
        void                    readDepth(RenderTargetHandle handle);
        void                    write(TextureHandle handle);
        void                    write(BufferHandle handle);
        void                    write(RenderTargetHandle rt, uint32_t face = 0);
        void                    write(const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]);
        // This frame's image of the window: an invalid handle is the main window. Passes on one window run in the
        // order they were added; a pass whose window has no image this frame (minimized, removed) is skipped.
        void                    writeSwapchain(WindowHandle window = {});

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
        std::string        dump() const;

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
        void                removePass(uint32_t passIdx);
        bool                bufferBarrier(const Pass& pass, QueueType queue, PipelineStage& before, PipelineStage& after);
        void                waitForOtherQueue(const Access& access, QueueType queue, bool writes);
        void                textureWaits(const Pass& pass, QueueType queue);
        void                destroyTargets();
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
        void                readPrevious(uint32_t passIdx, const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]);
        void                readDepth(uint32_t passIdx, RenderTargetHandle h);
        void                write(uint32_t passIdx, TextureHandle h);
        void                write(uint32_t passIdx, BufferHandle h);
        void                write(uint32_t passIdx, RenderTargetHandle h, uint32_t face);
        void                write(uint32_t passIdx, const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]);
        void                writeSwapchain(uint32_t passIdx, WindowHandle window);

        std::vector<Pass>                                           passes;
        std::vector<uint32_t>                                       executionOrder; // Sorted pass indices
        std::unordered_map<BufferHandle, Access, HandleHash>        bufferAccess;
        std::unordered_map<TextureHandle, Access, HandleHash>       textureAccess;
        std::vector<RenderTargetHandle>                             transientTargets;
        std::vector<RenderTargetHandle>                             historyTargets;     // made by the graph with history or readable: they live as long as it
        std::vector<uint32_t>                                       transientFirstPasses;
        std::vector<uint32_t>                                       transientLastPasses;
        uint64_t                                                    transientUses[QUEUE_COUNT] = {}; // per queue, the batch value of this frame's last pass that used a transient target
        bool                                                        transientsPlaced = false;
        std::vector<uint32_t>                                       sceneViews;     // Engine::addScenePasses's views
        bool                                                        animationPass = false;  // Engine::addAnimationPass ran
        uint32_t                                                    inspectPass = UINT32_MAX;   // Engine::updateInspectPass's: it runs right after the last pass it reads from
        Engine*                                                     engine   = nullptr;
        bool                                                        compiled = false;
    };
}
// clang-format on