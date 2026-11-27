#include "renderer/export.h"
#include "rasm/renderer.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/gpuScene.h"
#include "renderer/imageWriter.h"
#include "renderer/taa.h"
#include "renderer/uberMaterialTypes.h"
#include "renderer/viewGraph.h"
#include "renderer/weightedBlend.h"

#include "glm/gtc/matrix_transform.hpp"
#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <string>

namespace rasm
{
    namespace
    {
        // numthreads of shaders/passes/export.slang.
        constexpr uint32_t EXPORT_GROUP_SIZE = 8;
        constexpr uint32_t NO_TILE = UINT32_MAX;
    }

    namespace
    {
        uint32_t roundUp(uint32_t value, uint32_t multiple)
        {
            return (value + multiple - 1) / multiple * multiple;
        }

        double getHalton(uint32_t index, uint32_t base)
        {
            double result = 0.0;
            double fraction = 1.0;
            while (index > 0)
            {
                fraction /= static_cast<double>(base);
                result += fraction * static_cast<double>(index % base);
                index /= base;
            }
            return result;
        }

        // The Export pass's encoding: a file's format, or in memory RGBA8 for PNG8 and float for the others.
        uint32_t getExportOutput(const ExportDesc &desc)
        {
            if (desc.format == ExportFormat::PNG8)
                return gfx::EXPORT_OUTPUT_SRGB8;
            if (desc.path.empty())
                return gfx::EXPORT_OUTPUT_FLOAT;
            if (desc.format == ExportFormat::PNG16)
                return gfx::EXPORT_OUTPUT_SRGB16;
            return desc.format == ExportFormat::EXR_HALF ? gfx::EXPORT_OUTPUT_HALF : gfx::EXPORT_OUTPUT_FLOAT;
        }

        uint32_t getOutputBytes(uint32_t output)
        {
            if (output == gfx::EXPORT_OUTPUT_SRGB8)
                return 4;
            return output == gfx::EXPORT_OUTPUT_FLOAT ? 16 : 8;
        }

        bool hasBloom(const View &view, const RendererConfig &config)
        {
            return view.bloom == ViewToggle::DEFAULT ? config.bloom : view.bloom == ViewToggle::ON;
        }

        // The view's MSAA samples, or the most the device has below them; 1 without MSAA or without desc.msaa.
        uint32_t getExportSamples(const RenderContext &ctx, const View &view, const RendererConfig &config, bool msaa)
        {
            const AntiAliasing mode = view.antiAliasing == AntiAliasing::DEFAULT ? config.antiAliasing : view.antiAliasing;
            uint32_t samples = 1;
            if (msaa && mode == AntiAliasing::MSAA_2X)
                samples = 2;
            if (msaa && mode == AntiAliasing::MSAA_4X)
                samples = 4;
            if (msaa && mode == AntiAliasing::MSAA_8X)
                samples = 8;
            while (samples > 1 && !ctx.supportsSampleCount(samples))
                samples /= 2;
            return samples;
        }

        ExportJob *findJob(Exports &exports, ExportHandle handle)
        {
            for (const std::unique_ptr<ExportJob> &job : exports.jobs)
            {
                if (job->handle == handle && !job->cancelled)
                    return job.get();
            }
            return nullptr;
        }

        ExportJob *getRunningJob(Exports &exports)
        {
            for (const std::unique_ptr<ExportJob> &job : exports.jobs)
            {
                if (job->state == ExportState::RUNNING && !job->cancelled)
                    return job.get();
            }
            return nullptr;
        }

        const ExportJob *getRunningJob(const Exports &exports)
        {
            for (const std::unique_ptr<ExportJob> &job : exports.jobs)
            {
                if (job->state == ExportState::RUNNING && !job->cancelled)
                    return job.get();
            }
            return nullptr;
        }

        uint32_t getTileCount(const ExportLayout &layout)
        {
            return layout.columns * layout.rows;
        }
    }

