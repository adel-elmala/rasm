#include "rasm/render/renderGraph.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <utility>

namespace rasm
{
    namespace
    {
        enum class ResourceKind : uint32_t
        {
            TEXTURE,
            BUFFER,
            RENDER_TARGET,
            SWAPCHAIN,
        };

        struct ResourceKey
        {
            ResourceKind kind = ResourceKind::TEXTURE;
            uint64_t index = 0;
            uint64_t generation = 0;

            bool operator==(const ResourceKey &) const = default;
        };

        struct ResourceKeyHash
        {
            size_t operator()(const ResourceKey &key) const
            {
                return hashCombine(hashCombine(static_cast<size_t>(key.kind), std::hash<uint64_t>()(key.index)), std::hash<uint64_t>()(key.generation));
            }
        };
    }

    namespace
    {
        constexpr uint32_t NO_PASS = UINT32_MAX;

        void addEdge(std::vector<Pass> &passes, uint32_t from, uint32_t to)
        {
            passes[from].successors.push_back(to);
            passes[to].inDegree++;
        }

        ResourceKey keyOf(TextureHandle handle) { return {.kind = ResourceKind::TEXTURE, .index = handle.index, .generation = handle.generation}; }

        ResourceKey keyOf(BufferHandle handle) { return {.kind = ResourceKind::BUFFER, .index = handle.index, .generation = handle.generation}; }

        ResourceKey keyOf(RenderTargetHandle handle) { return {.kind = ResourceKind::RENDER_TARGET, .index = handle.index, .generation = handle.generation}; }

        // A depth read orders like a read of the whole target.
        void collectAccesses(const Pass &pass, std::vector<ResourceKey> &reads, std::vector<ResourceKey> &writes)
        {
            const ResourceKey swapchain = {.kind = ResourceKind::SWAPCHAIN};
            for (auto handle : pass.inputTextures)
                reads.push_back(keyOf(handle));
            for (auto handle : pass.inputBuffers)
                reads.push_back(keyOf(handle));
            for (auto handle : pass.inputRenderTargets)
                reads.push_back(keyOf(handle));
            for (auto handle : pass.depthRenderTargets)
                reads.push_back(keyOf(handle));
            if (pass.readsSwapchainDepth)
                reads.push_back(swapchain);
            for (auto handle : pass.outputTextures)
                writes.push_back(keyOf(handle));
            for (auto handle : pass.outputBuffers)
                writes.push_back(keyOf(handle));
            if (pass.outputRenderTarget.isValid())
                writes.push_back(keyOf(pass.outputRenderTarget));
            if (pass.writesSwapchain)
                writes.push_back(swapchain);
        }

        // Depth textures are sampleable only with occlusion culling (TextureUsage::SAMPLED_DEPTH_ATTACHMENT).
        bool canReadDepth(const Engine *engine, const Pass &pass)
        {
            if (!engine || engine->getConfig().occlusionCulling)
                return true;
            spdlog::error("Pass '{}' reads a depth texture, which needs EngineConfig::occlusionCulling; the read is ignored.", pass.name);
            return false;
        }
    }

    RenderGraph::RenderGraph(RenderGraph &&other) noexcept
    {
        *this = std::move(other);
    }

    // The transient targets' memory and the hold on its scene views are the graph's, so they go with the graph.
    RenderGraph::~RenderGraph()
    {
        destroyTransients();
        releaseSceneViews();
    }

    // The moved-from graph keeps no transient targets or scene views, so its destructor frees nothing.
    RenderGraph &RenderGraph::operator=(RenderGraph &&other) noexcept
    {
        if (this == &other)
            return *this;
        destroyTransients();
        releaseSceneViews();
        passes = std::move(other.passes);
        executionOrder = std::move(other.executionOrder);
        bufferAccess = std::move(other.bufferAccess);
        transientTargets = std::exchange(other.transientTargets, {});
        transientFirstPasses = std::move(other.transientFirstPasses);
        transientLastPasses = std::move(other.transientLastPasses);
        transientsPlaced = other.transientsPlaced;
        sceneViews = std::exchange(other.sceneViews, {});
        engine = other.engine;
        compiled = other.compiled;
        return *this;
    }

