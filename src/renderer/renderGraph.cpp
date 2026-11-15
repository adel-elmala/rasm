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
            SWAPCHAIN_DEPTH, // only the dump keeps it apart: for ordering, a depth read reads the swapchain
        };

        struct ResourceKey
        {
            ResourceKind kind = ResourceKind::TEXTURE;
            uint64_t index = 0;
            uint64_t generation = 0;

            bool operator==(const ResourceKey &) const = default;
        };

        // One resource a pass uses. mark is W for a write, R for a read, P for a read of what the frame before left.
        // A per-frame buffer is one access per slot.
        struct PassAccess
        {
            ResourceKey key;
            char mark = 'R';
            uint32_t slot = 0;
            bool perFrame = false;
            bool depth = false;
        };

        // One row of the lifetime table in RenderGraph::dump: a mark per position in the order.
        struct LifetimeRow
        {
            std::string label;
            std::string marks;
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

        // Every resource the pass uses, reads before writes. Both the ordering and the dump walk this one list.
        std::vector<PassAccess> accessesOf(const Pass &pass)
        {
            const ResourceKey swapchain = {.kind = ResourceKind::SWAPCHAIN};
            std::vector<PassAccess> accesses;
            for (auto handle : pass.inputTextures)
                accesses.push_back({.key = keyOf(handle)});
            for (auto handle : pass.inputBuffers)
                accesses.push_back({.key = keyOf(handle)});
            for (const auto &buffers : pass.frameInputBuffers)
            {
                for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
                    accesses.push_back({.key = keyOf(buffers.slots[slot]), .slot = slot, .perFrame = true});
            }
            for (auto handle : pass.previousBuffers)
                accesses.push_back({.key = keyOf(handle), .mark = 'P'});
            for (auto handle : pass.inputRenderTargets)
                accesses.push_back({.key = keyOf(handle)});
            for (auto handle : pass.previousRenderTargets)
                accesses.push_back({.key = keyOf(handle), .mark = 'P'});
            for (auto handle : pass.depthRenderTargets)
                accesses.push_back({.key = keyOf(handle), .depth = true});
            if (pass.readsSwapchainDepth)
                accesses.push_back({.key = swapchain, .depth = true});
            for (auto handle : pass.outputTextures)
                accesses.push_back({.key = keyOf(handle), .mark = 'W'});
            for (auto handle : pass.outputBuffers)
                accesses.push_back({.key = keyOf(handle), .mark = 'W'});
            for (const auto &buffers : pass.frameOutputBuffers)
            {
                for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
                    accesses.push_back({.key = keyOf(buffers.slots[slot]), .mark = 'W', .slot = slot, .perFrame = true});
            }
            if (pass.outputRenderTarget.isValid())
                accesses.push_back({.key = keyOf(pass.outputRenderTarget), .mark = 'W'});
            if (pass.writesSwapchain)
                accesses.push_back({.key = swapchain, .mark = 'W'});
            return accesses;
        }

        // A depth read orders like a read of the whole target. A read of the frame before does not order.
        void collectAccesses(const Pass &pass, std::vector<ResourceKey> &reads, std::vector<ResourceKey> &writes)
        {
            for (const PassAccess &access : accessesOf(pass))
            {
                if (access.mark == 'W')
                    writes.push_back(access.key);
                else if (access.mark == 'R')
                    reads.push_back(access.key);
            }
        }

        QueueType otherQueue(QueueType queue)
        {
            return queue == QueueType::GRAPHICS ? QueueType::COMPUTE : QueueType::GRAPHICS;
        }

        bool usesTransient(const Pass &pass, const std::vector<RenderTargetHandle> &transientTargets)
        {
            auto transient = [&transientTargets](RenderTargetHandle renderTarget)
            { return std::find(transientTargets.begin(), transientTargets.end(), renderTarget) != transientTargets.end(); };
            return std::any_of(pass.inputRenderTargets.begin(), pass.inputRenderTargets.end(), transient) ||
                   std::any_of(pass.depthRenderTargets.begin(), pass.depthRenderTargets.end(), transient) ||
                   (pass.outputRenderTarget.isValid() && transient(pass.outputRenderTarget));
        }

        // An unnamed resource shows its handle as #index:generation. A graph without an engine (the tests) has no names.
        std::string labelOf(Engine *engine, const ResourceKey &key)
        {
            std::string kind;
            std::string name;
            RenderContext *ctx = engine ? &engine->getRenderContext() : nullptr;
            switch (key.kind)
            {
            case ResourceKind::TEXTURE:
                kind = "texture";
                name = ctx ? ctx->getTextureDesc(TextureHandle{key.index, key.generation}).name : "";
                break;
            case ResourceKind::BUFFER:
                kind = "buffer";
                name = ctx ? ctx->getBufferDesc(BufferHandle{key.index, key.generation}).name : "";
                break;
            case ResourceKind::RENDER_TARGET:
                kind = "target";
                name = ctx ? ctx->getRenderTargetDesc(RenderTargetHandle{key.index, key.generation}).name : "";
                break;
            case ResourceKind::SWAPCHAIN:
                return "swapchain";
            case ResourceKind::SWAPCHAIN_DEPTH:
                return "swapchain depth";
            }
            return kind + (name.empty() ? " #" + std::to_string(key.index) + ":" + std::to_string(key.generation) : " '" + name + "'");
        }

        std::string joined(const std::vector<std::string> &items)
        {
            std::string text;
            for (const auto &item : items)
                text += (text.empty() ? "" : ", ") + item;
            return text.empty() ? "-" : text;
        }

        uint32_t rankOf(char mark)
        {
            switch (mark)
            {
            case 'W':
                return 3;
            case 'R':
                return 2;
            case 'P':
                return 1;
            default:
                return 0;
            }
        }

        // W beats R, and R beats P (a read of the frame before), so a mark shows the strongest use at that position.
        void markUse(std::vector<LifetimeRow> &rows, std::unordered_map<ResourceKey, uint32_t, ResourceKeyHash> &rowOf, const ResourceKey &key,
                     const std::string &label, uint32_t position, uint32_t positions, char mark)
        {
            auto [it, added] = rowOf.try_emplace(key, static_cast<uint32_t>(rows.size()));
            if (added)
                rows.push_back({.label = label, .marks = std::string(positions, ' ')});
            char &current = rows[it->second].marks[position];
            if (rankOf(mark) > rankOf(current))
                current = mark;
        }

        // A depth texture is sampleable when it is D32_SFLOAT and occlusion culling is on, or its target is depth-only
        // (TextureUsage::SAMPLED_DEPTH_ATTACHMENT, see depthTextureDesc in renderContext.cpp). The swapchain's depth is
        // D32_SFLOAT with one color.
        bool canReadDepth(const Engine *engine, const Pass &pass, uint32_t colorCount, Format depthFormat)
        {
            if (!engine)
                return true;
            if ((engine->getConfig().occlusionCulling || colorCount == 0) && depthFormat == Format::D32_SFLOAT)
                return true;
            spdlog::error("Pass '{}' reads a depth texture that can't be sampled: it needs D32_SFLOAT depth, and EngineConfig::occlusionCulling unless its target is depth-only; the read is ignored.", pass.name);
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
        textureAccess = std::move(other.textureAccess);
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

    void RenderGraph::addAsyncComputePass(std::string name, const std::function<void(PassBuilder &)> &setup, std::function<void(RenderContext &)> execute)
    {
        addComputePass(std::move(name), setup, std::move(execute));
        passes.back().async = true;
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
            if (pass.compute && pass.outputBuffers.empty() && pass.frameOutputBuffers.empty())
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

        std::fill(std::begin(transientUses), std::end(transientUses), 0);
        const uint32_t passCount = placed ? static_cast<uint32_t>(executionOrder.size()) : 0;
        for (uint32_t position = 0; position < passCount; ++position)
        {
            auto &pass = passes[executionOrder[position]];

            // An async pass records into the compute queue's batch, every other pass into the graphics one. Without
            // a compute queue both are the graphics batch, and nothing waits across queues. A batch that can't be
            // opened drops the frame, and endFrame submits nothing.
            if (!engine->useQueue(pass.async ? QueueType::COMPUTE : QueueType::GRAPHICS))
                break;
            const QueueType queue = engine->recordingQueue;
            const auto commandBuffer = engine->getCurrentFrameResources().commandBuffer;

            // Barriers are not allowed inside a render pass, so all of them go before the pass begins. beginPass
            // moves the attachments it draws to, and endPass leaves a render target's color SAMPLED for its readers.
            // A depth read moves the depth here, and the next pass that draws the target moves it back.
            // A transient target's first pass draws over memory that other targets used before. On its own queue the
            // aliasing barrier covers them; on the other queue it waits for this frame's passes there that used a
            // transient target. The frame before is covered by its end, where graphics waits for all compute work: a
            // first pass draws, so it is a graphics pass.
            if (std::find(transientFirstPasses.begin(), transientFirstPasses.end(), position) != transientFirstPasses.end())
            {
                const QueueType other = otherQueue(queue);
                if (transientUses[static_cast<uint32_t>(other)] > 0)
                    engine->waitForQueue(queue, other, transientUses[static_cast<uint32_t>(other)]);
                ctx.aliasingBarrier(commandBuffer);
            }
            if (usesTransient(pass, transientTargets))
                transientUses[static_cast<uint32_t>(queue)] = engine->openBatchValue(queue);
            if (engine->asyncCompute)
                textureWaits(pass, queue);
            PipelineStage before = {};
            PipelineStage after = {};
            if (bufferBarrier(pass, queue, before, after))
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

    // The passes in the order they run, then each resource's lifetime: one column per pass in that order, W where a
    // pass writes it, R where one reads it, P where one reads what the frame before left, - between its first and
    // last use, . outside them. A per-frame buffer is one row, named by its first slot. The swapchain's depth has its
    // own row: a pass that draws to the swapchain writes both. A graph that did not compile lists its passes in the
    // order they were added.
    std::string RenderGraph::dump() const
    {
        std::vector<uint32_t> order = executionOrder;
        if (!compiled)
        {
            order.clear();
            for (uint32_t i = 0; i < passes.size(); ++i)
                order.push_back(i);
        }
        std::vector<uint32_t> positionOf(passes.size(), NO_PASS);
        for (uint32_t position = 0; position < order.size(); ++position)
            positionOf[order[position]] = position;

        const auto positions = static_cast<uint32_t>(order.size());
        std::string text = "Render graph: " + std::to_string(passes.size()) + " passes, " +
                           (compiled ? "in the order they run" : "not compiled, in the order they were added") +
                           (engine ? (engine->asyncCompute ? ", async compute on" : ", async compute off") : "") + "\n";
        std::vector<LifetimeRow> rows;
        std::unordered_map<ResourceKey, uint32_t, ResourceKeyHash> rowOf;
        const ResourceKey swapchainDepth = {.kind = ResourceKind::SWAPCHAIN_DEPTH};
        for (uint32_t position = 0; position < positions; ++position)
        {
            const Pass &pass = passes[order[position]];
            std::vector<std::string> reads;
            std::vector<std::string> writes;
            auto use = [&](const ResourceKey &key, char mark, const char *note)
            {
                const std::string label = labelOf(engine, key);
                (mark == 'W' ? writes : reads).push_back(label + note);
                markUse(rows, rowOf, key, label, position, positions, mark);
            };
            for (const PassAccess &access : accessesOf(pass))
            {
                if (access.slot > 0)
                    continue;
                const bool swapchain = access.key.kind == ResourceKind::SWAPCHAIN;
                if (swapchain && access.depth)
                {
                    use(swapchainDepth, access.mark, "");
                    continue;
                }
                const char *note = access.mark == 'P' ? " (frame before)" : (access.perFrame ? " (per frame)" : (access.depth ? " (depth)" : ""));
                use(access.key, access.mark, note);
                if (swapchain && access.mark == 'W')
                    use(swapchainDepth, 'W', "");
            }

            // Without async compute an async pass runs on the graphics queue, like any compute pass.
            const bool onComputeQueue = pass.async && engine && engine->asyncCompute;
            const char *queue = onComputeQueue ? "async compute" : (pass.compute ? "compute" : "graphics");
            text += "\n[" + std::to_string(position) + "] " + pass.name + " (" + queue + ")\n";
            text += "    reads:  " + joined(reads) + "\n";
            text += "    writes: " + joined(writes) + "\n";
            if (!pass.compute)
            {
                const AttachmentOps &ops = pass.attachmentOps;
                text += std::string("    attachments: ") + (ops.loadColor ? "load color" : "clear color") + ", " + (ops.loadDepth ? "load depth" : "clear depth") + ", " +
                        (ops.storeDepth ? "store depth" : "discard depth") + "\n";
            }
            if (compiled)
            {
                // A per-frame buffer adds an edge per slot, so a successor can be listed more than once.
                std::vector<uint32_t> after;
                for (uint32_t successor : pass.successors)
                    after.push_back(positionOf[successor]);
                std::sort(after.begin(), after.end());
                after.erase(std::unique(after.begin(), after.end()), after.end());
                std::vector<std::string> next;
                for (uint32_t later : after)
                    next.push_back("[" + std::to_string(later) + "] " + passes[order[later]].name);
                text += "    before: " + joined(next) + "\n";
            }
        }

        for (auto target : transientTargets)
        {
            auto it = rowOf.find(keyOf(target));
            if (it != rowOf.end())
                rows[it->second].label += " (transient)";
        }
        text += "\nLifetimes (W write, R read, P read of the frame before, - alive between uses, . not used):\n    ";
        for (uint32_t position = 0; position < positions; ++position)
            text += static_cast<char>('0' + position % 10);
        text += "\n";
        for (auto &row : rows)
        {
            const size_t first = row.marks.find_first_not_of(' ');
            const size_t last = row.marks.find_last_not_of(' ');
            for (size_t i = 0; i < row.marks.size(); ++i)
            {
                if (row.marks[i] == ' ')
                    row.marks[i] = (i > first && i < last) ? '-' : '.';
            }
            text += "    " + row.marks + "  " + row.label + "\n";
        }
        return text;
    }

    // A failed placement, here or at a resize, leaves targets without textures. It is tried again every frame, and
    // until it works the frames draw no passes. Every target has a depth, depth-only ones included, so that is what
    // is looked at.
    bool RenderGraph::placeTransients()
    {
        if (transientTargets.empty())
            return true;
        auto &ctx = engine->getRenderContext();
        const bool missing = std::any_of(transientTargets.begin(), transientTargets.end(), [&ctx](const RenderTargetHandle &target)
                                         { return !ctx.getRenderTarget(target).attachments[0].depth.isValid(); });
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
    // waits for a write at the end of the frame before. Barriers cover earlier submissions on the same queue.
    // A use on the other queue is waited for at the start of the batch instead, and the semaphore makes its writes
    // visible to every stage, so it needs no barrier.
    bool RenderGraph::bufferBarrier(const Pass &pass, QueueType queue, PipelineStage &before, PipelineStage &after)
    {
        const PipelineStage stage = pass.compute ? PipelineStage::COMPUTE : PipelineStage::GRAPHICS;
        const auto q = static_cast<uint32_t>(queue);
        const uint64_t value = engine ? engine->openBatchValue(queue) : 0;
        const uint32_t slot = engine ? static_cast<uint32_t>(engine->frameCount) : 0;
        before = {};
        after = {};

        // A write is made visible to each stage once, so the next read in the same stage needs no barrier. A read of
        // what the frame before wrote waits for that write like any other read.
        auto read = [&](BufferHandle buffer)
        {
            auto &access = bufferAccess[buffer];
            waitForOtherQueue(access, queue, false);
            auto &own = access.queues[q];
            if (own.writeStages != PipelineStage{} && !hasStage(own.readStages, stage))
            {
                before = before | own.writeStages;
                after = after | stage;
            }
            own.readStages = own.readStages | stage;
            own.value = value;
        };
        // A write waits for every use since the last write, reads included.
        auto write = [&](BufferHandle buffer)
        {
            auto &access = bufferAccess[buffer];
            waitForOtherQueue(access, queue, true);
            const PipelineStage used = access.queues[q].writeStages | access.queues[q].readStages;
            if (used != PipelineStage{})
            {
                before = before | used;
                after = after | stage;
            }
            access = {};
            access.queues[q] = {.writeStages = stage, .value = value};
        };

        for (auto buffer : pass.inputBuffers)
            read(buffer);
        for (const auto &buffers : pass.frameInputBuffers)
            read(buffers.slots[slot]);
        for (auto buffer : pass.previousBuffers)
            read(buffer);
        for (auto buffer : pass.outputBuffers)
            write(buffer);
        for (const auto &buffers : pass.frameOutputBuffers)
            write(buffers.slots[slot]);
        return after != PipelineStage{};
    }

    // A read waits for the other queue's last write, and a write for every use there since.
    void RenderGraph::waitForOtherQueue(const Access &access, QueueType queue, bool writes)
    {
        const QueueType other = otherQueue(queue);
        const QueueUse &use = access.queues[static_cast<uint32_t>(other)];
        if (engine && (use.writeStages != PipelineStage{} || (writes && use.readStages != PipelineStage{})))
            engine->waitForQueue(queue, other, use.value);
    }

    // Textures order across queues only: within a queue their layout transitions do. Every use counts as a write,
    // since the first use on a queue is often a layout transition, which writes the image.
    void RenderGraph::textureWaits(const Pass &pass, QueueType queue)
    {
        const PipelineStage stage = pass.compute ? PipelineStage::COMPUTE : PipelineStage::GRAPHICS;
        const uint64_t value = engine->openBatchValue(queue);
        auto use = [&](TextureHandle texture)
        {
            if (!texture.isValid())
                return;
            auto &access = textureAccess[texture];
            waitForOtherQueue(access, queue, true);
            access = {};
            access.queues[static_cast<uint32_t>(queue)] = {.writeStages = stage, .value = value};
        };
        // A render target's colors and depth, from this frame's slot, or only the colors from the previous one.
        const uint32_t slot = static_cast<uint32_t>(engine->frameCount);
        const uint32_t previousSlot = (slot + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
        auto useRenderTarget = [&](RenderTargetHandle renderTarget, bool previous)
        {
            const RenderTarget &data = engine->getRenderContext().getRenderTarget(renderTarget);
            if (previous && !data.history)
                return;
            const RenderTargetAttachments &attachments = data.attachments[previous ? previousSlot : slot];
            for (uint32_t i = 0; i < data.colorCount; ++i)
                use(attachments.color[i]);
            if (!previous)
                use(attachments.depth);
        };

        for (auto texture : pass.inputTextures)
            use(texture);
        for (auto texture : pass.outputTextures)
            use(texture);
        for (auto renderTarget : pass.inputRenderTargets)
            useRenderTarget(renderTarget, false);
        for (auto renderTarget : pass.previousRenderTargets)
            useRenderTarget(renderTarget, true);
        for (auto renderTarget : pass.depthRenderTargets)
            use(engine->getRenderTargetDepthTexture(renderTarget));
        if (pass.outputRenderTarget.isValid())
            useRenderTarget(pass.outputRenderTarget, false);
        if (pass.readsSwapchainDepth || pass.writesSwapchain)
            use(engine->getCurrentFrameResources().depthTexture);
        if (pass.writesSwapchain)
            use(engine->swapchain.imageHandles[engine->imageIdx]);
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

    void RenderGraph::read(uint32_t passIdx, const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT])
    {
        FrameBuffers buffers;
        std::copy(std::begin(perFrame), std::end(perFrame), std::begin(buffers.slots));
        passes[passIdx].frameInputBuffers.push_back(buffers);
    }

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
        const RenderTarget *target = engine ? &engine->getRenderContext().getRenderTarget(h) : nullptr;
        if (canReadDepth(engine, passes[passIdx], target ? target->colorCount : 1, target ? target->depthFormat : Format::D32_SFLOAT))
            passes[passIdx].depthRenderTargets.push_back(h);
    }

    void RenderGraph::readSwapchainDepth(uint32_t passIdx)
    {
        if (canReadDepth(engine, passes[passIdx], 1, Format::D32_SFLOAT))
            passes[passIdx].readsSwapchainDepth = true;
    }

    void RenderGraph::write(uint32_t passIdx, TextureHandle h) { passes[passIdx].outputTextures.push_back(h); }

    void RenderGraph::write(uint32_t passIdx, BufferHandle h) { passes[passIdx].outputBuffers.push_back(h); }

    void RenderGraph::write(uint32_t passIdx, RenderTargetHandle h) { passes[passIdx].outputRenderTarget = h; }

    void RenderGraph::write(uint32_t passIdx, const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT])
    {
        FrameBuffers buffers;
        std::copy(std::begin(perFrame), std::end(perFrame), std::begin(buffers.slots));
        passes[passIdx].frameOutputBuffers.push_back(buffers);
    }

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

    void PassBuilder::read(const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]) { graph.read(currentPass, perFrame); }

    void PassBuilder::readPrevious(RenderTargetHandle handle) { graph.readPrevious(currentPass, handle); }

    void PassBuilder::readPrevious(BufferHandle handle) { graph.readPrevious(currentPass, handle); }

    void PassBuilder::readDepth(RenderTargetHandle handle) { graph.readDepth(currentPass, handle); }

    void PassBuilder::readSwapchainDepth() { graph.readSwapchainDepth(currentPass); }

    void PassBuilder::write(TextureHandle handle) { graph.write(currentPass, handle); }

    void PassBuilder::write(BufferHandle handle) { graph.write(currentPass, handle); }

    void PassBuilder::write(RenderTargetHandle handle) { graph.write(currentPass, handle); }

    void PassBuilder::write(const BufferHandle (&perFrame)[MAX_FRAMES_IN_FLIGHT]) { graph.write(currentPass, perFrame); }

    void PassBuilder::writeSwapchain() { graph.writeSwapchain(currentPass); }

}