    bool getExportLayout(uint32_t width, uint32_t height, uint32_t tileSize, uint32_t guard, uint32_t maxImageSize, ExportLayout &layout)
    {
        layout = {};
        if (width == 0 || height == 0 || tileSize == 0)
            return false;
        layout.width = width;
        layout.height = height;
        layout.virtualWidth = roundUp(width, EXPORT_ALIGNMENT);
        layout.virtualHeight = roundUp(height, EXPORT_ALIGNMENT);
        layout.guard = roundUp(guard, EXPORT_ALIGNMENT);
        uint32_t size = std::max(tileSize / EXPORT_ALIGNMENT, 1u) * EXPORT_ALIGNMENT;
        // A tile and its guard on both sides fit the device.
        const uint32_t deviceLimit = maxImageSize / EXPORT_ALIGNMENT * EXPORT_ALIGNMENT;
        if (deviceLimit < 2 * layout.guard + EXPORT_ALIGNMENT)
            return false;
        size = std::min(size, deviceLimit - 2 * layout.guard);
        layout.tileSize = size;
        layout.columns = (layout.virtualWidth + size - 1) / size;
        layout.rows = (layout.virtualHeight + size - 1) / size;
        // One tile across draws the virtual image's whole width; several draw a tile and its guard on both sides.
        layout.renderWidth = layout.columns == 1 ? layout.virtualWidth : std::min(size + 2 * layout.guard, layout.virtualWidth);
        layout.renderHeight = layout.rows == 1 ? layout.virtualHeight : std::min(size + 2 * layout.guard, layout.virtualHeight);
        return layout.renderWidth <= maxImageSize && layout.renderHeight <= maxImageSize;
    }

    // A render rect is placed with its guard before the tile, and moved back inside the virtual image at its edges, where
    // the image's own edge is what the bloom clamps to, as in a single render.
    ExportTile getExportTile(const ExportLayout &layout, uint32_t tile)
    {
        ExportTile result;
        const uint32_t column = tile % std::max(layout.columns, 1u);
        const uint32_t row = tile / std::max(layout.columns, 1u);
        result.x = column * layout.tileSize;
        result.y = row * layout.tileSize;
        result.width = std::min(layout.tileSize, layout.width - std::min(result.x, layout.width));
        result.height = std::min(layout.tileSize, layout.height - std::min(result.y, layout.height));
        result.renderX = std::min(result.x - std::min(result.x, layout.guard), layout.virtualWidth - layout.renderWidth);
        result.renderY = std::min(result.y - std::min(result.y, layout.guard), layout.virtualHeight - layout.renderHeight);
        return result;
    }

    // NDC x of the whole image runs over its width W (-1 at the left edge), and y over its height H (+1 at the top
    // row: clip space is Y-up). The render rect of R pixels from pixel r0 maps them to -1 .. 1: scale W / R, shift
    // (W - 2 r0) / R - 1, and the same in y with the sign of its shift turned.
    glm::dmat4 getExportTileClip(const ExportLayout &layout, const ExportTile &tile)
    {
        const double width = layout.width;
        const double height = layout.height;
        const double renderWidth = layout.renderWidth;
        const double renderHeight = layout.renderHeight;
        glm::dmat4 clip(1.0);
        clip[0][0] = width / renderWidth;
        clip[1][1] = height / renderHeight;
        clip[3][0] = (width - 2.0 * tile.renderX) / renderWidth - 1.0;
        clip[3][1] = 1.0 - (height - 2.0 * tile.renderY) / renderHeight;
        return clip;
    }

    glm::vec2 getExportJitter(uint32_t sample, uint32_t samples)
    {
        if (samples <= 1)
            return glm::vec2(0.0f);
        const auto side = static_cast<uint32_t>(std::lround(std::sqrt(static_cast<double>(samples))));
        if (side * side == samples)
        {
            const uint32_t x = sample % side;
            const uint32_t y = sample / side;
            return glm::vec2((static_cast<float>(x) + 0.5f) / static_cast<float>(side) - 0.5f, (static_cast<float>(y) + 0.5f) / static_cast<float>(side) - 0.5f);
        }
        return glm::vec2(static_cast<float>(getHalton(sample + 1, 2) - 0.5), static_cast<float>(getHalton(sample + 1, 3) - 0.5));
    }

