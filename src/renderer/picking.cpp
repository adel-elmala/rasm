#include "renderer/picking.h"
#include "rasm/renderer.h"
#include "rasm/renderSpace.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/blendSort.h"
#include "renderer/geometryPool.h"
#include "renderer/glyphCull.h"
#include "renderer/gpuScene.h"
#include "renderer/pipelineWarmup.h"
#include "renderer/viewGraph.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace rasm
{
    namespace
    {
        // Byte offsets in a slot's culling buffer: cull.slang's visible draws, its slot per draw and count per group,
        // the draw counts per run, the cull stats it adds to, which nothing reads, then the blend sort's keys and the
        // draws it copies aside (sort.slang).
        struct PickCullLayout
        {
            uint64_t cullSlots = 0;
            uint64_t groupCounts = 0;
            uint64_t drawCounts = 0;
            uint64_t stats = 0;
            uint64_t sortKeys = 0;
            uint64_t sortDraws = 0;
            uint32_t sortCapacity = 0;
            uint64_t size = 0;
        };
    }

    namespace
    {
        constexpr uint32_t CULL_GROUP_SIZE = 64; // numthreads of cull.slang
        constexpr uint32_t PICK_TEXEL_WORDS = 4; // an R32G32B32A32_UINT texel
        constexpr Format PICK_FORMAT = Format::R32G32B32A32_UINT;

        PickCullLayout getPickCullLayout(uint32_t capacity)
        {
            const uint64_t groups = capacity / CULL_GROUP_SIZE + gfx::MAX_DRAW_RUNS;
            PickCullLayout layout;
            layout.cullSlots = static_cast<uint64_t>(capacity) * sizeof(gfx::DrawInfo);
            layout.groupCounts = layout.cullSlots + static_cast<uint64_t>(capacity) * sizeof(uint32_t);
            layout.drawCounts = layout.groupCounts + groups * sizeof(uint32_t);
            layout.stats = layout.drawCounts + gfx::MAX_DRAW_RUNS * sizeof(uint32_t);
            layout.sortKeys = layout.stats + CULL_STATS_WORDS * sizeof(uint32_t);
            layout.sortCapacity = getBlendSortCapacity(capacity);
            layout.sortDraws = layout.sortKeys + static_cast<uint64_t>(layout.sortCapacity) * 2 * sizeof(uint32_t);
            layout.size = layout.sortDraws + static_cast<uint64_t>(layout.sortCapacity) * sizeof(gfx::DrawInfo);
            return layout;
        }

        bool isNearer(const PickGroup &a, const PickGroup &b)
        {
            if (a.centreDistance != b.centreDistance)
                return a.centreDistance < b.centreDistance;
            if (a.cameraDistance != b.cameraDistance)
                return a.cameraDistance < b.cameraDistance;
            return a.nearest.row != b.nearest.row ? a.nearest.row < b.nearest.row : a.nearest.feature < b.nearest.feature;
        }

        PickGroup toGroup(const PickSample &sample, const ViewPixels &rect)
        {
            const float dx = static_cast<float>(sample.x) + 0.5f - (static_cast<float>(rect.x) + 0.5f * static_cast<float>(rect.width));
            const float dy = static_cast<float>(sample.y) + 0.5f - (static_cast<float>(rect.y) + 0.5f * static_cast<float>(rect.height));
            return {.nearest = sample, .pixels = 1, .centreDistance = dx * dx + dy * dy, .cameraDistance = glm::length(sample.position)};
        }

        void releaseTexture(RenderContext &ctx, TextureHandle &texture)
        {
            if (texture.isValid())
                ctx.destroyTexture(texture);
            texture = {};
        }
    }

    void InstanceOwners::reset(uint32_t rows)
    {
        current.assign(rows, {});
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            slots[i].assign(rows, {});
            pending[i].clear();
        }
    }

    void InstanceOwners::assign(uint32_t first, uint32_t count, EntityHandle entity, MeshHandle mesh)
    {
        if (first + count > current.size())
            return;
        for (uint32_t i = 0; i < count; ++i)
            current[first + i] = {.entityIndex = static_cast<uint32_t>(entity.index), .entityGeneration = static_cast<uint32_t>(entity.generation), .subMesh = i, .mesh = mesh};
        for (auto &ranges : pending)
            ranges.push_back({.first = first, .count = count});
    }

    void InstanceOwners::sync(uint32_t slot)
    {
        for (const RowRange &range : pending[slot])
            std::copy(current.begin() + range.first, current.begin() + range.first + range.count, slots[slot].begin() + range.first);
        pending[slot].clear();
    }

    InstanceOwner InstanceOwners::get(uint32_t slot, uint32_t row) const
    {
        return slot < MAX_FRAMES_IN_FLIGHT && row < slots[slot].size() ? slots[slot][row] : InstanceOwner{};
    }

    // The rect's NDC range, x0..x1 and y0..y1 (y up: pixel row 0 is the top, NDC +1), stretched to -1..1 in clip space,
    // where it is a scale and a translation times w.
    glm::dmat4 getPickClipMatrix(const ViewPixels &rect, uint32_t width, uint32_t height)
    {
        const double x0 = 2.0 * rect.x / width - 1.0;
        const double x1 = 2.0 * (rect.x + rect.width) / width - 1.0;
        const double y0 = 1.0 - 2.0 * (rect.y + rect.height) / height;
        const double y1 = 1.0 - 2.0 * rect.y / height;
        glm::dmat4 clip(1.0);
        clip[0][0] = 2.0 / (x1 - x0);
        clip[1][1] = 2.0 / (y1 - y0);
        clip[3][0] = -(x1 + x0) / (x1 - x0);
        clip[3][1] = -(y1 + y0) / (y1 - y0);
        return clip;
    }

    uint32_t getPickArgument(const PickDesc &desc)
    {
        const float alpha = std::isfinite(desc.minAlpha) ? std::clamp(desc.minAlpha, 0.0f, 1.0f) : 0.5f;
        // Rounded down, so a surface whose alpha is minAlpha is picked.
        return (desc.skip & PICK_SKIP_ALL) | static_cast<uint32_t>(std::floor(alpha * 255.0f)) << 8;
    }

    // The octahedral code of rasm.slang's packPickNormal, back to a unit vector.
    glm::vec3 unpackPickNormal(uint32_t packed)
    {
        const glm::vec2 e = glm::vec2(static_cast<float>(packed & 0xFFFFu), static_cast<float>(packed >> 16)) / 65535.0f * 2.0f - 1.0f;
        glm::vec3 n(e.x, e.y, 1.0f - std::abs(e.x) - std::abs(e.y));
        if (n.z < 0.0f)
        {
            n.x = (1.0f - std::abs(e.y)) * (e.x >= 0.0f ? 1.0f : -1.0f);
            n.y = (1.0f - std::abs(e.x)) * (e.y >= 0.0f ? 1.0f : -1.0f);
        }
        return glm::normalize(n);
    }

    void groupPickSamples(std::vector<PickSample> &samples, const ViewPixels &rect, std::vector<PickGroup> &groups)
    {
        groups.clear();
        std::sort(samples.begin(), samples.end(), [](const PickSample &a, const PickSample &b)
                  { return a.row != b.row ? a.row < b.row : a.feature < b.feature; });
        for (const PickSample &sample : samples)
        {
            const PickGroup candidate = toGroup(sample, rect);
            if (groups.empty() || groups.back().nearest.row != sample.row || groups.back().nearest.feature != sample.feature)
            {
                groups.push_back(candidate);
                continue;
            }
            PickGroup &group = groups.back();
            const uint32_t pixels = group.pixels + 1;
            if (isNearer(candidate, group))
                group = candidate;
            group.pixels = pixels;
        }
        std::sort(groups.begin(), groups.end(), isNearer);
    }

    PickHandle Renderer::pick(const View &view, const PickDesc &desc)
    {
        if (!isLive(view.scene) || !isLive(view.camera))
        {
            spdlog::error("pick: the view's scene or camera is invalid or was destroyed.");
            return {};
        }
        return queuePick(&view, view.scene, view.camera, {}, desc);
    }

    PickHandle Renderer::pick(SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget, const PickDesc &desc)
    {
        if (!isLive(scene) || !isLive(camera) || !ctx.isAlive(renderTarget))
        {
            spdlog::error("pick: the scene, camera or render target is invalid or was destroyed.");
            return {};
        }
        return queuePick(nullptr, scene, camera, renderTarget, desc);
    }

    PickHandle Renderer::queuePick(const View *view, SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget, const PickDesc &desc)
    {
        if (desc.rect.width == 0 || desc.rect.height == 0 || desc.rect.width > MAX_PICK_SIZE || desc.rect.height > MAX_PICK_SIZE)
        {
            spdlog::error("pick: the rect is {} x {} pixels; it must be 1 to MAX_PICK_SIZE ({}) on each side. A box over much of a view is another query (D48).", desc.rect.width,
                          desc.rect.height, MAX_PICK_SIZE);
            return {};
        }
        if ((desc.skip & ~PICK_SKIP_ALL) != 0)
        {
            spdlog::error("pick: skip {:#x} has bits that are no PICK_SKIP_* value.", desc.skip);
            return {};
        }
        const HandleSlot slot = picks->handles.allocate();
        if (slot.index == 0)
        {
            spdlog::error("pick: {} picks are waiting to be taken; take them (takePick) before asking for more.", picks->handles.liveCount());
            return {};
        }
        // A request taken before keeps its hit vector's capacity, so a pick every frame allocates nothing.
        if (picks->spare.empty())
            picks->spare.emplace_back();
        PickRequest request = std::move(picks->spare.back());
        picks->spare.pop_back();
        request.handle = {slot.index, slot.generation};
        request.byView = view != nullptr;
        request.view = view != nullptr ? *view : View{};
        request.scene = scene;
        request.camera = camera;
        request.target = renderTarget;
        request.desc = desc;
        request.state = PickState::WAITING;
        request.hits.clear();
        picks->requests.push_back(std::move(request));
        return picks->requests.back().handle;
    }

    bool Renderer::isPickReady(PickHandle pick)
    {
        for (PickRequest &request : picks->requests)
        {
            if (request.handle != pick)
                continue;
            if (request.state == PickState::SUBMITTED && ctx.getFenceValue(frameFence) >= request.fenceValue)
                readPicks(request.slot);
            return request.state == PickState::DONE || request.state == PickState::FAILED;
        }
        return false;
    }

    bool Renderer::takePick(PickHandle pick, PickHit *hits, uint32_t capacity, uint32_t &hitCount)
    {
        hitCount = 0;
        auto it = std::find_if(picks->requests.begin(), picks->requests.end(), [pick](const PickRequest &request)
                               { return request.handle == pick; });
        if (it == picks->requests.end())
        {
            spdlog::error("takePick: unknown pick, or one already taken.");
            return false;
        }
        if (capacity > 0 && hits == nullptr)
        {
            spdlog::error("takePick: hits is null and capacity is {}.", capacity);
            return false;
        }
        if (it->state == PickState::WAITING || it->state == PickState::RECORDED)
        {
            // Waiting here would never end: no submitted frame has drawn it yet.
            spdlog::error("takePick: no frame has drawn this pick yet; it is drawn at the end of the next frame (endFrame).");
            return false;
        }
        if (it->state == PickState::SUBMITTED)
        {
            const uint32_t slot = it->slot;
            if (ctx.waitForFence(frameFence, it->fenceValue))
                readPicks(slot);
            else
                it->state = PickState::FAILED;
            // readPicks may not move requests, but find it again all the same.
            it = std::find_if(picks->requests.begin(), picks->requests.end(), [pick](const PickRequest &request)
                              { return request.handle == pick; });
        }
        const bool done = it->state == PickState::DONE;
        hitCount = static_cast<uint32_t>(it->hits.size());
        if (done)
            std::copy_n(it->hits.begin(), std::min(capacity, hitCount), hits);
        picks->handles.release(pick.index, pick.generation);
        picks->spare.push_back(std::move(*it));
        picks->requests.erase(it);
        return done;
    }

    // In the frame's last graphics batch, after every pass: each pick waiting is a frustum cull of its view's draw list
    // through its rect (cull.slang, frustum only), then those draws into its column of the slot's targets with the
    // pick pipelines, the viewport moved so that the view's pixels land where they do in the view. Then one copy of
    // the targets' used corner. A pick whose view did not draw in this frame fails.
    void Renderer::recordPicks()
    {
        const bool any = std::any_of(picks->requests.begin(), picks->requests.end(), [](const PickRequest &request)
                                     { return request.state == PickState::WAITING; });
        if (!any)
            return;
        ZoneScoped;

        const auto frame = static_cast<uint32_t>(frameCount);
        PickSlot &slot = picks->slots[frame];
        // The frame's picks, side by side: their views, their rects inside them, and the room they need.
        uint32_t chosen[MAX_FRAME_PICKS] = {};
        uint32_t views[MAX_FRAME_PICKS] = {};
        uint32_t chosenCount = 0;
        uint32_t rowWidth = 0;
        uint32_t rowHeight = 0;
        uint32_t drawCapacity = 0;
        uint32_t glyphChunks = 0;
        uint32_t glyphBatches = 0;
        for (uint32_t i = 0; i < picks->requests.size() && chosenCount < MAX_FRAME_PICKS; ++i)
        {
            PickRequest &request = picks->requests[i];
            if (request.state != PickState::WAITING)
                continue;
            const RenderTargetHandle target = !request.byView ? request.target : (viewGraph ? viewGraph->getSceneTarget(request.view) : RenderTargetHandle{});
            uint32_t viewIndex = UINT32_MAX;
            for (uint32_t v = 0; target.isValid() && v < MAX_SCENE_VIEWS && viewIndex == UINT32_MAX; ++v)
            {
                const SceneView &view = gpuScene->views[v];
                if (view.graphs > 0 && !view.depthOnly && view.scene == request.scene && view.camera == request.camera && view.renderTarget == target &&
                    view.preparedSync == gpuScene->syncCount)
                    viewIndex = v;
            }
            if (viewIndex == UINT32_MAX)
            {
                request.state = PickState::FAILED;
                spdlog::error("pick: its view did not draw in the frame that would draw the pick; takePick returns false for it.");
                continue;
            }
            const SceneView &view = gpuScene->views[viewIndex];
            const uint32_t width = ctx.getRenderTarget(view.renderTarget).width;
            const uint32_t height = ctx.getRenderTarget(view.renderTarget).height;
            const ViewPixels &asked = request.desc.rect;
            if (asked.x >= width || asked.y >= height)
            {
                request.state = PickState::FAILED;
                spdlog::error("pick: the rect at ({}, {}) is outside the view's {} x {} pixels; takePick returns false for it.", asked.x, asked.y, width, height);
                continue;
            }
            const ViewPixels rect = {.x = asked.x, .y = asked.y, .width = std::min(asked.width, width - asked.x), .height = std::min(asked.height, height - asked.y)};
            // A view with nothing to draw this frame shows nothing.
            if (!view.ready)
            {
                request.state = PickState::DONE;
                request.rect = rect;
                continue;
            }
            if (rowWidth + rect.width > MAX_PICK_ROW_WIDTH)
                break;
            request.rect = rect;
            request.atlasX = rowWidth;
            request.origin = view.origin;
            rowWidth += rect.width;
            rowHeight = std::max(rowHeight, rect.height);
            drawCapacity = std::max(drawCapacity, static_cast<uint32_t>(gpuScene->drawLists[view.scene.index].draws.size()));
            if (view.drawGlyphs)
            {
                glyphChunks = std::max(glyphChunks, view.glyphChunkCapacity[frame]);
                glyphBatches = std::max(glyphBatches, view.glyphBatchCapacity[frame]);
            }
            chosen[chosenCount] = i;
            views[chosenCount++] = viewIndex;
        }
        if (chosenCount == 0)
            return;

        // The pick pipelines: rasm.slang's vertex stage and pickMain, per alpha mode, made on the first pick (P31). The BLEND one
        // tests depth but does not write it: its draws, sorted, write the ids in the order the view blends them. The
        // weighted and additive runs are not sorted, so theirs (the same shaders) writes depth: the nearest surface whose
        // alpha passes minAlpha is picked. The point run's is points.slang's, made once a picked scene has one; without it
        // the pick sees no points.
        const char *alphaDefines[4] = {"", "#define RASM_ALPHA_MASK 1\n", "#define RASM_ALPHA_BLEND 1\n", "#define RASM_ALPHA_BLEND 1\n"};
        const char *names[4] = {"rasmPick", "rasmPickMask", "rasmPickBlend", "rasmPickBlend"};
        const Format formats[MAX_COLOR_ATTACHMENTS] = {PICK_FORMAT, PICK_FORMAT};
        std::string source;
        for (uint32_t i = 0; i < 4; ++i)
        {
            if (picks->pipelines[i].isValid())
                continue;
            if (source.empty())
                source = readFile(pathFromUtf8(config.shaderDir) / "passes/rasm.slang");
            picks->pipelines[i] = createGraphicsPipeline(names[i], getShaderVariant(std::string("#define RASM_PICK 1\n") + alphaDefines[i], source), formats, DEPTH_FORMAT, "vertMain", "pickMain",
                                                         BlendMode::NONE, i != 2);
        }
        const bool pipelines = picks->pipelines[0].isValid() && picks->pipelines[1].isValid() && picks->pipelines[2].isValid() && picks->pipelines[3].isValid();
        bool pointRuns = false;
        for (uint32_t c = 0; c < chosenCount; ++c)
        {
            const auto &runs = gpuScene->drawLists[gpuScene->views[views[c]].scene.index].runs;
            pointRuns = pointRuns || std::any_of(runs.begin(), runs.end(), [](const DrawRun &run)
                                                 { return run.points; });
        }
        if (pipelines && pointRuns && !picks->pointPipeline.isValid())
        {
            const std::string pointSource = readFile(pathFromUtf8(config.shaderDir) / "passes/points.slang");
            picks->pointPipeline = createGraphicsPipeline("pointsPick", getShaderVariant("#define RASM_PICK 1\n", pointSource), formats, DEPTH_FORMAT, "vertMain", "pickMain", BlendMode::NONE, true);
        }
        // A view that drew glyphs culls them again through the rect into the slot's glyph buffer, and draws them with the
        // glyph pick pipeline, made with the first pick of such a view; without it the pick sees no glyphs.
        if (pipelines && glyphChunks > 0 && !picks->glyphPipeline.isValid())
        {
            if (source.empty())
                source = readFile(pathFromUtf8(config.shaderDir) / "passes/rasm.slang");
            picks->glyphPipeline = createGraphicsPipeline("rasmPickGlyphs", getShaderVariant("#define RASM_PICK 1\n#define RASM_GLYPHS 1\n", source), formats, DEPTH_FORMAT, "vertMain", "pickMain",
                                                          BlendMode::NONE, true);
        }
        bool pickGlyphs = glyphChunks > 0 && picks->glyphPipeline.isValid();
        if (pickGlyphs && (slot.glyphChunkCapacity < glyphChunks || slot.glyphBatchCapacity < glyphBatches))
        {
            const uint32_t chunks = std::max(glyphChunks, slot.glyphChunkCapacity);
            const uint32_t batches = std::max(glyphBatches, slot.glyphBatchCapacity);
            const uint64_t size = getGlyphCullLayout(chunks, batches).size;
            if (slot.glyphCull.isValid())
                pickGlyphs = ctx.resizeBuffer(slot.glyphCull, size);
            else
                slot.glyphCull = ctx.createBuffer({.name = "Pick Glyph Buffer", .usage = BufferUsage::INDIRECT, .size = size});
            pickGlyphs = pickGlyphs && slot.glyphCull.isValid();
            slot.glyphChunkCapacity = pickGlyphs ? chunks : 0;
            slot.glyphBatchCapacity = pickGlyphs ? batches : 0;
        }
        const GlyphCullLayout glyphLayout = getGlyphCullLayout(slot.glyphChunkCapacity, slot.glyphBatchCapacity);

        // The slot's targets, culling buffer and host copy grow to what the frame needs, and stay.
        bool resources = pipelines;
        if (resources && (slot.width < rowWidth || slot.height < rowHeight))
        {
            const uint32_t width = std::min(std::max(rowWidth, slot.width * 2), MAX_PICK_ROW_WIDTH);
            const uint32_t height = std::min(std::max(rowHeight, slot.height * 2), MAX_PICK_SIZE);
            releaseTexture(ctx, slot.ids);
            releaseTexture(ctx, slot.positions);
            releaseTexture(ctx, slot.depth);
            slot.ids = ctx.createTexture({.name = "Pick Ids", .width = width, .height = height, .format = PICK_FORMAT, .usage = TextureUsage::COLOR_ATTACHMENT, .readable = true});
            slot.positions = ctx.createTexture({.name = "Pick Positions", .width = width, .height = height, .format = PICK_FORMAT, .usage = TextureUsage::COLOR_ATTACHMENT, .readable = true});
            slot.depth = ctx.createTexture({.name = "Pick Depth", .width = width, .height = height, .format = DEPTH_FORMAT, .usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT});
            resources = slot.ids.isValid() && slot.positions.isValid() && slot.depth.isValid();
            slot.width = resources ? width : 0;
            slot.height = resources ? height : 0;
        }
        if (resources && slot.cullCapacity < drawCapacity)
        {
            const uint32_t capacity = std::max(drawCapacity, slot.cullCapacity * 2);
            const uint64_t size = getPickCullLayout(capacity).size;
            if (slot.cull.isValid())
                resources = ctx.resizeBuffer(slot.cull, size);
            else
                slot.cull = ctx.createBuffer({.name = "Pick Cull Buffer", .usage = BufferUsage::INDIRECT, .size = size});
            resources = resources && slot.cull.isValid();
            slot.cullCapacity = resources ? capacity : 0;
        }
        const uint64_t copySize = static_cast<uint64_t>(rowWidth) * rowHeight * PICK_TEXEL_WORDS * sizeof(uint32_t) * 2;
        if (resources && slot.readbackSize < copySize)
        {
            const uint64_t size = std::max(copySize, slot.readbackSize * 2);
            if (slot.readback.isValid())
                ctx.destroyBuffer(slot.readback);
            slot.readback = ctx.createBuffer({.name = "Pick Readback", .usage = BufferUsage::STORAGE, .size = size, .hostReadable = true});
            resources = slot.readback.isValid();
            slot.readbackSize = resources ? size : 0;
        }
        if (!resources)
        {
            for (uint32_t i = 0; i < chosenCount; ++i)
                picks->requests[chosen[i]].state = PickState::FAILED;
            spdlog::error("pick: the pick pipelines or targets could not be made; takePick returns false for this frame's picks.");
            return;
        }

        // A compute-queue pass (the skinning, culling) may have written what the picks read.
        if (asyncCompute)
            waitForQueueWork(QueueType::GRAPHICS, QueueType::COMPUTE);
        if (!useQueue(QueueType::GRAPHICS))
            return;
        const CommandBufferHandle commandBuffer = frameResources[frame].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "Pick");
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE | PipelineStage::GRAPHICS);
        const uint64_t cullAddress = ctx.getBufferDeviceAddress(slot.cull);
        const PickCullLayout layout = getPickCullLayout(slot.cullCapacity);
        for (uint32_t i = 0; i < chosenCount; ++i)
        {
            PickRequest &request = picks->requests[chosen[i]];
            const SceneView &view = gpuScene->views[views[i]];
            const SceneDrawList &drawList = gpuScene->drawLists[view.scene.index];
            const auto &target = ctx.getRenderTarget(view.renderTarget);

            // The view's SceneData, with its projection without jitter and its frustum through the rect, culled into
            // the slot's buffer by the frustum alone: the same LODs, vector sizes and labels as the view.
            gfx::SceneData sceneData = view.sceneData;
            sceneData.projection = view.projection;
            sceneData.jitter = glm::vec2(0.0f);
            sceneData.textureLodBias = 0.0f;
            const glm::dmat4 clip = getPickClipMatrix(request.rect, target.width, target.height);
            getFrustumPlanes(glm::mat4(clip * glm::dmat4(view.projection) * glm::dmat4(sceneData.view)), sceneData.frustumPlanes);
            if (view.culled)
            {
                sceneData.drawInfoPtr = cullAddress;
                sceneData.cullSlotsPtr = cullAddress + layout.cullSlots;
                sceneData.groupCountsPtr = cullAddress + layout.groupCounts;
                sceneData.drawCountsPtr = cullAddress + layout.drawCounts;
                sceneData.cullStatsPtr = cullAddress + layout.stats;
            }
            sceneData.hizLevelCount = 0;
            sceneData.visibilityPtr = 0;
            sceneData.hizPtr = 0;
            sceneData.meshletVisibilityPtr = 0;
            sceneData.meshletRuns = 0;
            sceneData.coneRuns = 0;
            sceneData.meshletDrawsPtr = 0;
            sceneData.groupMeshletsPtr = 0;
            // The blend run is sorted as the view sorted it (its draw order first, then back to front) and drawn last
            // without writing depth, so the see-through surface the view shows on top writes the pixel last.
            const bool sorted = view.culled && view.sortBlend;
            const uint32_t sortCapacity = sorted ? getBlendSortCapacity(drawList.runs[view.blendRun].count) : 0;
            sceneData.sortKeysPtr = sorted ? cullAddress + layout.sortKeys : 0;
            sceneData.sortDrawsPtr = sorted ? cullAddress + layout.sortDraws : 0;
            sceneData.sortCapacity = sortCapacity;
            sceneData.blendRun = sorted ? view.blendRun : gfx::MAX_DRAW_RUNS;
            // One culling phase keeps every run, and the view's weight span is not the pick's to measure.
            sceneData.weightedRun = gfx::MAX_DRAW_RUNS;
            sceneData.additiveRun = gfx::MAX_DRAW_RUNS;
            sceneData.weightedRangePtr = 0;
            sceneData.debugView = 0;
            // A pick draws every point by triangles, which take the same pixels as the compute path.
            sceneData.pointRasterPtr = 0;
            sceneData.pointBatchesPtr = 0;
            sceneData.pointPixelsPtr = 0;
            sceneData.pointBatchCapacity = 0;
            // Its glyphs are culled into the slot's glyph buffer, never into the view's.
            const bool glyphs = pickGlyphs && view.culled && view.drawGlyphs;
            if (glyphs)
                writeGlyphCullPointers(sceneData, ctx.getBufferDeviceAddress(slot.glyphCull), glyphLayout, slot.glyphChunkCapacity, slot.glyphBatchCapacity);
            else
                writeGlyphCullPointers(sceneData, 0, {}, 0, 0);
            const uint32_t sceneDataIndex = MAX_SCENE_VIEWS + i;
            ctx.fillBuffer(gpuScene->sceneData[frame], &sceneData, sizeof(sceneData), static_cast<size_t>(sceneDataIndex) * sizeof(gfx::SceneData));
            const uint64_t sceneAddress = getSceneDataAddress(ctx, *gpuScene, frame, sceneDataIndex);

            if (view.culled)
            {
                // The pick before read this buffer as its draws.
                if (i > 0)
                    ctx.memoryBarrier(commandBuffer, PipelineStage::GRAPHICS, PipelineStage::COMPUTE);
                const gfx::PushConstants cullConstants{.scene = sceneAddress};
                if (glyphs)
                    recordGlyphReset(sceneAddress);
                ctx.bindPipeline(commandBuffer, gpuScene->cullCountPipeline);
                ctx.pushConstants(commandBuffer, gpuScene->cullCountPipeline, &cullConstants, sizeof(cullConstants), 0);
                ctx.dispatch(commandBuffer, view.groupCount);
                ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
                ctx.bindPipeline(commandBuffer, gpuScene->cullWritePipeline);
                ctx.pushConstants(commandBuffer, gpuScene->cullWritePipeline, &cullConstants, sizeof(cullConstants), 0);
                ctx.dispatch(commandBuffer, view.groupCount);
                if (glyphs)
                    recordGlyphCull(sceneAddress, slot.glyphCull, 0);
                if (sorted)
                {
                    ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
                    recordBlendSort(sceneAddress, sortCapacity);
                }
                ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::GRAPHICS);
            }

            // The first pick clears the targets; the others keep what the ones before drew in their columns.
            const TextureHandle colors[2] = {slot.ids, slot.positions};
            ctx.transitionImageLayout(commandBuffer, slot.ids, TextureUsage::COLOR_ATTACHMENT, i == 0);
            ctx.transitionImageLayout(commandBuffer, slot.positions, TextureUsage::COLOR_ATTACHMENT, i == 0);
            ctx.transitionImageLayout(commandBuffer, slot.depth, TextureUsage::DEPTH_STENCIL_ATTACHMENT, i == 0);
            ctx.beginRendering(commandBuffer, colors, 2, slot.depth, glm::vec4(0.0f), {.loadColor = i > 0, .loadDepth = i > 0, .storeDepth = true});
            // The view's pixel (x, y) of the rect lands on the column's (0, 0): the rasterizer sees the view's pixels.
            ctx.setViewport(commandBuffer, static_cast<float>(request.atlasX) - static_cast<float>(request.rect.x), -static_cast<float>(request.rect.y), static_cast<float>(target.width),
                            static_cast<float>(target.height));
            ctx.setScissor(commandBuffer, static_cast<int32_t>(request.atlasX), 0, request.rect.width, request.rect.height);
            ctx.bindIndexBuffer(commandBuffer, geometryPool->indexBuffer(), 0, Format::U32_UINT);
            const uint32_t argument = getPickArgument(request.desc);
            PipelineHandle bound = {};
            for (uint32_t r = 0; r < drawList.runs.size(); ++r)
            {
                const DrawRun &run = drawList.runs[r];
                if (run.points && !picks->pointPipeline.isValid())
                    continue;
                uint32_t kind = run.alphaMode == AlphaMode::NONE ? 0 : (run.alphaMode == AlphaMode::MASK ? 1 : 2);
                if (run.alphaMode == AlphaMode::BLEND && run.blendMethod != BlendMethod::SORTED)
                    kind = 3;
                const PipelineHandle pipeline = run.points ? picks->pointPipeline : picks->pipelines[kind];
                if (pipeline != bound)
                {
                    ctx.bindPipeline(commandBuffer, pipeline);
                    ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
                    bound = pipeline;
                }
                // Through a mirrored view the front faces turn clockwise, as drawSceneView swaps them.
                CullMode cullMode = run.cullMode;
                if (view.mirrored && cullMode != CullMode::NONE)
                    cullMode = cullMode == CullMode::BACK ? CullMode::FRONT : CullMode::BACK;
                ctx.setCullMode(commandBuffer, cullMode);
                const gfx::PushConstants pushConstants{.scene = sceneAddress, .firstDraw = run.first, .argument = argument};
                ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
                if (view.culled)
                    ctx.drawIndexedIndirectCount(commandBuffer, slot.cull, run.first * sizeof(gfx::DrawInfo), slot.cull, layout.drawCounts + r * sizeof(uint32_t), run.count, sizeof(gfx::DrawInfo));
                else
                    ctx.drawIndexedIndirect(commandBuffer, drawList.buffers[frame], run.first * sizeof(gfx::DrawInfo), run.count, sizeof(gfx::DrawInfo));
            }
            if (glyphs)
            {
                CullMode cullMode = drawList.glyphCullMode;
                if (view.mirrored && cullMode != CullMode::NONE)
                    cullMode = CullMode::FRONT;
                // The weighted chunks' commands too: a pick sees every glyph, with the BLEND ones' minAlpha.
                drawGlyphs(sceneAddress, slot.glyphCull, 0, glyphLayout.commands, glyphLayout.commandCapacity, false, picks->glyphPipeline, cullMode, argument);
                drawGlyphs(sceneAddress, slot.glyphCull, 0, glyphLayout.weightedCommands, glyphLayout.commandCapacity, true, picks->glyphPipeline, cullMode, argument);
            }
            ctx.setCullMode(commandBuffer, CullMode::NONE);
            ctx.endRendering(commandBuffer);
            request.state = PickState::RECORDED;
            request.slot = frame;
        }

        // The used corner of both targets, one after the other, rows packed.
        const uint64_t idsSize = copySize / 2;
        const bool copied = ctx.copyTextureToBuffer(commandBuffer, slot.ids, rowWidth, rowHeight, slot.readback, 0) &&
                            ctx.copyTextureToBuffer(commandBuffer, slot.positions, rowWidth, rowHeight, slot.readback, idsSize);
        endGpuTimer(commandBuffer, timer);
        slot.usedWidth = rowWidth;
        slot.usedHeight = rowHeight;
        if (!copied)
        {
            for (uint32_t i = 0; i < chosenCount; ++i)
                picks->requests[chosen[i]].state = PickState::FAILED;
            spdlog::error("pick: the copy of the pick targets failed; takePick returns false for this frame's picks.");
        }
    }

    // After submitBatches: the picks drawn this frame are done at the frame's fence value. A frame that did not go out
    // took them with it, so the next one draws them again.
    void Renderer::finishPickSubmit(bool submitted)
    {
        for (PickRequest &request : picks->requests)
        {
            if (request.state != PickState::RECORDED)
                continue;
            request.state = submitted ? PickState::SUBMITTED : PickState::WAITING;
            request.fenceValue = frameFenceValue;
        }
    }

    // Reads the slot's copy once its frame is done: before the slot's next frame records new picks into it, and before
    // its next sync gives its instance rows to other entities, so each row names the entity the frame drew.
    void Renderer::readPicks(uint32_t slotIndex)
    {
        const PickSlot &slot = picks->slots[slotIndex];
        bool any = false;
        for (const PickRequest &request : picks->requests)
            any = any || (request.state == PickState::SUBMITTED && request.slot == slotIndex && ctx.getFenceValue(frameFence) >= request.fenceValue);
        if (!any)
            return;
        ZoneScoped;

        const size_t texels = static_cast<size_t>(slot.usedWidth) * slot.usedHeight;
        std::vector<uint32_t> &words = picks->texels;
        words.resize(texels * PICK_TEXEL_WORDS * 2);
        const bool read = ctx.readBuffer(slot.readback, words.data(), words.size() * sizeof(uint32_t), 0);
        const uint32_t *ids = words.data();
        const uint32_t *positions = words.data() + texels * PICK_TEXEL_WORDS;
        for (PickRequest &request : picks->requests)
        {
            if (request.state != PickState::SUBMITTED || request.slot != slotIndex || ctx.getFenceValue(frameFence) < request.fenceValue)
                continue;
            if (!read)
            {
                request.state = PickState::FAILED;
                continue;
            }
            std::vector<PickSample> &samples = picks->samples;
            samples.clear();
            for (uint32_t y = 0; y < request.rect.height; ++y)
            {
                for (uint32_t x = 0; x < request.rect.width; ++x)
                {
                    const size_t texel = (static_cast<size_t>(y) * slot.usedWidth + request.atlasX + x) * PICK_TEXEL_WORDS;
                    if (ids[texel] == 0)
                        continue;
                    samples.push_back({.row = ids[texel] - 1,
                                       .feature = ids[texel + 1],
                                       .flags = ids[texel + 2],
                                       .normal = positions[texel + 3],
                                       .position = glm::vec3(std::bit_cast<float>(positions[texel]), std::bit_cast<float>(positions[texel + 1]), std::bit_cast<float>(positions[texel + 2])),
                                       .x = request.rect.x + x,
                                       .y = request.rect.y + y});
                }
            }
            // Ranked from the centre the caller asked for, also when the rect was cut at the view's edge.
            groupPickSamples(samples, request.desc.rect, picks->groups);
            request.hits.resize(picks->groups.size());
            for (size_t i = 0; i < picks->groups.size(); ++i)
            {
                const PickGroup &group = picks->groups[i];
                const InstanceOwner owner = picks->owners.get(slotIndex, group.nearest.row);
                PickHit &hit = request.hits[i];
                hit.entity = {owner.entityIndex, owner.entityGeneration};
                hit.subMesh = owner.subMesh;
                hit.feature = (group.nearest.flags & PICK_HIT_FEATURE) != 0 ? group.nearest.feature : 0;
                // A point's index in its chunk's drawn order goes back to the application's (Mesh::pointOrder), through the
                // mesh the rows were given for. Once that mesh is destroyed the index cannot be mapped: no feature then.
                if ((group.nearest.flags & PICK_HIT_POINT) != 0)
                {
                    const bool mapped = isLive(owner.mesh) && hit.feature < registry.meshes[owner.mesh.index].pointOrder.size();
                    hit.feature = mapped ? registry.meshes[owner.mesh.index].pointOrder[hit.feature] : 0;
                    hit.flags = mapped ? hit.flags : hit.flags & ~PICK_HIT_FEATURE;
                }
                hit.flags = group.nearest.flags;
                hit.pixels = group.pixels;
                hit.x = group.nearest.x;
                hit.y = group.nearest.y;
                hit.position = toWorldPosition(group.nearest.position, request.origin);
                hit.normal = unpackPickNormal(group.nearest.normal);
                hit.distance = glm::length(glm::dvec3(group.nearest.position));
            }
            request.state = PickState::DONE;
        }
    }
}
