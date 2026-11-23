#include "rasm/engine.h"
#include "assets/loadJobs.h"
#include "core/utils.h"
#include "core/workerPool.h"
#include "platform/paths.h"
#include "platform/platform.h"
#include "renderer/environmentBake.h"
#include "renderer/geometryPool.h"
#include "renderer/frameStats.h"
#include "renderer/gpuScene.h"
#include "renderer/inspector.h"
#include "renderer/lightClusters.h"
#include "renderer/readback.h"
#include "renderer/animation.h"
#include "renderer/renderer.h"
#include "renderer/shaderCompiler.h"
#include "renderer/statsOverlay.h"
#include "renderer/ui.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "glm/glm.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace rasm
{
    namespace
    {
        // Main-thread time a frame spends finishing loads. The load that crosses it still finishes.
        constexpr std::chrono::milliseconds LOAD_BUDGET(4);
        // The bytes of a texture filled between two checks of LOAD_BUDGET: about 0.6 ms of copying into the staging ring.
        constexpr uint64_t TEXTURE_FILL_PART = 4ull << 20;

        ShaderCompiler::Target getShaderCompilerTarget(rasm::Backend backend)
        {
            switch (backend)
            {
            case Backend::VULKAN:
                return ShaderCompiler::Target::SPIRV;
            case Backend::DX12:
                return ShaderCompiler::Target::HLSL;
            case Backend::METAL:
                return ShaderCompiler::Target::MSL;
            case Backend::WEBGPU:
                return ShaderCompiler::Target::WGSL;
            default:
                spdlog::error("Unsupported backend for shader compilation.");
                return ShaderCompiler::Target::SPIRV; // Default to SPIR-V
            }
        }
    }

    Engine::Engine(const EngineConfig &config) : config(config), geometryPool(std::make_unique<GeometryPool>()), gpuScene(std::make_unique<GpuScene>()), gpuLights(std::make_unique<GpuLights>()), gpuAnimation(std::make_unique<GpuAnimation>()), environments(std::make_unique<Environments>()), frameStats(std::make_unique<FrameStatsCollector>()), statsOverlay(std::make_unique<StatsOverlay>()), ctx(this), shaderCompiler(std::make_unique<ShaderCompiler>())
    {
        readbacks = std::make_unique<Readbacks>();
        // Before anything can fail, so a load on an engine that did not start still has workers. One hardware thread is
        // left for the main thread.
        const uint32_t hardwareThreads = std::thread::hardware_concurrency();
        const uint32_t loadThreads = config.loadThreads > 0 ? config.loadThreads : std::max(2u, hardwareThreads) - 1;
        workers = std::make_unique<WorkerPool>(loadThreads, config.loadMemoryBudget, "RASM loader");
        registry.lightHandles = HandlePool(static_cast<uint64_t>(config.maxLights) + 1);
        registry.materialHandles = HandlePool(static_cast<uint64_t>(config.maxMaterials) + 1);
        // In a real implementation, this is where we'd initialize the window, graphics context, etc.
        spdlog::info("initializing engine with config: appName={}, windowWidth={}, windowHeight={}, enableValidation={}",
                     config.appName, config.windowWidth, config.windowHeight, config.enableValidation);

        this->config.shaderDir = resolveFromExecutableDir(config.shaderDir.empty() ? RASM_SHADER_DIR : config.shaderDir);
        if (!config.assetRoot.empty())
        {
            this->config.assetRoot = resolveFromExecutableDir(config.assetRoot);
        }

        if (config.windowWidth <= 0 || config.windowHeight <= 0)
        {
            spdlog::error("Window dimensions must be positive.");
            return;
        }
        // After the check, so an engine that stops here makes no folder.
        this->config.cacheDir = resolveCacheDir(config.cacheDir);
        if (!this->config.cacheDir.empty())
            spdlog::info("Shader and pipeline caches in {}", this->config.cacheDir);

        const bool headless = config.platform == PlatformType::HEADLESS;
        platform = headless ? createHeadlessPlatform() : createSdlPlatform();
        this->mainWindow = createWindow(config.windowWidth, config.windowHeight);
        if (!headless && !mainWindow.handle.isValid())
        {
            spdlog::error("Failed to initialize engine window.");
            return;
        }

        if (!ctx.initialize(config.preferredBackend))
        {
            spdlog::error("Failed to initialize render context.");
            return;
        }

        // Each frame submit signals the next value; a slot is free again once its last value is reached.
        frameFence = ctx.createFence("Frame Fence");
        if (!frameFence.isValid())
        {
            spdlog::error("Failed to create the frame fence.");
            return;
        }

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            frameResources[i].commandBuffer = ctx.createCommandBuffer("Frame Command Buffer " + std::to_string(i));
            if (!frameResources[i].commandBuffer.isValid())
            {
                spdlog::error("Failed to create command buffer for frame {}", i);
                return;
            }
            batchCommandBuffers[i][static_cast<uint32_t>(QueueType::GRAPHICS)].push_back(frameResources[i].commandBuffer);
        }

        // Each compute submit signals the next value, like the frame fence for graphics.
        if (config.asyncCompute && ctx.hasAsyncCompute())
        {
            computeFence = ctx.createFence("Compute Fence");
            asyncCompute = computeFence.isValid();
            if (!asyncCompute)
                spdlog::warn("Failed to create the compute fence; compute passes run on the graphics queue.");
        }

        auto shaderTarget = getShaderCompilerTarget(config.preferredBackend);
        if (!shaderCompiler->initialize(shaderTarget, this->config.shaderDir, this->config.cacheDir))
        {
            spdlog::error("Failed to initialize shader compiler.");
            return;
        }

        if (!geometryPool->create(ctx, this->config) || !createGpuScene() || !createGpuLights() || !createGpuAnimation())
            return;
        createFrameStats();

        // Create bindless texture descriptor set layout, pool, and set
        bindlessDescriptorSetLayout = ctx.createBindlessDescriptorSetLayout({.name = "BindlessDescriptorSetLayout",
                                                                             .stage = ShaderType::ALL,
                                                                             .count = MAX_BINDLESS_TEXTURES,
                                                                             .type = DescriptorType::COMBINED_IMAGE_SAMPLER,
                                                                             .storageImageCount = MAX_BINDLESS_STORAGE_IMAGES});
        if (!bindlessDescriptorSetLayout.isValid())
        {
            spdlog::error("Failed to create bindless descriptor set layout.");
            return;
        }

        bindlessDescriptorPool = ctx.createDescriptorPool({.name = "BindlessDescriptorPool",
                                                           .sizes = {{.type = DescriptorType::COMBINED_IMAGE_SAMPLER, .count = MAX_BINDLESS_TEXTURES},
                                                                     {.type = DescriptorType::STORAGE_IMAGE, .count = MAX_BINDLESS_STORAGE_IMAGES}},
                                                           .maxSets = 1});
        if (!bindlessDescriptorPool.isValid())
        {
            spdlog::error("Failed to create bindless descriptor pool.");
            return;
        }

        bindlessDescriptorSet = ctx.allocateDescriptorSet("BindlessDescriptorSet", bindlessDescriptorPool, bindlessDescriptorSetLayout);
        if (!bindlessDescriptorSet.isValid())
        {
            spdlog::error("Failed to allocate bindless descriptor set.");
            return;
        }

        if (!createSwapchainResources() || !createEnvironments())
            return;

        // Its pipeline is built with the bindless layout, so it comes after it.
        if (!createStatsOverlay())
            return;
        // So is the UI's. A UI that fails to start stays off, and the stats overlay takes its place.
#if defined(RASM_IMGUI)
        if (this->config.ui)
            createUi();
#else
        if (this->config.ui)
            spdlog::warn("EngineConfig::ui is set, but RASM was built without Dear ImGui (RASM_IMGUI=OFF); the UI stays off.");
#endif

        // The glTF default material: white base color, metallic 1, roughness 1.
        defaultMaterial = createMaterial(MaterialType::PBR);
        if (!defaultMaterial.isValid())
        {
            spdlog::error("Failed to create the default material.");
            return;
        }
        Material defaultValues = getMaterial(defaultMaterial);
        defaultValues.name = "default";
        defaultValues.params[static_cast<uint32_t>(PbrParam::METALLIC)] = 1.0f;
        updateMaterial(defaultMaterial, defaultValues);

        renderer = std::make_unique<Renderer>(*this);
        isRunning = true;
    }

    Engine::~Engine()
    {
        // Loads that still run stop at their next step, queued ones are dropped, and the workers end. Jobs never touch
        // the engine, so this could come later; first, so no job outlives what it reads.
        for (PendingMeshLoad &load : pendingMeshes)
            workers->releaseBytes(workers->dropLoad(load.job->ticket));
        for (auto &[handle, load] : pendingTextures)
        {
            if (load.job)
                workers->releaseBytes(workers->dropLoad(load.job->ticket));
        }
        for (PendingEnvironmentLoad &load : pendingEnvironments)
            workers->releaseBytes(workers->dropLoad(load.job->ticket));
        workers->shutdown();
        // Its graph frees what it made through the context.
        renderer.reset();
        shaderCompiler->shutdown();
#if defined(RASM_IMGUI)
        destroyUi();
#endif
        ctx.cleanup();

        destroyWindow(this->mainWindow);
        spdlog::info("Engine shutdown, cleaned up resources.");
    }

    Window Engine::createWindow(uint32_t width, uint32_t height)
    {
        return platform ? platform->createWindow(width, height) : Window{};
    }

    void Engine::destroyWindow(const Window &window)
    {
        if (platform)
            platform->destroyWindow(window);
    }

    void Engine::pollEvents(CameraHandle camera)
    {
        // The first frame has no previous one, and a long stall (a debugger, a window drag) is capped so the camera does not jump.
        const auto now = std::chrono::steady_clock::now();
        frameTime = lastFrameTime == std::chrono::steady_clock::time_point{} ? 0.0f : std::min(std::chrono::duration<float>(now - lastFrameTime).count(), 0.1f);
        deltaTime = config.fixedDeltaTime > 0.0f ? config.fixedDeltaTime : frameTime;
        lastFrameTime = now;

        PlatformEvents events = {};
        bool uiActive = false;
#if defined(RASM_IMGUI)
        uiActive = ui && ui->active;
#endif
        if (platform)
            platform->pollEvents(mainWindow, uiActive, events);

        if (events.quit)
            isRunning = false;
        if (events.statsToggles % 2 == 1)
            config.showStats = !config.showStats;
        // Cycles by distance (-1), LOD 0, 1, ..., MAX_MESH_LODS - 1, by distance.
        for (uint32_t i = 0; i < events.forcedLodSteps; ++i)
            config.forcedLod = config.forcedLod + 1 >= static_cast<int32_t>(MAX_MESH_LODS) ? -1 : config.forcedLod + 1;
        if (events.dumpGraph)
            graphDumpRequested = true;

        events.camera.deltaTime = deltaTime;
        updateCamera(camera, events.camera);
    }

    std::string Engine::resolveAssetPath(const std::string &path) const
    {
        // Every loader expects UTF-8, whichever encoding the path arrived in.
        if (config.assetRoot.empty())
        {
            return pathToUtf8(pathFromUtf8(path));
        }
        return pathToUtf8((pathFromUtf8(config.assetRoot) / pathFromUtf8(path)).lexically_normal());
    }

    void Engine::waitForLoads()
    {
        finishLoads(true);
    }

    // Runs at the start of a frame, before anything binds the bindless set or the geometry pool, so
    // what it writes is in place for this frame's draws and its uploads go out with this frame's submit.
    // Without wait it stops at LOAD_BUDGET, so loads that finish together spread over several frames. With wait it
    // finishes what is ready, which gives bytes back to the load budget, then waits for the next load to end: a load
    // queued behind the budget can only start once the main thread has taken the ones before it.
    void Engine::finishLoads(bool wait)
    {
        if (!wait)
        {
            finishReadyLoads(true);
            return;
        }
        // What is filled is loaded once its copies are done, so the wait for them comes before the one for the next load.
        for (;;)
        {
            const uint64_t seen = workers->completions();
            if (!finishReadyLoads(false))
                return;
            if (ctx.isLoadUploadDone(loadUploadValue))
                workers->waitForCompletion(seen);
            else if (!ctx.waitForLoadUploads(loadUploadValue))
            {
                spdlog::error("waitForLoads: the load uploads did not go out; some loads stay unfinished.");
                return;
            }
        }
    }

    // A finished load's decoded data is freed on a worker, and its bytes go back to the load budget once it is freed, so
    // the budget does not count a result as gone while it is still in memory (except a glTF image that two textures
    // share, P26). Freeing a few MiB gives their pages back to the OS, which took 0.15 to 0.3 ms each on the main
    // thread (P26). A free that finds the pool stopping happens here, at once.
    void Engine::freeOnWorker(std::shared_ptr<const void> result, uint64_t bytes)
    {
        if (!result)
        {
            workers->releaseBytes(bytes);
            return;
        }
        WorkerPool *pool = workers.get();
        pool->pushChild([pool, result = std::move(result), bytes]() mutable
                        {
                            result.reset();
                            pool->releaseBytes(bytes); });
    }

    // A queued job never runs and a running one frees its result when it ends; a finished one's result goes to a worker
    // like a filled one's, its bytes after it (dropLoad returns them, also for a load that finished just now).
    void Engine::dropPendingLoad(LoadTicket &ticket, std::shared_ptr<const void> result)
    {
        freeOnWorker(std::move(result), workers->dropLoad(ticket));
    }

    // Returns whether loads are still pending. Textures come last: a finished glTF adds its textures, each filled on its
    // own (P2), so a big model's copies spread over frames too, and waitForLoads fills them in the same call. What this
    // fills is used only once its copies are done (publishUploadedLoads, in a later frame): a load's uploads need no wait
    // for the frames, on the transfer queue when there is one, and the frames never wait for them.
    bool Engine::finishReadyLoads(bool budgeted)
    {
        const auto deadline = std::chrono::steady_clock::now() + LOAD_BUDGET;
        auto overBudget = [budgeted, deadline]()
        { return budgeted && std::chrono::steady_clock::now() >= deadline; };

        publishUploadedLoads();
        ctx.beginLoadUploads();
        std::vector<MeshHandle> meshes;
        std::vector<EnvironmentHandle> uploadedEnvironments;
        std::vector<PendingTextureLoad> filledTextures;

        for (size_t i = 0; i < pendingMeshes.size() && !overBudget();)
        {
            if (pendingMeshes[i].job->ticket.state.load(std::memory_order_acquire) != LoadState::DONE)
            {
                ++i;
                continue;
            }
            PendingMeshLoad load = std::move(pendingMeshes[i]);
            pendingMeshes.erase(pendingMeshes.begin() + static_cast<std::ptrdiff_t>(i));
            finishMeshLoad(load);
            if (isLive(load.handle) && registry.meshes[load.handle.index].state == MeshState::LOADING)
                meshes.push_back(load.handle);
            // Its textures took their levels' bytes with them (loadMaterial); the rest of the result is freed on a worker.
            const uint64_t held = load.job->ticket.bytes.exchange(0);
            freeOnWorker(std::move(load.job), held - std::min(held, load.movedBytes));
        }

        for (size_t i = 0; i < pendingEnvironments.size() && !overBudget();)
        {
            if (pendingEnvironments[i].job->ticket.state.load(std::memory_order_acquire) != LoadState::DONE)
            {
                ++i;
                continue;
            }
            PendingEnvironmentLoad load = std::move(pendingEnvironments[i]);
            pendingEnvironments.erase(pendingEnvironments.begin() + static_cast<std::ptrdiff_t>(i));
            finishEnvironmentLoad(load);
            uploadedEnvironments.push_back(load.handle);
            const uint64_t held = load.job->ticket.bytes.exchange(0);
            freeOnWorker(std::move(load.job), held);
        }

        // At least one part of a texture a frame, so meshes that use up the budget frame after frame do not hold textures
        // back. A texture is filled in parts of TEXTURE_FILL_PART, so one big texture does not make a long frame either
        // (P24), and the ones begun go first, so few are half done at a time. An entry leaves the map while it is filled:
        // a fill that fails destroys its texture. One that is not done comes back with its levels; one that is comes back
        // without them, and waits for its copies.
        bool filledOne = false;
        for (const bool begun : {true, false})
        {
            for (auto it = pendingTextures.begin(); it != pendingTextures.end() && (!filledOne || !overBudget());)
            {
                const PendingTextureLoad &entry = it->second;
                const bool started = entry.progress.mip > 0 || entry.progress.row > 0;
                if (entry.filled || started != begun || (entry.job && entry.job->ticket.state.load(std::memory_order_acquire) != LoadState::DONE))
                {
                    ++it;
                    continue;
                }
                PendingTextureLoad load = std::move(it->second);
                it = pendingTextures.erase(it);
                // A part that copied nothing found the staging ring full: the rest waits for a later frame.
                bool finished = false;
                bool moved = true;
                while (!finished && moved && (!filledOne || !overBudget()))
                {
                    const TextureFillProgress before = load.progress;
                    finished = finishTextureLoad(load, budgeted ? TEXTURE_FILL_PART : UINT64_MAX);
                    moved = load.progress.mip != before.mip || load.progress.row != before.row;
                    filledOne = true;
                }
                if (!finished)
                {
                    filledTextures.push_back(std::move(load));
                    continue;
                }
                // A texture has its own job (loadTexture) or its glTF's levels (loadMaterial), never both.
                if (load.job)
                {
                    const uint64_t held = load.job->ticket.bytes.exchange(0);
                    freeOnWorker(std::move(load.job), held);
                }
                else
                    freeOnWorker(std::move(load.levels), load.bytes);
                if (ctx.isAlive(load.handle))
                    filledTextures.push_back({.handle = load.handle, .path = std::move(load.path), .filled = true});
                else
                    gpuScene->texturesFinished = true;
            }
        }

        const uint64_t value = ctx.endLoadUploads();
        loadUploadValue = value;
        for (MeshHandle mesh : meshes)
            uploadingMeshes.emplace_back(mesh, value);
        for (EnvironmentHandle environment : uploadedEnvironments)
        {
            if (isLive(environment))
                environments->environments[environment.index].uploadValue = value;
        }
        for (PendingTextureLoad &texture : filledTextures)
        {
            texture.uploadValue = value;
            const TextureHandle handle = texture.handle;
            pendingTextures.emplace(handle, std::move(texture));
        }
        // An environment whose image is still copied bakes later, so it is still loading too.
        bool environmentsUploading = false;
        for (const Environment &environment : environments->environments)
            environmentsUploading = environmentsUploading || (environment.state == BakeState::WAITING && !ctx.isLoadUploadDone(environment.uploadValue));
        return !pendingMeshes.empty() || !pendingEnvironments.empty() || !pendingTextures.empty() || !uploadingMeshes.empty() || environmentsUploading;
    }

    // A mesh and a texture are loaded once the GPU has their copies; an environment is baked then (recordEnvironmentBakes).
    // The frame's submits wait for what is published here, which is done, so they never wait for copies in flight.
    void Engine::publishUploadedLoads()
    {
        ctx.updateLoadUploadsDone();
        for (auto it = pendingTextures.begin(); it != pendingTextures.end();)
        {
            if (!it->second.filled || !ctx.isLoadUploadDone(it->second.uploadValue))
            {
                ++it;
                continue;
            }
            it = pendingTextures.erase(it);
            // syncGpuScene checks the entities that wait for textures only after one finished.
            gpuScene->texturesFinished = true;
        }
        std::erase_if(uploadingMeshes, [this](const std::pair<MeshHandle, uint64_t> &mesh)
                      {
                          if (!ctx.isLoadUploadDone(mesh.second))
                              return false;
                          // A mesh destroyed meanwhile (its handle or its slot's new mesh) is left alone.
                          if (isLive(mesh.first) && registry.meshes[mesh.first.index].state == MeshState::LOADING)
                              registry.meshes[mesh.first.index].state = MeshState::READY;
                          return true; });
    }

    LoadStats Engine::getLoadStats() const
    {
        const WorkerPoolStats pool = workers->stats();
        return {.workers = pool.workers, .queued = pool.queued, .running = pool.running, .decodedBytes = pool.bytes, .peakDecodedBytes = pool.peakBytes};
    }

    TextureHandle Engine::createTexture(const TextureDesc &desc)
    {
        return ctx.createTexture(desc);
    }

    BufferHandle Engine::createBuffer(const BufferDesc &desc)
    {
        return ctx.createBuffer(desc);
    }

    RenderTargetHandle Engine::createRenderTarget(const RenderTargetDesc &desc)
    {
        return ctx.createRenderTarget(desc);
    }

    void Engine::destroyBuffer(const BufferHandle &buffer)
    {
        ctx.destroyBuffer(buffer);
    }

    void Engine::destroyRenderTarget(const RenderTargetHandle &renderTarget)
    {
        ctx.destroyRenderTarget(renderTarget);
    }

    RenderGraph Engine::createRenderGraph()
    {
        RenderGraph graph(this);
        return graph;
    }

    // The swapchain has no depth, so neither has the pipeline.
    PipelineHandle Engine::createPipeline(const std::string &name, const std::string &shaderSource)
    {
        const Format colorFormats[MAX_COLOR_ATTACHMENTS] = {ctx.getSwapchainImageFormat()};
        return createGraphicsPipeline(name, shaderSource, colorFormats, Format::UNKNOWN, "vertMain", "fragMain", BlendMode::NONE, true);
    }

    // The pipeline draws into targets with the formats of this one: its colors, in order, and its depth. A depth-only
    // target gives a pipeline without colors.
    PipelineHandle Engine::createPipeline(const std::string &name, const std::string &shaderSource, const RenderTargetHandle &renderTarget)
    {
        const auto &rt = ctx.getRenderTarget(renderTarget);
        // Every render target has a depth format, so an invalid or stale handle is the one without.
        if (rt.depthFormat == Format::UNKNOWN)
        {
            spdlog::error("Pipeline '{}' needs a valid render target.", name);
            return {};
        }
        return createGraphicsPipeline(name, shaderSource, rt.colorFormats, rt.depthFormat, "vertMain", "fragMain", BlendMode::NONE, true, rt.samples);
    }

    // A pipeline of the scene passes, from a file in the shader folder, for targets with this one's formats and sample
    // count. defines are preprocessor lines put before the source, for the variants of one shader; #line keeps the
    // diagnostics' line numbers those of the file.
    PipelineHandle Engine::createScenePipeline(const std::string &name, const std::string &shaderFile, const std::string &defines, const RenderTargetHandle &renderTarget,
                                               const char *vertexEntryPoint, const char *fragmentEntryPoint, BlendMode blendMode, bool depthWrite, bool alphaToCoverage, bool depthTest)
    {
        const auto &rt = ctx.getRenderTarget(renderTarget);
        if (rt.depthFormat == Format::UNKNOWN)
        {
            spdlog::error("Pipeline '{}' needs a valid render target.", name);
            return {};
        }
        const std::string file = readFile(pathFromUtf8(config.shaderDir) / shaderFile);
        const std::string source = defines.empty() ? file : defines + "#line 1\n" + file;
        return createGraphicsPipeline(name, source, rt.colorFormats, rt.depthFormat, vertexEntryPoint, fragmentEntryPoint, blendMode, depthWrite, rt.samples, alphaToCoverage, depthTest);
    }

    PipelineHandle Engine::createGraphicsPipeline(const std::string &name, const std::string &shaderSource, const Format *colorFormats, Format depthFormat, const char *vertEntryPoint,
                                                  const char *fragEntryPoint, BlendMode blendMode, bool depthWrite, uint32_t samples, bool alphaToCoverage, bool depthTest)
    {
        ZoneScoped;

        auto vertShader = shaderCompiler->compileFromString(shaderSource, name + ".vert.slang", vertEntryPoint);
        auto fragShader = shaderCompiler->compileFromString(shaderSource, name + ".frag.slang", fragEntryPoint);

        if (vertShader.code.empty() || fragShader.code.empty())
        {
            spdlog::error("Failed to compile the shaders of pipeline '{}'.", name);
            return {};
        }

        PipelineDesc desc = {.name = "Pipeline for " + name,
                             .vertexShader = ctx.createShader({.name = name + ".vert", .type = ShaderType::VERTEX, .entryPoint = vertEntryPoint, .code = std::move(vertShader.code)}),
                             .fragmentShader = ctx.createShader({.name = name + ".frag", .type = ShaderType::FRAGMENT, .entryPoint = fragEntryPoint, .code = std::move(fragShader.code)}),
                             .descriptorSetLayout = bindlessDescriptorSetLayout,
                             .topology = PrimitiveTopology::TRIANGLE_LIST,
                             .depthStencilAttachmentFormat = depthFormat,
                             .blendMode = blendMode,
                             .depthWrite = depthWrite,
                             .depthTest = depthTest,
                             .samples = samples,
                             .alphaToCoverage = alphaToCoverage};
        std::copy(colorFormats, colorFormats + MAX_COLOR_ATTACHMENTS, std::begin(desc.colorAttachmentFormats));
        return ctx.createPipeline(desc);
    }

    // A mesh pipeline of the scene passes: the file's taskMain (lib/meshletTask.slang), its mesh and fragment entry points,
    // for targets with this one's formats and sample count.
    PipelineHandle Engine::createMeshScenePipeline(const std::string &name, const std::string &shaderFile, const std::string &defines, const RenderTargetHandle &renderTarget,
                                                   const char *meshEntryPoint, const char *fragmentEntryPoint, BlendMode blendMode, bool depthWrite, bool alphaToCoverage)
    {
        ZoneScoped;

        const auto &rt = ctx.getRenderTarget(renderTarget);
        const std::string file = readFile(pathFromUtf8(config.shaderDir) / shaderFile);
        const std::string source = defines.empty() ? file : defines + "#line 1\n" + file;
        auto taskShader = shaderCompiler->compileFromString(source, name + ".task.slang", "taskMain");
        auto meshShader = shaderCompiler->compileFromString(source, name + ".mesh.slang", meshEntryPoint);
        auto fragShader = shaderCompiler->compileFromString(source, name + ".frag.slang", fragmentEntryPoint);
        if (taskShader.code.empty() || meshShader.code.empty() || fragShader.code.empty())
        {
            spdlog::error("Failed to compile the shaders of mesh pipeline '{}'.", name);
            return {};
        }

        PipelineDesc desc = {.name = "Mesh Pipeline for " + name,
                             .fragmentShader = ctx.createShader({.name = name + ".frag", .type = ShaderType::FRAGMENT, .entryPoint = fragmentEntryPoint, .code = std::move(fragShader.code)}),
                             .taskShader = ctx.createShader({.name = name + ".task", .type = ShaderType::TASK, .entryPoint = "taskMain", .code = std::move(taskShader.code)}),
                             .meshShader = ctx.createShader({.name = name + ".mesh", .type = ShaderType::MESH, .entryPoint = meshEntryPoint, .code = std::move(meshShader.code)}),
                             .descriptorSetLayout = bindlessDescriptorSetLayout,
                             .depthStencilAttachmentFormat = rt.depthFormat,
                             .blendMode = blendMode,
                             .depthWrite = depthWrite,
                             .samples = rt.samples,
                             .alphaToCoverage = alphaToCoverage};
        std::copy(rt.colorFormats, rt.colorFormats + MAX_COLOR_ATTACHMENTS, std::begin(desc.colorAttachmentFormats));
        return ctx.createPipeline(desc);
    }

    PipelineHandle Engine::createComputePipeline(const std::string &name, const std::string &shaderSource, const std::string &entryPoint)
    {
        ZoneScoped;

        auto compShader = shaderCompiler->compileFromString(shaderSource, name + ".comp.slang", entryPoint);
        ShaderHandle shader;
        if (!compShader.code.empty())
            shader = ctx.createShader({.name = name + ".comp", .type = ShaderType::COMPUTE, .entryPoint = entryPoint, .code = std::move(compShader.code)});
        // An invalid handle would turn the desc into a graphics pipeline with no shaders.
        if (!shader.isValid())
        {
            spdlog::error("Failed to create the shader of compute pipeline '{}'.", name);
            return {};
        }

        return ctx.createPipeline({.name = "Compute Pipeline for " + name, .computeShader = shader, .descriptorSetLayout = bindlessDescriptorSetLayout});
    }

    bool Engine::beginFrame(CameraHandle camera)
    {
        ZoneScoped;

        pollEvents(camera);
        finishLoads(false);
        // No frame is being recorded, so what was destroyed since the last frame waits only for work already submitted.
        // Here, and not after the fence wait only, so frees go on while frames are skipped (a minimized window).
        ctx.tagReleases({.fence = frameFence, .value = frameFenceValue}, {.fence = computeFence, .value = computeFenceValue});
        ctx.collectReleases();

        // Nothing can be presented to a minimized or zero-sized window (B5).
        if (mainWindow.minimized || mainWindow.pixelWidth == 0 || mainWindow.pixelHeight == 0)
            return false;

        if (mainWindow.resized || swapchainOutOfDate)
        {
            mainWindow.resized = false;
            swapchainOutOfDate = !recreateSwapchain();
            if (swapchainOutOfDate)
                return false;
        }

        // Wait for the GPU to finish the previous frame in this slot before its command buffers are reused.
        auto &currentFrame = frameResources[frameCount];
        if (!ctx.waitForFence(frameFence, currentFrame.fenceValue))
            return false;
        if (asyncCompute && !ctx.waitForFence(computeFence, currentFrame.computeFenceValue))
            return false;
        if (readbackComputeWait > 0 && ctx.getFenceValue(frameFence) >= readbackComputeWait)
            readbackComputeWait = 0;
        ctx.collectReleases();

        // After the wait: they read what the slot's last frame measured and rewrite the tables it read.
        updateFrameStats();
        if (!ui)
            updateStatsOverlay();
#if defined(RASM_IMGUI)
        if (ui)
            buildUi(camera);
#endif
        syncGpuScene();

        // B4
        auto status = ctx.acquireNextImage(imageIdx);
        if (status == SwapchainStatus::OUT_OF_DATE)
        {
            swapchainOutOfDate = true;
            return false;
        }
        if (status == SwapchainStatus::FAILURE)
            return false;
        if (status == SwapchainStatus::SUBOPTIMAL)
            swapchainOutOfDate = true; // The image is acquired and still usable; recreate after this frame.

        swapchainRendered = false;

        // record commands for the current frame
        for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
        {
            batchesUsed[q] = 0;
            batchOpen[q] = false;
        }
        batchValues[static_cast<uint32_t>(QueueType::GRAPHICS)] = frameFenceValue;
        batchValues[static_cast<uint32_t>(QueueType::COMPUTE)] = computeFenceValue;
        closedBatches.clear();
        recordingFailed = false;
        if (!useQueue(QueueType::GRAPHICS))
            return false;
        frameStats->frameTimer = beginGpuTimer(currentFrame.commandBuffer, "Frame");
        ctx.clearNewHistory(currentFrame.commandBuffer);
        recordEnvironmentBakes();
        return true;
    }

    void Engine::endFrame()
    {
        ZoneScoped;

        // The rest of the frame is graphics work, and the frame's last graphics submit presents.
        if (useQueue(QueueType::GRAPHICS))
        {
            auto &currentFrame = frameResources[frameCount];

            // Present needs the image in PRESENT_SRC, so a frame that only drew to render targets clears it.
            if (!swapchainRendered)
            {
                auto renderTarget = currentRenderTarget;
                const uint32_t face = currentFace;
                resetRenderTarget();
                beginPass("Clear");
                endPass();
                setRenderTarget(renderTarget, face);
            }
#if defined(RASM_IMGUI)
            drawUiFallback();
#endif
            if (!ui)
                drawStatsOverlay();
            recordReadbacks();
            // An offscreen swapchain image is never presented; the next acquire of it drops what it holds.
            if (config.platform != PlatformType::HEADLESS)
                ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::PRESENT_SRC);
            // updateFrameStats reads the cull stats on the host, after the slot's fence. With a compute queue the
            // culling runs there, and submitBatches puts the barrier in its last batch.
            if (!asyncCompute)
                ctx.memoryBarrier(currentFrame.commandBuffer, PipelineStage::COMPUTE, PipelineStage::HOST);
            endGpuTimer(currentFrame.commandBuffer, frameStats->frameTimer);

            // The frame ends when both queues are done with it: compute work that nothing drew with still finishes
            // before this slot's graphics fence says so, and before the next frame's graphics work starts.
            if (asyncCompute)
                waitForQueueWork(QueueType::GRAPHICS, QueueType::COMPUTE);
        }
        closeBatch(QueueType::COMPUTE);
        closeBatch(QueueType::GRAPHICS);
        const bool presenting = submitBatches();
        ctx.tagReleases({.fence = frameFence, .value = frameFenceValue}, {.fence = computeFence, .value = computeFenceValue});
        finishReadbackSubmit(presenting);
        finishEnvironmentBakes();
        if (presenting)
        {
            auto status = ctx.present();
            if (status == SwapchainStatus::SUBOPTIMAL || status == SwapchainStatus::OUT_OF_DATE)
                swapchainOutOfDate = true;
        }

        frameCount = (frameCount + 1) % MAX_FRAMES_IN_FLIGHT; // Toggle between 0 and 1 for double buffering

        FrameMark;
    }

    // A batch is the commands one queue runs between two waits for the other queue: one command buffer and one
    // submit. Each queue records into one open batch at a time, and a batch is closed when the other queue needs
    // what it did (waitForQueue) or at the end of the frame. Recording moves between the open batches as passes
    // change queue, and FrameResources::commandBuffer names the one in use. Without a compute queue every pass
    // records into the graphics batch. A batch that can't be opened drops the frame: nothing more is recorded into
    // it, and endFrame submits none of it.
    bool Engine::useQueue(QueueType queue)
    {
        if (recordingFailed)
            return false;
        if (!asyncCompute)
            queue = QueueType::GRAPHICS;
        const auto q = static_cast<uint32_t>(queue);
        auto &commandBuffers = batchCommandBuffers[frameCount][q];
        if (!batchOpen[q])
        {
            if (batchesUsed[q] == commandBuffers.size())
            {
                const std::string name = std::string(queue == QueueType::COMPUTE ? "Compute" : "Frame") + " Command Buffer " +
                                         std::to_string(frameCount) + "." + std::to_string(batchesUsed[q]);
                const CommandBufferHandle created = ctx.createCommandBuffer(name, queue);
                if (!created.isValid())
                {
                    spdlog::error("Failed to create '{}'; the frame is dropped.", name);
                    recordingFailed = true;
                    return false;
                }
                commandBuffers.push_back(created);
            }
            if (!ctx.beginCommandBuffer(commandBuffers[batchesUsed[q]]))
            {
                spdlog::error("Failed to begin a command buffer; the frame is dropped.");
                recordingFailed = true;
                return false;
            }
            ++batchesUsed[q];
            batchOpen[q] = true;
            for (uint32_t x = 0; x < QUEUE_COUNT; ++x)
                batchWaits[q][x] = 0;
            // Readback copies run outside any graph, so no graph orders a later compute use of their textures after
            // them. The frame that made them was submitted before this one, so the wait is on a batch already out.
            if (queue == QueueType::COMPUTE)
                batchWaits[q][static_cast<uint32_t>(QueueType::GRAPHICS)] = readbackComputeWait;
        }
        recordingQueue = queue;
        frameResources[frameCount].commandBuffer = commandBuffers[batchesUsed[q] - 1];
        return true;
    }

    // The value the queue's open batch will signal.
    uint64_t Engine::openBatchValue(QueueType queue) const
    {
        return batchValues[static_cast<uint32_t>(queue)] + 1;
    }

    // The waiter's open batch starts only once the queue reaches the value. A value of the queue's open batch
    // closes that batch first, so every wait is on a batch closed, and later submitted, before the waiter's.
    void Engine::waitForQueue(QueueType waiter, QueueType queue, uint64_t value)
    {
        if (!asyncCompute || waiter == queue)
            return;
        const auto q = static_cast<uint32_t>(queue);
        if (value > batchValues[q] && batchOpen[q])
            closeBatch(queue);
        auto &wait = batchWaits[static_cast<uint32_t>(waiter)][q];
        wait = std::max(wait, std::min(value, batchValues[q]));
    }

    // Waits for everything recorded on the queue so far.
    void Engine::waitForQueueWork(QueueType waiter, QueueType queue)
    {
        waitForQueue(waiter, queue, openBatchValue(queue));
    }

    void Engine::closeBatch(QueueType queue)
    {
        const auto q = static_cast<uint32_t>(queue);
        if (!batchOpen[q])
            return;
        batchOpen[q] = false;
        const CommandBufferHandle commandBuffer = batchCommandBuffers[frameCount][q][batchesUsed[q] - 1];
        ClosedBatch batch = {.queue = queue, .commandBuffer = commandBuffer, .value = ++batchValues[q]};
        std::copy(std::begin(batchWaits[q]), std::end(batchWaits[q]), std::begin(batch.waits));
        closedBatches.push_back(batch);
    }

    // The batches go out together at the end of the frame, in the order they were closed, so the GPU starts the
    // frame once it is recorded, as with a single submit, and every wait is on a batch already submitted. The
    // last graphics batch presents. A failed submit ends the frame there: what follows may wait for it, and a wait
    // for a value nothing signals never ends. A dropped frame submits nothing. Returns whether the present submit
    // went out.
    bool Engine::submitBatches()
    {
        ZoneScoped;

        size_t presentBatch = closedBatches.size();
        size_t lastCompute = closedBatches.size();
        for (size_t i = 0; i < closedBatches.size(); ++i)
        {
            if (closedBatches[i].queue == QueueType::GRAPHICS)
                presentBatch = i;
            else
                lastCompute = i;
        }

        // Command buffers end only here, so the frame's last compute batch, often closed long before the end,
        // still takes the barrier: updateFrameStats reads the cull stats on the host, after the slot's fences.
        // Every one is ended, so none is still recording at its next begin.
        bool recorded = !recordingFailed;
        for (size_t i = 0; i < closedBatches.size(); ++i)
        {
            if (i == lastCompute)
                ctx.memoryBarrier(closedBatches[i].commandBuffer, PipelineStage::COMPUTE, PipelineStage::HOST);
            recorded = ctx.endCommandBuffer(closedBatches[i].commandBuffer) && recorded;
        }
        if (!recorded)
        {
            ctx.discardRecording();
            return false;
        }

        for (size_t i = 0; i < closedBatches.size(); ++i)
        {
            const ClosedBatch &batch = closedBatches[i];
            const bool compute = batch.queue == QueueType::COMPUTE;
            FenceValue waits[QUEUE_COUNT] = {};
            uint32_t waitCount = 0;
            for (uint32_t x = 0; x < QUEUE_COUNT; ++x)
            {
                if (batch.waits[x] > 0)
                    waits[waitCount++] = {.fence = x == static_cast<uint32_t>(QueueType::COMPUTE) ? computeFence : frameFence, .value = batch.waits[x]};
            }

            // The slot only takes the new value once a submit will signal it; a value nothing signals would block
            // the next wait on this slot forever. Every graphics submit may wait for the swapchain image, and only
            // the frame's first one does. After a failed present submit the swapchain keeps the image acquired,
            // and the next acquire hands it out again.
            const bool submitted = ctx.submit({.queue = batch.queue,
                                               .commandBuffers = &batch.commandBuffer,
                                               .commandBufferCount = 1,
                                               .waits = waits,
                                               .waitCount = waitCount,
                                               .signal = {.fence = compute ? computeFence : frameFence, .value = batch.value},
                                               .waitsForSwapchainImage = !compute,
                                               .presentsSwapchainImage = i == presentBatch});
            if (!submitted)
            {
                ctx.discardRecording();
                return false;
            }
            if (compute)
                computeFenceValue = frameResources[frameCount].computeFenceValue = batch.value;
            else
                frameFenceValue = frameResources[frameCount].fenceValue = batch.value;
        }
        return presentBatch < closedBatches.size();
    }

    void Engine::beginPass(const std::string &name, const AttachmentOps &ops)
    {
        auto currentFrame = frameResources[frameCount];
        frameStats->passTimer = beginGpuTimer(currentFrame.commandBuffer, name);
        uint32_t width = swapchain.width;
        uint32_t height = swapchain.height;
        TextureHandle colors[MAX_COLOR_ATTACHMENTS] = {swapchain.imageHandles[imageIdx]};
        uint32_t colorCount = 1;
        // The swapchain has no depth: the scene draws into a render target, and the tone-map pass brings it here.
        TextureHandle depth = {};
        glm::vec4 clearColor = BACKGROUND_COLOR;
        if (currentRenderTarget.isValid())
        {
            const auto &renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            width = renderTargetData.width;
            height = renderTargetData.height;
            colorCount = renderTargetData.colorCount;
            std::copy(std::begin(renderTargetData.attachments[frameCount].color), std::end(renderTargetData.attachments[frameCount].color), std::begin(colors));
            depth = renderTargetData.attachments[frameCount].depth;
            clearColor = renderTargetData.clearColor;
        }

        // What the pass clears is dropped. The barrier covers all of a cube's faces, so it drops a cube only in the frame's
        // first pass on it: a pass that clears one face keeps the others.
        for (uint32_t i = 0; i < colorCount; ++i)
            ctx.transitionImageLayout(currentFrame.commandBuffer, colors[i], TextureUsage::COLOR_ATTACHMENT, !ops.loadColor);
        if (depth.isValid())
            ctx.transitionImageLayout(currentFrame.commandBuffer, depth, TextureUsage::DEPTH_STENCIL_ATTACHMENT, !ops.loadDepth && (ops.firstWrite || !ctx.getTextureDesc(depth).cube));
        ctx.beginRendering(currentFrame.commandBuffer, colors, colorCount, depth, clearColor, ops, currentRenderTarget.isValid() ? currentFace : 0);

        // begin dynamic rendering
        ctx.setViewport(currentFrame.commandBuffer, 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        ctx.setScissor(currentFrame.commandBuffer, 0, 0, width, height);
        // Every pipeline takes its cull mode from the command buffer; drawSceneView changes it and restores this.
        ctx.setCullMode(currentFrame.commandBuffer, CullMode::NONE);
    }

    void Engine::endPass()
    {
        auto currentFrame = frameResources[frameCount];
        ctx.endRendering(currentFrame.commandBuffer);

        if (currentRenderTarget.isValid())
        {
            // Later passes sample the color attachment through the bindless set, whose descriptors promise
            // this layout (B10). Depth stays an attachment; a graph pass that reads it moves it (PassBuilder::readDepth).
            const auto &renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            for (uint32_t i = 0; i < renderTargetData.colorCount; ++i)
                ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].color[i], TextureUsage::SAMPLED);
        }
        else
        {
            // endFrame moves it to PRESENT_SRC.
            swapchainRendered = true;
        }
        endGpuTimer(currentFrame.commandBuffer, frameStats->passTimer);
    }

    void Engine::setRenderTarget(const RenderTargetHandle &renderTarget, uint32_t face)
    {
        this->currentRenderTarget = renderTarget;
        this->currentFace = face;
    }

    void Engine::resetRenderTarget()
    {
        this->currentRenderTarget = RenderTargetHandle{};
        this->currentFace = 0;
    }

    bool Engine::recreateSwapchain()
    {
        auto status = ctx.recreateSwapchain();
        if (status != SwapchainStatus::OK)
        {
            // A 0x0 surface keeps the old swapchain, while a failure may have destroyed it with its images.
            swapchain.imageHandles = ctx.getSwapchainImages();
            return false;
        }
        if (!createSwapchainResources())
            return false;
        spdlog::info("Recreated swapchain: {}x{}", swapchain.width, swapchain.height);
        return true;
    }

    // Called at startup and after a successful swapchain recreation, both with the device idle.
    bool Engine::createSwapchainResources()
    {
        swapchain.imageHandles = ctx.getSwapchainImages();
        swapchain.imageFormat = ctx.getSwapchainImageFormat();

        // Size everything from the swapchain extent. It is in pixels and can differ from the
        // window's logical size on HiDPI displays (B17).
        auto extent = ctx.getSwapchainExtent();
        swapchain.width = extent.x;
        swapchain.height = extent.y;

        ctx.resizeRenderTargets();
        updateCameraAspectRatio(static_cast<float>(swapchain.width) / static_cast<float>(swapchain.height));
        return true;
    }

    void Engine::render(SceneHandle scene, CameraHandle camera)
    {
        // The renderer is made last in the constructor, so an engine that failed to start has none.
        if (renderer)
            renderer->render({.scene = scene, .camera = camera});
    }

    void Engine::setInspection(const Inspection &value)
    {
        if (!inspector)
            spdlog::warn("setInspection: the UI is off (EngineConfig::ui), so nothing is shown.");
        // The sections open for a new pick only, so a call every frame (a new exposure) leaves the window as it is.
        const bool newPick = value.target != inspection.target || value.kind != inspection.kind || value.attachment != inspection.attachment;
        const bool newEntity = !(value.entity == inspection.entity);
        inspection = value;
        if (inspector && newPick)
            inspector->openBuffers = !value.target.empty();
        if (inspector && newEntity)
        {
            inspector->openTables = value.entity.isValid();
            inspector->openRows = value.entity.isValid();
        }
    }

    const Inspection &Engine::getInspection() const
    {
        return inspection;
    }