    uint32_t getExportPixelBytes(ExportFormat format)
    {
        if (format == ExportFormat::PNG8)
            return 4;
        return format == ExportFormat::EXR_FLOAT ? 16 : 8;
    }

    const ExportViewFrame *getExportViewFrame(const Exports &exports, SceneHandle scene, CameraHandle camera, const RenderTargetHandle &target)
    {
        const ExportViewFrame &view = exports.view;
        return view.active && view.scene == scene && view.camera == camera && view.target == target ? &view : nullptr;
    }

    glm::vec4 getExportLabelProbe(const ExportViewFrame &frame, glm::vec4 probe)
    {
        float test = probe.w;
        if (test >= LABEL_PROBE_RESET)
            test -= LABEL_PROBE_RESET;
        if (test == LABEL_PROBE_NONE)
            return glm::vec4(0.0f, 0.0f, 0.0f, LABEL_PROBE_NONE);
        if (!frame.survey)
            return test == LABEL_PROBE_DEPTH ? glm::vec4(0.0f, 0.0f, probe.z, LABEL_PROBE_DEPTH) : glm::vec4(0.0f, 0.0f, 0.0f, LABEL_PROBE_NONE);
        // The pixel the anchor falls in, kept inside the image as a single render's 3 x 3 test keeps it.
        const float x = std::clamp(std::floor(probe.x), 0.0f, static_cast<float>(frame.imageWidth - 1));
        const float y = std::clamp(std::floor(probe.y), 0.0f, static_cast<float>(frame.imageHeight - 1));
        const ExportTile &tile = frame.tile;
        if (x < static_cast<float>(tile.x) || x >= static_cast<float>(tile.x + tile.width) || y < static_cast<float>(tile.y) || y >= static_cast<float>(tile.y + tile.height))
            return glm::vec4(0.0f, 0.0f, 0.0f, LABEL_PROBE_NONE);
        return glm::vec4(probe.x - static_cast<float>(tile.renderX), probe.y - static_cast<float>(tile.renderY), probe.z, test + LABEL_PROBE_RESET);
    }

    void gatherExportWeightedWords(Exports &exports, uint32_t slot, float nearest, float farthest)
    {
        ExportJob *job = getRunningJob(exports);
        if (job == nullptr || slot >= MAX_FRAMES_IN_FLIGHT || !job->surveySlots[slot])
            return;
        if (job->nearest > job->farthest)
        {
            job->nearest = nearest;
            job->farthest = farthest;
            return;
        }
        job->nearest = std::min(job->nearest, nearest);
        job->farthest = std::max(job->farthest, farthest);
    }

    bool getExportStreamView(const Exports &exports, SceneHandle scene, CameraHandle &camera, uint32_t &width, uint32_t &height, float &pixelScale)
    {
        const ExportJob *job = getRunningJob(exports);
        if (job == nullptr || !(job->view.scene == scene) || !job->camera.isValid() || job->phase == ExportPhase::FLUSH)
            return false;
        camera = job->camera;
        width = job->layout.width;
        height = job->layout.height;
        pixelScale = job->desc.pixelScale;
        return true;
    }

    bool isExportFrozen(const Exports &exports, SceneHandle scene)
    {
        const ExportJob *job = getRunningJob(exports);
        return job != nullptr && job->view.scene == scene && (job->phase == ExportPhase::SURVEY || job->phase == ExportPhase::DRAIN || job->phase == ExportPhase::TILES);
    }

    void setExportStreamSettled(Exports &exports, SceneHandle scene, bool settled)
    {
        const ExportJob *job = getRunningJob(exports);
        if (job != nullptr && job->view.scene == scene && !settled)
            exports.streamBusy = true;
    }

    ExportJob::ExportJob() = default;
    ExportJob::~ExportJob() = default;