    void RenderGraph::destroyTransients()
    {
        if (engine && !transientTargets.empty())
            engine->getRenderContext().destroyTransientRenderTargets(transientTargets.data(), static_cast<uint32_t>(transientTargets.size()));
        transientTargets.clear();
    }

    void RenderGraph::releaseSceneViews()
    {
        if (engine)
        {
            for (uint32_t view : sceneViews)
                engine->releaseSceneView(view);
        }
        sceneViews.clear();
    }

    void RenderGraph::addPass(std::string name, const std::function<void(PassBuilder &)> &setup, std::function<void(RenderContext &)> execute)
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

    void RenderGraph::addComputePass(std::string name, const std::function<void(PassBuilder &)> &setup, std::function<void(RenderContext &)> execute)
    {
        addPass(std::move(name), setup, std::move(execute));
        auto &pass = passes.back();
        pass.compute = true;
        // A compute pass runs outside any render pass, so it has no attachments to write.
        if (pass.outputRenderTarget.isValid() || pass.writesSwapchain)
        {
            spdlog::error("Compute pass '{}' cannot write a render target or the swapchain; the write is ignored.", pass.name);
            pass.outputRenderTarget = {};
            pass.writesSwapchain = false;
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
            // Its color or depth would be sampled while it is an attachment.
            const bool readsColor = std::find(pass.inputRenderTargets.begin(), pass.inputRenderTargets.end(), pass.outputRenderTarget) != pass.inputRenderTargets.end();
            const bool readsDepth = std::find(pass.depthRenderTargets.begin(), pass.depthRenderTargets.end(), pass.outputRenderTarget) != pass.depthRenderTargets.end();
            if ((pass.outputRenderTarget.isValid() && (readsColor || readsDepth)) || (pass.writesSwapchain && pass.readsSwapchainDepth))
            {
                spdlog::error("Pass '{}' reads the render target it draws to.", pass.name);
                valid = false;
            }
            // execute begins the pass on what it writes, so it must be exactly one target.
            if (!pass.compute && pass.outputRenderTarget.isValid() == pass.writesSwapchain)
            {
                spdlog::error("Pass '{}' must write either one render target or the swapchain.", pass.name);
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

        // 1. The writers of each resource, in the order the passes were added.
        std::vector<std::vector<ResourceKey>> reads(passes.size());
        std::unordered_map<ResourceKey, std::vector<uint32_t>, ResourceKeyHash> writers;
        for (uint32_t i = 0; i < passes.size(); ++i)
        {
            std::vector<ResourceKey> writes;
            collectAccesses(passes[i], reads[i], writes);
            for (const auto &key : writes)
            {
                auto &list = writers[key];
                if (list.empty() || list.back() != i)
                    list.push_back(i);
            }
        }

        // 2. The writers of a resource run in the order they were added. A reader runs after the last writer added
        // before it, and before the writer after that one. A reader added before every writer reads the last one,
        // unless it writes the resource itself: then it reads what the resource held before this frame's writes.
        for (const auto &[key, list] : writers)
        {
            for (size_t k = 1; k < list.size(); ++k)
                addEdge(passes, list[k - 1], list[k]);
        }
        for (uint32_t i = 0; i < passes.size(); ++i)
        {
            for (const auto &key : reads[i])
            {
                auto it = writers.find(key);
                if (it == writers.end())
                    continue;
                const auto &list = it->second;
                const auto next = std::lower_bound(list.begin(), list.end(), i);
                const bool writesItToo = next != list.end() && *next == i;
                uint32_t producer = NO_PASS;
                if (next != list.begin())
                    producer = *(next - 1);
                else if (!writesItToo)
                    producer = list.back();
                if (producer != NO_PASS && producer != i)
                    addEdge(passes, producer, i);
                if (next != list.begin() && next != list.end() && !writesItToo)
                    addEdge(passes, i, *next);
            }

            // A buffer has one copy, so what the frame before wrote is there only until this frame's first write.
            for (auto handle : passes[i].previousBuffers)
            {
                auto it = writers.find(keyOf(handle));
                if (it != writers.end() && it->second.front() != i)
                    addEdge(passes, i, it->second.front());
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

        // The transient memory is laid out from the lifetimes, so it is laid out again only when they change.
        const std::vector<uint32_t> firstPasses = transientFirstPasses;
        const std::vector<uint32_t> lastPasses = transientLastPasses;
        if (!transientLifetimes(sortedIndices))
        {
            executionOrder.clear();
            return;
        }
        if (transientFirstPasses != firstPasses || transientLastPasses != lastPasses)
            transientsPlaced = false;

        assignAttachmentOps(sortedIndices);

        // 5. Store the final execution order.
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
        const bool placed = compiled && placeTransients();

        if (!engine->beginFrame(camera))
            return;

        auto commandBuffer = engine->getCurrentFrameResources().commandBuffer;
        const uint32_t passCount = placed ? static_cast<uint32_t>(executionOrder.size()) : 0;
        for (uint32_t position = 0; position < passCount; ++position)
        {
            auto &pass = passes[executionOrder[position]];

            // Barriers are not allowed inside a render pass, so all of them go before the pass begins. beginPass
            // moves the attachments it draws to, and endPass leaves a render target's color SAMPLED for its readers.
            // A depth read moves the depth here, and the next pass that draws the target moves it back.
            // A transient target's first pass draws over memory that other targets used before, last frame's included.
            if (std::find(transientFirstPasses.begin(), transientFirstPasses.end(), position) != transientFirstPasses.end())
                ctx.aliasingBarrier(commandBuffer);
            PipelineStage before = {};
            PipelineStage after = {};
            if (bufferBarrier(pass, before, after))
                ctx.memoryBarrier(commandBuffer, before, after);
            for (auto texture : pass.inputTextures)
                ctx.transitionImageLayout(commandBuffer, texture, TextureUsage::SAMPLED);
            for (auto renderTarget : pass.depthRenderTargets)
                ctx.transitionImageLayout(commandBuffer, engine->getRenderTargetDepthTexture(renderTarget), TextureUsage::SAMPLED);
            if (pass.readsSwapchainDepth)
                ctx.transitionImageLayout(commandBuffer, engine->getCurrentFrameResources().depthTexture, TextureUsage::SAMPLED);

            if (pass.compute)
            {
                const uint32_t timer = engine->beginGpuTimer(commandBuffer, pass.name);
                pass.execute(ctx);
                engine->endGpuTimer(commandBuffer, timer);
                continue;
            }
            engine->setRenderTarget(pass.outputRenderTarget);
            engine->beginPass(pass.name, pass.attachmentOps);
            pass.execute(ctx);
            engine->endPass();
            engine->resetRenderTarget();
        }
        engine->endFrame();
    }

    // A failed placement, here or at a resize, leaves targets without textures. It is tried again every frame, and
    // until it works the frames draw no passes.
    bool RenderGraph::placeTransients()
    {
        if (transientTargets.empty())
            return true;
        auto &ctx = engine->getRenderContext();
        const bool missing = std::any_of(transientTargets.begin(), transientTargets.end(), [&ctx](const RenderTargetHandle &target)
                                         { return !ctx.getRenderTarget(target).attachments[0].color[0].isValid(); });
        if (!transientsPlaced || missing)
            transientsPlaced = ctx.placeTransientRenderTargets(transientTargets.data(), transientFirstPasses.data(), transientLastPasses.data(), static_cast<uint32_t>(transientTargets.size()));
        return transientsPlaced;
    }

    // Passes that draw to one target run in the order they were added, so the first clears it, and each later one loads
    // what the one before drew, depth included, which that one must then store. So must the pass before a depth read.
    void RenderGraph::assignAttachmentOps(const std::vector<uint32_t> &order)
    {
        std::unordered_map<RenderTargetHandle, uint32_t, HandleHash> lastWriters;
        uint32_t lastSwapchainWriter = NO_PASS;
        for (uint32_t passIdx : order)
        {
            Pass &pass = passes[passIdx];
            pass.attachmentOps = {};
            for (auto renderTarget : pass.depthRenderTargets)
            {
                auto it = lastWriters.find(renderTarget);
                if (it != lastWriters.end() && it->second != NO_PASS)
                    passes[it->second].attachmentOps.storeDepth = true;
            }
            if (pass.readsSwapchainDepth && lastSwapchainWriter != NO_PASS)
                passes[lastSwapchainWriter].attachmentOps.storeDepth = true;
            if (pass.compute)
                continue;
            uint32_t &previous = pass.writesSwapchain ? lastSwapchainWriter : lastWriters.try_emplace(pass.outputRenderTarget, NO_PASS).first->second;
            if (previous != NO_PASS)
            {
                pass.attachmentOps.loadColor = true;
                pass.attachmentOps.loadDepth = true;
                passes[previous].attachmentOps.storeDepth = true;
            }
            previous = passIdx;
        }
    }

    // Each transient render target lives from the first pass in the order that uses it to the last, as positions in the
    // order. Its memory holds another target's data before that, so its first use must draw it.
    bool RenderGraph::transientLifetimes(const std::vector<uint32_t> &order)
    {
        const auto count = transientTargets.size();
        transientFirstPasses.assign(count, UINT32_MAX);
        transientLastPasses.assign(count, 0);
        std::unordered_map<RenderTargetHandle, uint32_t, HandleHash> transientIndex;
        for (uint32_t i = 0; i < count; ++i)
            transientIndex[transientTargets[i]] = i;

        bool valid = true;
        for (uint32_t position = 0; position < order.size(); ++position)
        {
            const Pass &pass = passes[order[position]];
            auto use = [&](RenderTargetHandle renderTarget, bool draws)
            {
                auto it = transientIndex.find(renderTarget);
                if (it == transientIndex.end())
                    return;
                if (transientFirstPasses[it->second] == UINT32_MAX)
                {
                    transientFirstPasses[it->second] = position;
                    if (!draws)
                    {
                        spdlog::error("Pass '{}' reads a transient render target that no pass before it draws.", pass.name);
                        valid = false;
                    }
                }
                transientLastPasses[it->second] = position;
            };
            for (auto renderTarget : pass.inputRenderTargets)
                use(renderTarget, false);
            for (auto renderTarget : pass.depthRenderTargets)
                use(renderTarget, false);
            if (pass.outputRenderTarget.isValid())
                use(pass.outputRenderTarget, true);
        }

        // A target no pass declares is still created, and kept for the whole frame.
        for (uint32_t i = 0; i < count; ++i)
        {
            if (transientFirstPasses[i] != UINT32_MAX)
                continue;
            transientFirstPasses[i] = 0;
            transientLastPasses[i] = order.empty() ? 0 : static_cast<uint32_t>(order.size() - 1);
        }
        return valid;
    }

    // Shaders reach buffers through device addresses, so one global barrier covers all of a pass's buffers.
    // The state carries over to the next frame: a write waits for the reads of the frame before, and a read
    // waits for a write at the end of the frame before. Barriers cover earlier submissions on the queue.
    bool RenderGraph::bufferBarrier(const Pass &pass, PipelineStage &before, PipelineStage &after)
    {
        const PipelineStage stage = pass.compute ? PipelineStage::COMPUTE : PipelineStage::GRAPHICS;
        before = {};
        after = {};

        // A write is made visible to each stage once, so the next read in the same stage needs no barrier. A read of
        // what the frame before wrote waits for that write like any other read.
        auto read = [&](BufferHandle buffer)
        {
            auto &access = bufferAccess[buffer];
            if (access.writeStages != PipelineStage{} && !hasStage(access.readStages, stage))
            {
                before = before | access.writeStages;
                after = after | stage;
            }
            access.readStages = access.readStages | stage;
        };
        for (auto buffer : pass.inputBuffers)
            read(buffer);
        for (auto buffer : pass.previousBuffers)
            read(buffer);

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

    // A history target lives across frames. Any other is transient: its memory is the graph's to share.
    RenderTargetHandle RenderGraph::createRenderTarget(const RenderTargetDesc &desc)
    {
        if (desc.history)
            return engine->createRenderTarget(desc);
        auto handle = engine->getRenderContext().createTransientRenderTarget(desc);
        if (handle.isValid())
        {
            transientTargets.push_back(handle);
            transientsPlaced = false;
        }
        return handle;
    }

    void RenderGraph::read(uint32_t passIdx, TextureHandle h) { passes[passIdx].inputTextures.push_back(h); }

    void RenderGraph::read(uint32_t passIdx, BufferHandle h) { passes[passIdx].inputBuffers.push_back(h); }

    void RenderGraph::read(uint32_t passIdx, RenderTargetHandle h) { passes[passIdx].inputRenderTargets.push_back(h); }

    // Last frame's copy is never written this frame, so the read adds no edge. The copy is always SAMPLED: the frame
    // before drew it, or it was cleared when it was made.
    void RenderGraph::readPrevious(uint32_t passIdx, BufferHandle h) { passes[passIdx].previousBuffers.push_back(h); }

    void RenderGraph::readPrevious(uint32_t passIdx, RenderTargetHandle h)
    {
        if (engine && !engine->getRenderContext().getRenderTarget(h).history)
        {
            spdlog::error("Pass '{}' reads the previous frame of a render target created without history; the read is ignored.", passes[passIdx].name);
            return;
        }
        passes[passIdx].previousRenderTargets.push_back(h);
    }

    void RenderGraph::readDepth(uint32_t passIdx, RenderTargetHandle h)
    {
        if (canReadDepth(engine, passes[passIdx]))
            passes[passIdx].depthRenderTargets.push_back(h);
    }

    void RenderGraph::readSwapchainDepth(uint32_t passIdx)
    {
        if (canReadDepth(engine, passes[passIdx]))
            passes[passIdx].readsSwapchainDepth = true;
    }

    void RenderGraph::write(uint32_t passIdx, TextureHandle h) { passes[passIdx].outputTextures.push_back(h); }

    void RenderGraph::write(uint32_t passIdx, BufferHandle h) { passes[passIdx].outputBuffers.push_back(h); }

    void RenderGraph::write(uint32_t passIdx, RenderTargetHandle h) { passes[passIdx].outputRenderTarget = h; }

    void RenderGraph::writeSwapchain(uint32_t passIdx) { passes[passIdx].writesSwapchain = true; }

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

    void PassBuilder::readPrevious(RenderTargetHandle handle) { graph.readPrevious(currentPass, handle); }

    void PassBuilder::readPrevious(BufferHandle handle) { graph.readPrevious(currentPass, handle); }

    void PassBuilder::readDepth(RenderTargetHandle handle) { graph.readDepth(currentPass, handle); }

    void PassBuilder::readSwapchainDepth() { graph.readSwapchainDepth(currentPass); }

    void PassBuilder::write(TextureHandle handle) { graph.write(currentPass, handle); }

    void PassBuilder::write(BufferHandle handle) { graph.write(currentPass, handle); }

    void PassBuilder::write(RenderTargetHandle handle) { graph.write(currentPass, handle); }

    void PassBuilder::writeSwapchain() { graph.writeSwapchain(currentPass); }

}