#if !defined(RASM_IMGUI)
    // Built without Dear ImGui: the UI is always off.
    bool Engine::addUiPass(RenderGraph &)
    {
        return false;
    }

    void Engine::updateInspectPass(RenderGraph &) {}

    void Engine::setUiCallback(std::function<void()>)
    {
        spdlog::warn("setUiCallback: RASM was built without Dear ImGui (RASM_IMGUI=OFF), so the callback never runs.");
    }

    uint64_t Engine::getUiTexture(const TextureHandle &)
    {
        return 0;
    }
#endif

    bool Engine::running() const
    {
        return isRunning;
    }

    VRAMStats Engine::queryMemoryStats()
    {
        return ctx.queryVRAMStats();
    }

    const FrameResources &Engine::getCurrentFrameResources()
    {
        return frameResources[frameCount % MAX_FRAMES_IN_FLIGHT];
    }

    EngineConfig Engine::getConfig() const
    {
        return config;
    }

    const Window &Engine::getMainWindow() const
    {
        return mainWindow;
    }

    RenderContext &Engine::getRenderContext()
    {
        return ctx;
    }

    DescriptorSetHandle Engine::getBindlessDescriptorSet()
    {
        return bindlessDescriptorSet;
    }

    uint32_t Engine::addBindlessTexture(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, SamplerType sampler, uint32_t slot)
    {
        auto txtID = ctx.updateBindlessDescriptorSet(bindlessSet, texture, sampler, slot);
        if (txtID == UINT32_MAX)
            spdlog::error("Failed to add bindless texture.");
        return txtID;
    }

    uint32_t Engine::getBindlessTextureIndex(const TextureHandle &texture)
    {
        return ctx.getBindlessTextureIndex(texture);
    }

    PoolStats Engine::getPoolStats() const
    {
        const GeometryPoolStats geometry = geometryPool->stats();
        return {.entities = static_cast<uint32_t>(registry.entityHandles.liveCount()),
                .meshes = static_cast<uint32_t>(registry.meshHandles.liveCount()),
                .materials = static_cast<uint32_t>(registry.materialHandles.liveCount()),
                .lights = static_cast<uint32_t>(registry.lightHandles.liveCount()),
                .textures = ctx.getTextureCount(),
                .bindlessTextures = ctx.getBindlessTextureCount(),
                .vertices = geometry.vertices,
                .indices = geometry.indices,
                .subMeshes = geometry.subMeshes,
                .meshlets = geometry.meshlets,
                .instances = gpuScene->instanceRows.used(),
                .animationPoolBytes = gpuAnimation->poolRanges.used(),
                .releasesWaiting = ctx.getReleasesWaiting()};
    }

    bool Engine::isLive(const EntityHandle &entity) const
    {
        return registry.entityHandles.isAlive(entity.index, entity.generation);
    }

    bool Engine::isLive(const MeshHandle &mesh) const
    {
        return registry.meshHandles.isAlive(mesh.index, mesh.generation);
    }

    bool Engine::isLive(const MaterialHandle &material) const
    {
        return registry.materialHandles.isAlive(material.index, material.generation);
    }

    bool Engine::isLive(const CameraHandle &camera) const
    {
        return registry.cameraHandles.isAlive(camera.index, camera.generation);
    }

    bool Engine::isLive(const LightHandle &light) const
    {
        return registry.lightHandles.isAlive(light.index, light.generation);
    }

    bool Engine::isLive(const SceneHandle &scene) const
    {
        return registry.sceneHandles.isAlive(scene.index, scene.generation);
    }

    bool Engine::isLive(const EnvironmentHandle &environment) const
    {
        return registry.environmentHandles.isAlive(environment.index, environment.generation);
    }

    TextureHandle Engine::getRenderTargetTexture(const RenderTargetHandle &renderTarget, uint32_t attachment)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        if (attachment >= rt.colorCount)
            return {};
        return rt.attachments[frameCount % MAX_FRAMES_IN_FLIGHT].color[attachment];
    }

    TextureHandle Engine::getRenderTargetDepthTexture(const RenderTargetHandle &renderTarget)
    {
        return ctx.getRenderTarget(renderTarget).attachments[frameCount % MAX_FRAMES_IN_FLIGHT].depth;
    }

    uint32_t Engine::getRenderTargetStorageIndex(const RenderTargetHandle &renderTarget, uint32_t attachment)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        if (attachment >= rt.colorCount)
            return UINT32_MAX;
        return rt.storageSlots[frameCount % MAX_FRAMES_IN_FLIGHT][attachment];
    }

    TextureHandle Engine::getPreviousRenderTargetTexture(const RenderTargetHandle &renderTarget, uint32_t attachment)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        if (!rt.history || attachment >= rt.colorCount)
            return {};
        return rt.attachments[(frameCount + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT].color[attachment];
    }

} // namespace rasm