    // The export starts with the next frame of Renderer::render, after the exports before it. Its camera is copied
    // there, so the application's camera may move; what the view's scene draws must hold still.
    ExportHandle Renderer::exportView(const View &view, const ExportDesc &desc)
    {
        if (!isLive(view.scene) || !isLive(view.camera))
        {
            spdlog::error("exportView needs a valid scene and camera.");
            return {};
        }
        if (desc.width == 0 || desc.height == 0 || desc.samples == 0 || desc.samples > MAX_EXPORT_SAMPLES || !(desc.pixelScale > 0.0f) || !std::isfinite(desc.pixelScale))
        {
            spdlog::error("exportView: the size must not be 0, samples must be 1 to {}, and pixelScale above 0.", MAX_EXPORT_SAMPLES);
            return {};
        }
        if (!desc.toneMap && (desc.format == ExportFormat::PNG8 || desc.format == ExportFormat::PNG16))
        {
            spdlog::error("exportView: toneMap false writes the scene's linear HDR color, which only an EXR format (EXR_HALF, EXR_FLOAT) holds.");
            return {};
        }
        const uint32_t guard = hasBloom(view, config) ? EXPORT_BLOOM_GUARD : EXPORT_GUARD;
        ExportLayout layout;
        if (!getExportLayout(desc.width, desc.height, desc.tileSize > 0 ? desc.tileSize : DEFAULT_EXPORT_TILE_SIZE, guard, ctx.getMaxImageSize(), layout))
        {
            spdlog::error("exportView: the device's largest image ({} pixels) cannot hold a tile and its guard of {} pixels.", ctx.getMaxImageSize(), guard);
            return {};
        }
        auto job = std::make_unique<ExportJob>();
        if (desc.path.empty())
        {
            const uint32_t bytes = getOutputBytes(getExportOutput(desc));
            try
            {
                job->image.pixels.resize(static_cast<size_t>(desc.width) * desc.height * bytes);
            }
            catch (const std::bad_alloc &)
            {
                spdlog::error("exportView: an image of {} x {} does not fit in memory; give a path to write it to a file as it is drawn.", desc.width, desc.height);
                return {};
            }
            job->image.width = desc.width;
            job->image.height = desc.height;
            job->image.format = desc.format == ExportFormat::PNG8 ? ReadbackFormat::RGBA8_SRGB : ReadbackFormat::RGBA32_FLOAT;
        }
        const HandleSlot slot = exports->handles.allocate();
        if (slot.index == 0)
        {
            spdlog::error("exportView: too many exports are kept; take or cancel the finished ones.");
            return {};
        }
        job->handle = {slot.index, slot.generation};
        job->view = view;
        job->desc = desc;
        job->layout = layout;
        const uint64_t tiles = getTileCount(layout);
        job->framesTotal = tiles * desc.samples + tiles + MAX_FRAMES_IN_FLIGHT;
        const ExportHandle handle = job->handle;
        exports->jobs.push_back(std::move(job));
        return handle;
    }

    ExportProgress Renderer::getExportProgress(ExportHandle exportHandle) const
    {
        for (const std::unique_ptr<ExportJob> &job : exports->jobs)
        {
            if (!(job->handle == exportHandle) || job->cancelled)
                continue;
            ExportProgress progress{.state = job->state, .tilesDone = job->tilesDone, .tileCount = getTileCount(job->layout)};
            const double done = static_cast<double>(job->framesDone) / static_cast<double>(std::max<uint64_t>(job->framesTotal, 1));
            progress.fraction = job->state == ExportState::DONE ? 1.0f : static_cast<float>(std::min(done, 0.99));
            return progress;
        }
        return {};
    }

    // Its GPU resources and its camera go with the next frame of Renderer::render, once the graph no longer draws it.
    bool Renderer::cancelExport(ExportHandle exportHandle)
    {
        ExportJob *job = findJob(*exports, exportHandle);
        if (job == nullptr)
            return false;
        job->cancelled = true;
        job->writer.reset();
        ++exports->graphKey;
        return true;
    }

    bool Renderer::takeExport(ExportHandle exportHandle, ReadbackImage &image)
    {
        ExportJob *job = findJob(*exports, exportHandle);
        if (job == nullptr || job->state != ExportState::DONE)
            return false;
        image = std::move(job->image);
        job->image = {};
        job->cancelled = true;
        ++exports->graphKey;
        return true;
    }

    void Renderer::failExport(ExportJob &job, const char *reason)
    {
        spdlog::error("Export {}: {}", job.handle.index, reason);
        job.state = ExportState::FAILED;
        job.writer.reset();
        ++exports->graphKey;
    }

    // The camera copy, the scene target of one render rect, the tile's sum and the slots' outputs and parameters, and
    // the file.
    bool Renderer::startExport(ExportJob &job)
    {
        const ExportDesc &desc = job.desc;
        const ExportLayout &layout = job.layout;
        job.camera = createCamera(registry.cameras[job.view.camera.index].projection, registry.cameras[job.view.camera.index].rig.transform);
        if (!job.camera.isValid())
            return false;
        registry.cameras[job.camera.index] = registry.cameras[job.view.camera.index];
        registry.cameras[job.camera.index].rig.controller.type = CameraController::Type::NONE;
        job.target = createRenderTarget({
            .name = "export_hdr",
            .width = layout.renderWidth,
            .height = layout.renderHeight,
            .colorFormats = {SCENE_COLOR_FORMAT},
            .depthFormat = Format::D32_SFLOAT,
            .clearColor = desc.transparent ? glm::vec4(0.0f) : BACKGROUND_COLOR,
            .samples = getExportSamples(ctx, job.view, config, desc.msaa),
        });
        const uint64_t pixels = static_cast<uint64_t>(layout.tileSize) * layout.tileSize;
        job.accum = ctx.createBuffer({.name = "Export Sum", .usage = BufferUsage::STORAGE, .size = pixels * 16});
        bool created = job.target.isValid() && job.accum.isValid();
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT && created; ++i)
        {
            job.outputs[i] = ctx.createBuffer({.name = "Export Output " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = pixels * getOutputBytes(getExportOutput(desc)), .hostVisible = true, .hostReadable = true});
            job.params[i] = ctx.createBuffer({.name = "Export Params " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = sizeof(gfx::ExportParams), .hostVisible = true});
            created = job.outputs[i].isValid() && job.params[i].isValid();
        }
        if (!created)
            return false;
        if (!desc.path.empty())
        {
            job.writer = std::make_unique<ImageWriter>();
            if (!job.writer->start(desc.path, desc.format, desc.width, desc.height))
                return false;
        }
        return true;
    }

    void Renderer::destroyExportResources(ExportJob &job)
    {
        if (job.camera.isValid() && isLive(job.camera))
            destroyCamera(job.camera);
        if (job.target.isValid())
            destroyRenderTarget(job.target);
        if (job.accum.isValid())
            destroyBuffer(job.accum);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            if (job.outputs[i].isValid())
                destroyBuffer(job.outputs[i]);
            if (job.params[i].isValid())
                destroyBuffer(job.params[i]);
            job.outputs[i] = {};
            job.params[i] = {};
        }
        job.camera = {};
        job.target = {};
        job.accum = {};
    }

    // What the running export draws this frame, before the graph is built and the frame begins (prepareSceneView reads
    // it): a phase moves on here when its condition holds, and on a submitted frame in finishExportSubmit.
    void Renderer::updateExports()
    {
        ZoneScoped;
        Exports &state = *exports;
        const bool streamBusy = state.streamBusy;
        state.streamBusy = false;
        state.view = {};
        ExportJob *job = getRunningJob(state);
        if (job == nullptr)
            return;
        if (!isLive(job->view.scene))
        {
            failExport(*job, "its scene was destroyed.");
            return;
        }
        if (!job->target.isValid())
        {
            ++state.graphKey;
            if (!isLive(job->view.camera))
            {
                failExport(*job, "its camera was destroyed before it started.");
                return;
            }
            if (!startExport(*job))
            {
                failExport(*job, "its targets, buffers or file could not be made.");
                return;
            }
        }
        if (job->writer && job->writer->finished() && job->writer->failed())
        {
            failExport(*job, "the file could not be written.");
            return;
        }

        if (job->phase == ExportPhase::SETTLE)
        {
            // The view's first frames make its pipelines; then the loads and the streamers must be quiet.
            ++job->phaseFrames;
            const LoadStats loadStats = getLoadStats();
            const bool quiet = job->phaseFrames > MAX_FRAMES_IN_FLIGHT && !streamBusy && loadStats.queued == 0 && loadStats.running == 0;
            if (quiet || job->phaseFrames >= EXPORT_SETTLE_FRAMES)
            {
                if (!quiet)
                    spdlog::warn("Export {}: the scene still loads or streams after {} frames; its tiles start anyway.", job->handle.index, job->phaseFrames);
                const SceneDrawList &drawList = gpuScene->drawLists[job->view.scene.index];
                const bool weighted = drawList.weightedGlyphChunks > 0 || std::any_of(drawList.runs.begin(), drawList.runs.end(), [](const DrawRun &run)
                                                                                      { return run.alphaMode == AlphaMode::BLEND && run.blendMethod == BlendMethod::WEIGHTED; });
                job->survey = weighted || !drawList.labelEntities.empty();
                const uint64_t tiles = getTileCount(job->layout);
                job->framesTotal = tiles * job->desc.samples + (job->survey ? tiles + MAX_FRAMES_IN_FLIGHT : 0);
                job->phase = job->survey ? ExportPhase::SURVEY : ExportPhase::TILES;
                job->phaseFrames = 0;
                job->tile = 0;
                job->sample = 0;
            }
        }
        ExportViewFrame &view = state.view;
        view.active = true;
        view.scene = job->view.scene;
        view.camera = job->camera;
        view.target = job->target;
        if (job->phase == ExportPhase::FLUSH)
        {
            view.idle = true;
            if (job->tilesDone == getTileCount(job->layout))
            {
                if (!job->writer)
                    job->state = ExportState::DONE;
                else
                {
                    job->writer->finish();
                    if (job->writer->finished())
                        job->state = job->writer->failed() ? ExportState::FAILED : ExportState::DONE;
                }
                if (job->state != ExportState::RUNNING)
                    ++state.graphKey;
            }
            return;
        }

        view.imageWidth = job->layout.width;
        view.imageHeight = job->layout.height;
        view.pixelScale = job->desc.pixelScale;
        view.transparent = job->desc.transparent;
        view.survey = job->phase == ExportPhase::SURVEY;
        view.frozen = job->phase == ExportPhase::TILES;
        view.weightedDepths = job->weightedDepths;
        // A row of tiles waits while the writer has enough bands to write.
        const bool waiting = job->phase == ExportPhase::TILES && job->writer && job->tile % job->layout.columns == 0 && job->sample == 0 && job->writer->queued() >= EXPORT_MAX_QUEUED_BANDS;
        view.idle = waiting;
        const uint32_t tile = job->phase == ExportPhase::TILES || job->phase == ExportPhase::SURVEY ? job->tile : 0;
        view.tile = getExportTile(job->layout, tile);
        const bool jittered = job->phase == ExportPhase::TILES;
        const glm::vec2 jitter = jittered ? getExportJitter(job->sample, job->desc.samples) : glm::vec2(0.0f);
        // The jitter moves the sample in the render rect's pixels; y is up in clip space and down in the image.
        const glm::dvec2 offset(2.0 * jitter.x / job->layout.renderWidth, -2.0 * jitter.y / job->layout.renderHeight);
        const glm::dmat4 clip = glm::translate(glm::dmat4(1.0), glm::dvec3(offset, 0.0)) * getExportTileClip(job->layout, view.tile);
        view.tileClip = glm::mat4(clip);
        view.textureLodBias = jittered && job->desc.samples > 1 ? TAA_TEXTURE_LOD_BIAS : 0.0f;
    }

    // The running export's view: its scene passes into its render rect, the MSAA resolve and the bloom as the view has
    // them, and the Export pass.
    bool Renderer::addExportPasses(RenderGraph &graph, const RenderTargetHandle *sampled, uint32_t sampledCount)
    {
        ExportJob *job = getRunningJob(*exports);
        if (job == nullptr || !job->target.isValid() || !isLive(job->camera))
            return true;
        if (!addScenePasses(graph, job->view.scene, job->camera, job->target, nullptr, 0, sampled, sampledCount))
            return false;
        RenderTargetHandle resolved = job->target;
        if (ctx.getRenderTarget(job->target).samples > 1)
            resolved = addMsaaResolvePass(graph, job->target, job->camera);
        if (!resolved.isValid())
            return false;
        const RenderTargetHandle bloom = hasBloom(job->view, config) ? addBloomPasses(graph, resolved, job->camera) : RenderTargetHandle{};
        const ExportHandle handle = job->handle;
        graph.addComputePass(
            "Export",
            [job, resolved, bloom](PassBuilder &builder)
            {
                builder.read(resolved);
                if (bloom.isValid())
                    builder.read(bloom);
                builder.write(job->accum);
                builder.write(job->outputs);
            },
            [this, handle, resolved, bloom](RenderContext &)
            {
                ExportJob *running = getRunningJob(*exports);
                if (running != nullptr && running->handle == handle)
                    recordExport(*running, resolved, bloom);
            });
        return true;
    }

    // A tile's sample: added to its sum, or on its last sample the sum written out for readExports. Survey and drain
    // frames only mark the frame drawn.
    void Renderer::recordExport(ExportJob &job, const RenderTargetHandle &source, const RenderTargetHandle &bloom)
    {
        const ExportViewFrame &view = exports->view;
        if (!view.active || view.idle)
            return;
        if (job.phase != ExportPhase::TILES)
        {
            job.recorded = true;
            return;
        }
        if (!exports->pipeline.isValid())
        {
            exports->pipeline = createComputePipeline("export", readFile(pathFromUtf8(config.shaderDir) / "passes/export.slang"), "exportMain");
            if (!exports->pipeline.isValid())
            {
                spdlog::error("Failed to create the Export pipeline; the export waits for it.");
                return;
            }
        }
        const auto frame = static_cast<uint32_t>(frameCount);
        const ExportTile &tile = view.tile;
        const ExportDesc &desc = job.desc;
        const Camera &camera = registry.cameras[job.camera.index];
        const bool last = job.sample + 1 == desc.samples;
        uint32_t flags = (job.sample == 0 ? gfx::EXPORT_FLAG_FIRST : 0u) | (last ? gfx::EXPORT_FLAG_LAST : 0u);
        flags |= (desc.toneMap ? gfx::EXPORT_FLAG_TONE_MAP : 0u) | (desc.transparent ? gfx::EXPORT_FLAG_TRANSPARENT : 0u);
        const gfx::ExportParams params{
            .accumPtr = ctx.getBufferDeviceAddress(job.accum),
            .outputPtr = ctx.getBufferDeviceAddress(job.outputs[frame]),
            .source = getBindlessTextureIndex(getRenderTargetTexture(source)),
            .bloom = bloom.isValid() && bloomRuns(job.camera) ? getBindlessTextureIndex(getRenderTargetTexture(bloom)) : INVALID_TEXTURE_INDEX,
            .exposure = camera.exposure,
            .bloomStrength = camera.bloomStrength,
            .offsetX = tile.x - tile.renderX,
            .offsetY = tile.y - tile.renderY,
            .width = tile.width,
            .height = tile.height,
            .weight = 1.0f / static_cast<float>(desc.samples),
            .flags = flags,
            .output = getExportOutput(desc),
        };
        if (!ctx.fillBuffer(job.params[frame], &params, sizeof(params), 0))
            return;
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const gfx::ExportPushConstants pushConstants{.params = ctx.getBufferDeviceAddress(job.params[frame])};
        // The sum the frames before wrote.
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, exports->pipeline);
        ctx.bindDescriptorSet(commandBuffer, exports->pipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, exports->pipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, (tile.width + EXPORT_GROUP_SIZE - 1) / EXPORT_GROUP_SIZE, (tile.height + EXPORT_GROUP_SIZE - 1) / EXPORT_GROUP_SIZE);
        if (last)
            ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::HOST);
        job.recorded = true;
    }

    // A submitted frame moves the export on; a dropped one is drawn again, since its sample never ran.
    void Renderer::finishExportSubmit(bool submitted)
    {
        ExportJob *job = getRunningJob(*exports);
        if (job == nullptr)
            return;
        const bool recorded = job->recorded;
        job->recorded = false;
        if (!submitted || !recorded)
            return;
        const auto slot = static_cast<uint32_t>(frameCount);
        job->surveySlots[slot] = job->phase == ExportPhase::SURVEY;
        ++job->framesDone;
        const uint32_t tiles = getTileCount(job->layout);
        if (job->phase == ExportPhase::SURVEY && ++job->tile == tiles)
        {
            job->phase = ExportPhase::DRAIN;
            job->phaseFrames = 0;
        }
        else if (job->phase == ExportPhase::DRAIN && ++job->phaseFrames >= MAX_FRAMES_IN_FLIGHT)
        {
            // Both slots' words of the survey's last frames are in: its span, snapped as a view's is, is every tile's.
            WeightedRange range;
            if (job->nearest <= job->farthest)
                stepWeightedRange(range, job->nearest, job->farthest, registry.cameras[job->camera.index].projection.type == CameraProjection::Type::ORTHOGRAPHIC, 0.0f);
            job->weightedDepths = getWeightedDepths(range);
            job->phase = ExportPhase::TILES;
            job->tile = 0;
            job->sample = 0;
        }
        else if (job->phase == ExportPhase::TILES)
        {
            if (++job->sample == job->desc.samples)
            {
                job->outputTiles[slot] = job->tile;
                job->sample = 0;
                if (++job->tile == tiles)
                    job->phase = ExportPhase::FLUSH;
            }
        }
    }

    // After the slot's fence wait: the tile its last frame finished goes into the band (or the image), and a full band
    // to the writer.
    void Renderer::readExports(uint32_t slot)
    {
        ExportJob *job = getRunningJob(*exports);
        if (job == nullptr || job->outputTiles[slot] == NO_TILE)
            return;
        ZoneScoped;
        const ExportTile tile = getExportTile(job->layout, job->outputTiles[slot]);
        job->outputTiles[slot] = NO_TILE;
        const uint32_t bytes = getOutputBytes(getExportOutput(job->desc));
        const size_t rowBytes = static_cast<size_t>(tile.width) * bytes;
        const size_t imageRow = static_cast<size_t>(job->layout.width) * bytes;
        uint8_t *target = nullptr;
        if (!job->writer)
            target = job->image.pixels.data() + static_cast<size_t>(tile.y) * imageRow + static_cast<size_t>(tile.x) * bytes;
        else
        {
            if (job->bandTiles == 0)
                job->band.assign(imageRow * tile.height, 0);
            target = job->band.data() + static_cast<size_t>(tile.x) * bytes;
        }
        for (uint32_t row = 0; row < tile.height; ++row)
        {
            if (!ctx.readBuffer(job->outputs[slot], target + row * imageRow, rowBytes, row * rowBytes))
            {
                failExport(*job, "a tile could not be read back.");
                return;
            }
        }
        ++job->tilesDone;
        if (job->writer && ++job->bandTiles == job->layout.columns)
        {
            job->writer->push(std::move(job->band), tile.height);
            job->band = {};
            job->bandTiles = 0;
        }
    }

    // Once the graph no longer draws them, finished and cancelled exports give back their camera, targets and buffers;
    // cancelled and taken ones go.
    void Renderer::collectExports()
    {
        for (const std::unique_ptr<ExportJob> &job : exports->jobs)
        {
            if (job->state != ExportState::RUNNING || job->cancelled)
                destroyExportResources(*job);
        }
        std::erase_if(exports->jobs, [this](const std::unique_ptr<ExportJob> &job)
                      {
                          if (!job->cancelled)
                              return false;
                          exports->handles.release(job->handle.index, job->handle.generation);
                          return true; });
    }
}
