#include "rasm/renderer.h"
#include "core/utils.h"
#include "renderer/loadHooks.h"
#include "platform/input.h"
#include "platform/paths.h"
#include "platform/platform.h"
#include "renderer/environmentBake.h"
#include "renderer/geometryPool.h"
#include "renderer/frameStats.h"
#include "renderer/gpuScene.h"
#include "renderer/inspector.h"
#include "renderer/lightClusters.h"
#include "renderer/picking.h"
#include "renderer/readback.h"
#include "renderer/animation.h"
#include "renderer/viewGraph.h"
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

    // Without SDL: the application's window, or offscreen images when it gives none.
    Renderer::Renderer(const RendererConfig &config) : Renderer(config, config.nativeWindow.type != NativeWindowType::NONE ? createNativePlatform() : createHeadlessPlatform()) {}

    Renderer::Renderer(const RendererConfig &config, std::unique_ptr<Platform> windowPlatform) : config(config), geometryPool(std::make_unique<GeometryPool>()), gpuScene(std::make_unique<GpuScene>()), gpuLights(std::make_unique<GpuLights>()), gpuAnimation(std::make_unique<GpuAnimation>()), environments(std::make_unique<Environments>()), frameStats(std::make_unique<FrameStatsCollector>()), statsOverlay(std::make_unique<StatsOverlay>()), shaderCompiler(std::make_unique<ShaderCompiler>()), platform(std::move(windowPlatform))
    {
        readbacks = std::make_unique<Readbacks>();
        picks = std::make_unique<Picks>();
        // Room for a burst of events, so a steady frame does not grow the queue (P27).
        inputState = std::make_unique<InputState>();
        inputEvents.reserve(256);
        pendingInputEvents.reserve(256);
        registry.lightHandles = HandlePool(static_cast<uint64_t>(config.maxLights) + 1);
        registry.materialHandles = HandlePool(static_cast<uint64_t>(config.maxMaterials) + 1);
        // In a real implementation, this is where we'd initialize the window, graphics context, etc.
        spdlog::info("Starting the renderer: appName={}, windowWidth={}, windowHeight={}, enableValidation={}",
                     config.appName, config.windowWidth, config.windowHeight, config.enableValidation);

        this->config.shaderDir = resolveFromBaseDir(config.shaderDir.empty() ? RASM_SHADER_DIR : config.shaderDir);

        if (config.windowWidth <= 0 || config.windowHeight <= 0)
        {
            spdlog::error("Window dimensions must be positive.");
            return;
        }
        // After the check, so an engine that stops here makes no folder.
        this->config.cacheDir = resolveCacheDir(config.cacheDir);
        if (!this->config.cacheDir.empty())
            spdlog::info("Shader and pipeline caches in {}", this->config.cacheDir);

        const bool headless = platform->type() == PlatformType::HEADLESS;
        // The main window is the first one, so its handle index is MAIN_WINDOW_INDEX.
        const HandleSlot mainSlot = windowHandles.allocate();
        windows[mainSlot.index] = platform->createWindow({.title = "RASM Engine",
                                                          .width = static_cast<uint32_t>(config.windowWidth),
                                                          .height = static_cast<uint32_t>(config.windowHeight),
                                                          .native = config.nativeWindow});
        windows[mainSlot.index].handle = {mainSlot.index, mainSlot.generation};
        if (!headless && windows[MAIN_WINDOW_INDEX].native.type == NativeWindowType::NONE)
        {
            spdlog::error("Failed to initialize engine window.");
            return;
        }

        if (!ctx.initialize({.backend = config.preferredBackend,
                             .appName = config.appName,
                             .window = &windows[MAIN_WINDOW_INDEX],
                             .headless = headless,
                             .enableValidation = config.enableValidation,
                             .enableSyncValidation = config.enableSyncValidation,
                             .enableGpuValidation = config.enableGpuValidation,
                             .asyncCompute = config.asyncCompute,
                             .transferQueue = config.transferQueue,
                             .hostImageCopy = config.hostImageCopy,
                             .sampledDepth = config.occlusionCulling,
                             .bindlessTextures = config.maxBindlessTextures,
                             .cacheDir = this->config.cacheDir}))
        {
            spdlog::error("Failed to initialize render context.");
            return;
        }
        // getConfig gives the slots the set has, lowered to the device's limit.
        this->config.maxBindlessTextures = ctx.getBindlessTextureCapacity();

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

        // The render context made the set; the pipelines and passes name it often.
        bindlessDescriptorSetLayout = ctx.getBindlessDescriptorSetLayout();
        bindlessDescriptorSet = ctx.getBindlessDescriptorSet();

        if (!createSwapchainResources(WindowHandle{}) || !createEnvironments())
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
            spdlog::warn("RendererConfig::ui is set, but RASM was built without Dear ImGui (RASM_IMGUI=OFF); the UI stays off.");
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

        viewGraph = std::make_unique<ViewGraph>(*this);
        isRunning = true;
    }

    Renderer::~Renderer()
    {
        // The loader must go first (an Engine's does). One that is still here drops its loads and refuses every call
        // from now on, so destroying it later is safe.
        if (loads)
        {
            spdlog::error("A Renderer is destroyed before its AssetLoader: destroy the AssetLoader first. Its loads are dropped, and it refuses every call from now on.");
            loads->detach();
            loads = nullptr;
        }
        // Its graph frees what it made through the context.
        viewGraph.reset();
        shaderCompiler->shutdown();
#if defined(RASM_IMGUI)
        destroyUi();
#endif
        ctx.cleanup();

        // The main window last: the SDL platform quits SDL with its last window.
        for (uint32_t i = MAX_WINDOWS; i-- > 0;)
        {
            if (platform && windows[i].handle.isValid())
                platform->destroyWindow(windows[i]);
        }
        spdlog::info("Renderer shutdown, cleaned up resources.");
    }

    // The platform opens or wraps the window, and the render context gives it a swapchain. On failure nothing is left.
    WindowHandle Renderer::addWindow(const WindowDesc &desc)
    {
        if (!isRunning || !platform)
        {
            spdlog::error("addWindow: the engine did not start.");
            return {};
        }
        if (recordingFrame)
        {
            spdlog::error("addWindow: a frame is being recorded; add windows between frames.");
            return {};
        }
        const PlatformType type = platform->type();
        const bool headless = type == PlatformType::HEADLESS;
        const bool hasNative = desc.native.type != NativeWindowType::NONE;
        if ((type == PlatformType::NATIVE) != hasNative)
        {
            spdlog::error("addWindow: {}", type == PlatformType::NATIVE ? "a renderer on the application's window draws into the application's windows: set WindowDesc::native."
                                                                        : "WindowDesc::native is for a renderer on the application's windows; this one opens its own or has none.");
            return {};
        }
        const bool sized = hasNative ? (desc.native.pixelWidth > 0 || desc.width > 0) && (desc.native.pixelHeight > 0 || desc.height > 0) : desc.width > 0 && desc.height > 0;
        if (!sized)
        {
            spdlog::error("addWindow: a window needs a width and a height (for a native window, its pixel size or WindowDesc's).");
            return {};
        }
        const HandleSlot slot = windowHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::error("addWindow: {} windows are open, the most there can be (MAX_WINDOWS - 1); remove one first.", windowHandles.liveCount());
            return {};
        }
        Window window = platform->createWindow(desc);
        if (!headless && window.native.type == NativeWindowType::NONE)
        {
            windowHandles.release(slot.index, slot.generation);
            return {};
        }
        const WindowHandle handle = {slot.index, slot.generation};
        window.handle = handle;
        windows[slot.index] = window;
        windowFrames[slot.index] = {};
        if (!ctx.createSwapchain(windows[slot.index]) || !createSwapchainResources(handle))
        {
            ctx.destroySwapchain(handle);
            platform->destroyWindow(windows[slot.index]);
            windows[slot.index] = {};
            windowHandles.release(slot.index, slot.generation);
            return {};
        }
        return handle;
    }

    void Renderer::removeWindow(WindowHandle window)
    {
        if (!isLive(window) || window.index == MAIN_WINDOW_INDEX)
        {
            spdlog::error("removeWindow: the window is invalid, removed, or the main window.");
            return;
        }
        if (recordingFrame)
        {
            spdlog::error("removeWindow: a frame is being recorded; remove windows between frames.");
            return;
        }
        ctx.destroySwapchain(window);
        if (platform)
            platform->destroyWindow(windows[window.index]);
        windows[window.index] = {};
        windowFrames[window.index] = {};
        windowHandles.release(window.index, window.generation);
    }

    const Window &Renderer::getWindow(WindowHandle window) const
    {
        static const Window none{};
        if (!window.isValid())
            return windows[MAIN_WINDOW_INDEX];
        return isLive(window) ? windows[window.index] : none;
    }

    bool Renderer::isLive(const WindowHandle &window) const
    {
        return windowHandles.isAlive(window.index, window.generation);
    }

    // An invalid handle names the main window; a removed one, the main window too (its passes are skipped before).
    Renderer::WindowFrame &Renderer::frameOf(const WindowHandle &window)
    {
        return windowFrames[isLive(window) ? window.index : MAIN_WINDOW_INDEX];
    }

    Window &Renderer::windowOf(const WindowHandle &window)
    {
        return windows[isLive(window) ? window.index : MAIN_WINDOW_INDEX];
    }

    void Renderer::pollEvents()
    {
        // A second call in the same frame leaves this frame's input as it is: the window's new events wait for the
        // next frame, but a close request stops the engine at once, so a loop that renders no frames can still end.
        if (inputPolled)
        {
            bool closed = false;
            if (platform)
                platform->pollEvents(windows, MAX_WINDOWS, pendingInputEvents, closed);
            if (closed)
                isRunning = false;
            return;
        }
        inputPolled = true;

        // The first frame has no previous one, and a long stall (a debugger, a window drag) is capped so the camera does not jump.
        const auto now = std::chrono::steady_clock::now();
        frameTime = lastFrameTime == std::chrono::steady_clock::time_point{} ? 0.0f : std::min(std::chrono::duration<float>(now - lastFrameTime).count(), 0.1f);
        deltaTime = config.fixedDeltaTime > 0.0f ? config.fixedDeltaTime : frameTime;
        lastFrameTime = now;

        // The application's events, queued since the last poll, then the platform's. The last frame's stay readable
        // (getInputEvents) until here; both vectors keep their capacity. All go to the UI only while it builds frames,
        // and what the last UI frame took does not reach the camera or the engine's keys.
        inputEvents.swap(pendingInputEvents);
        pendingInputEvents.clear();
        bool closed = false;
        if (platform)
            platform->pollEvents(windows, MAX_WINDOWS, inputEvents, closed);
        bool uiActive = false;
        const UiInputCapture capture = getUiInputCapture();
#if defined(RASM_IMGUI)
        uiActive = ui && ui->active;
#endif
        beginInputFrame(*inputState);
        const Window &mainWindow = windows[MAIN_WINDOW_INDEX];
        const InputArea area = {.width = static_cast<float>(mainWindow.width), .height = static_cast<float>(mainWindow.height)};
        for (const InputEvent &event : inputEvents)
        {
            // The UI and the engine's camera input live in the main window; the others' events are the application's.
            if (event.window.isValid() && event.window.index != MAIN_WINDOW_INDEX)
                continue;
#if defined(RASM_IMGUI)
            if (uiActive)
                processUiEvent(event);
#endif
            applyInputEvent(*inputState, event, area, capture);
        }
        (void)uiActive;

        // Escape closes only a window RASM opened: an application that owns its window decides when to stop.
        if (closed || (inputState->escapes > 0 && platform->type() == PlatformType::SDL))
        {
            if (!closed)
                spdlog::info("Escape key pressed, closing window.");
            isRunning = false;
        }
        if (inputState->statsToggles % 2 == 1)
            config.showStats = !config.showStats;
        // Cycles by distance (-1), LOD 0, 1, ..., MAX_MESH_LODS - 1, by distance.
        for (uint32_t i = 0; i < inputState->forcedLodSteps; ++i)
            config.forcedLod = config.forcedLod + 1 >= static_cast<int32_t>(MAX_MESH_LODS) ? -1 : config.forcedLod + 1;
        if (inputState->dumpGraph)
            graphDumpRequested = true;

        cameraInput = takeCameraInput(inputState->camera, capture, deltaTime);
    }

    void Renderer::addInputEvent(const InputEvent &event)
    {
        pendingInputEvents.push_back(event);
    }

    const InputEvent *Renderer::getInputEvents(uint32_t &count) const
    {
        count = static_cast<uint32_t>(inputEvents.size());
        return inputEvents.data();
    }

    CameraInput Renderer::getCameraInput() const
    {
        return cameraInput;
    }

    void Renderer::setCameraButtons(const CameraButtons &buttons)
    {
        inputState->camera.buttons = buttons;
    }

    void Renderer::setMouseGrab(bool on, WindowHandle window)
    {
        const Window &target = getWindow(window);
        if (platform && target.handle.isValid())
            platform->setMouseGrab(target, on);
    }

    UiInputCapture Renderer::getUiInputCapture() const
    {
#if defined(RASM_IMGUI)
        if (ui && ui->active)
            return readUiInputCapture();
#endif
        return {};
    }

    float Renderer::getDeltaTime() const
    {
        return deltaTime;
    }

    void Renderer::setWindowSize(uint32_t width, uint32_t height, uint32_t pixelWidth, uint32_t pixelHeight)
    {
        setWindowSize(WindowHandle{}, width, height, pixelWidth, pixelHeight);
    }

    void Renderer::setWindowSize(WindowHandle window, uint32_t width, uint32_t height, uint32_t pixelWidth, uint32_t pixelHeight)
    {
        if (platform->type() == PlatformType::SDL)
        {
            spdlog::warn("setWindowSize: the SDL platform reads its window's size itself.");
            return;
        }
        if (window.isValid() && !isLive(window))
        {
            spdlog::error("setWindowSize: the window is invalid or was removed.");
            return;
        }
        // The swapchain follows the pixel size from the next frame (B17); points only place the mouse and the UI.
        Window &target = windowOf(window);
        target.resized = target.resized || pixelWidth != target.pixelWidth || pixelHeight != target.pixelHeight;
        target.width = width;
        target.height = height;
        target.pixelWidth = pixelWidth;
        target.pixelHeight = pixelHeight;
        target.minimized = pixelWidth == 0 || pixelHeight == 0;
    }

    void Renderer::setWindowContentScale(float scale)
    {
        setWindowContentScale(WindowHandle{}, scale);
    }

    void Renderer::setWindowContentScale(WindowHandle window, float scale)
    {
        if (platform->type() == PlatformType::SDL)
        {
            spdlog::warn("setWindowContentScale: the SDL platform reads its display's scale itself.");
            return;
        }
        if (!(scale > 0.0f))
        {
            spdlog::error("setWindowContentScale: the scale must be above 0, not {}.", scale);
            return;
        }
        if (window.isValid() && !isLive(window))
        {
            spdlog::error("setWindowContentScale: the window is invalid or was removed.");
            return;
        }
        windowOf(window).contentScale = scale;
    }

    // The attached loader waits; a renderer without one has nothing to wait for.
    void Renderer::waitForLoads()
    {
        if (loads)
            loads->waitForLoads();
    }

    LoadStats Renderer::getLoadStats() const
    {
        return loads ? loads->getLoadStats() : LoadStats{};
    }

    // Its pixels are on the GPU: an entity that waited for it may be drawn (syncGpuScene checks the waiting ones).
    void Renderer::textureLoaded(TextureHandle texture)
    {
        loadingTextures.erase(texture);
        gpuScene->texturesFinished = true;
    }

    TextureHandle Renderer::createTexture(const TextureDesc &desc)
    {
        return ctx.createTexture(desc);
    }

    BufferHandle Renderer::createBuffer(const BufferDesc &desc)
    {
        return ctx.createBuffer(desc);
    }

    RenderTargetHandle Renderer::createRenderTarget(const RenderTargetDesc &desc)
    {
        return ctx.createRenderTarget(desc);
    }

    void Renderer::destroyBuffer(const BufferHandle &buffer)
    {
        ctx.destroyBuffer(buffer);
    }

    // A material may sample its colors (a view texture), so it loses them first, as with destroyTexture.
    void Renderer::destroyRenderTarget(const RenderTargetHandle &renderTarget)
    {
        if (ctx.isAlive(renderTarget))
        {
            const RenderTarget &target = ctx.getRenderTarget(renderTarget);
            for (const RenderTargetAttachments &attachments : target.attachments)
            {
                for (uint32_t c = 0; c < target.colorCount; ++c)
                    detachTexture(attachments.color[c]);
            }
        }
        ctx.destroyRenderTarget(renderTarget);
    }

    // The slots the target's textures keep may be read by a frame in flight, so a new size waits for those frames: the
    // last values both queues signaled. Nothing else waits, and the old textures go through the release queue.
    bool Renderer::resizeRenderTarget(const RenderTargetHandle &renderTarget, uint32_t width, uint32_t height)
    {
        if (recordingFrame)
        {
            spdlog::error("resizeRenderTarget: a frame is being recorded; resize between frames.");
            return false;
        }
        const RenderTarget &target = ctx.getRenderTarget(renderTarget);
        if (target.colorCount == 0 && target.depthFormat == Format::UNKNOWN)
            return false;
        if (target.width == width && target.height == height)
            return true;
        if (!ctx.waitForFence(frameFence, frameFenceValue) || (asyncCompute && !ctx.waitForFence(computeFence, computeFenceValue)))
        {
            spdlog::error("resizeRenderTarget: the wait for the frames in flight failed.");
            return false;
        }
        return ctx.resizeRenderTarget(renderTarget, width, height);
    }

    RenderGraph Renderer::createRenderGraph()
    {
        RenderGraph graph(this);
        return graph;
    }

    // The swapchain has no depth, so neither has the pipeline.
    PipelineHandle Renderer::createPipeline(const std::string &name, const std::string &shaderSource)
    {
        const Format colorFormats[MAX_COLOR_ATTACHMENTS] = {ctx.getSwapchainImageFormat()};
        return createGraphicsPipeline(name, shaderSource, colorFormats, Format::UNKNOWN, "vertMain", "fragMain", BlendMode::NONE, true);
    }

    // The pipeline draws into targets with the formats of this one: its colors, in order, and its depth. A depth-only
    // target gives a pipeline without colors.
    PipelineHandle Renderer::createPipeline(const std::string &name, const std::string &shaderSource, const RenderTargetHandle &renderTarget)
    {
        const auto &rt = ctx.getRenderTarget(renderTarget);
        // Every render target has a color or a depth format, so an invalid or stale handle is the one with neither.
        if (rt.depthFormat == Format::UNKNOWN && rt.colorCount == 0)
        {
            spdlog::error("Pipeline '{}' needs a valid render target.", name);
            return {};
        }
        return createGraphicsPipeline(name, shaderSource, rt.colorFormats, rt.depthFormat, "vertMain", "fragMain", BlendMode::NONE, true, rt.samples);
    }

    // A pipeline of the scene passes, from a file in the shader folder, for targets with this one's formats and sample
    // count. defines are preprocessor lines put before the source, for the variants of one shader; #line keeps the
    // diagnostics' line numbers those of the file.
    PipelineHandle Renderer::createScenePipeline(const std::string &name, const std::string &shaderFile, const std::string &defines, const RenderTargetHandle &renderTarget,
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

    PipelineHandle Renderer::createGraphicsPipeline(const std::string &name, const std::string &shaderSource, const Format *colorFormats, Format depthFormat, const char *vertEntryPoint,
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
    PipelineHandle Renderer::createMeshScenePipeline(const std::string &name, const std::string &shaderFile, const std::string &defines, const RenderTargetHandle &renderTarget,
                                                     const char *meshEntryPoint, const char *fragmentEntryPoint, BlendMode blendMode, bool depthWrite, bool alphaToCoverage)
    {
        ZoneScoped;

        const auto &rt = ctx.getRenderTarget(renderTarget);
        if (rt.depthFormat == Format::UNKNOWN)
        {
            spdlog::error("Pipeline '{}' needs a valid render target.", name);
            return {};
        }
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

    PipelineHandle Renderer::createComputePipeline(const std::string &name, const std::string &shaderSource, const std::string &entryPoint)
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

    // Recreates each window's swapchain when the window changed size or it is out of date. Returns false when no
    // window can show anything: each is minimized or zero-sized (B5), or its recreation failed. Renderer::render polls and
    // calls it before beginFrame, so its views take a window's new size in the same frame.
    bool Renderer::updateSwapchain()
    {
        bool anyShown = false;
        for (uint32_t i = 0; i < MAX_WINDOWS; ++i)
        {
            Window &window = windows[i];
            WindowFrame &frame = windowFrames[i];
            if (!window.handle.isValid() || window.minimized || window.pixelWidth == 0 || window.pixelHeight == 0)
                continue;
            if (window.resized || frame.outOfDate)
            {
                window.resized = false;
                frame.outOfDate = !recreateSwapchain(window.handle);
                if (frame.outOfDate)
                    continue;
            }
            anyShown = true;
        }
        return anyShown;
    }

    bool Renderer::beginFrame(CameraHandle camera)
    {
        ZoneScoped;

        pollEvents();
        inputPolled = false;
        // An invalid handle is a frame with no camera to move; a dead one is an error.
        const bool controlled = camera.isValid() && (!isLive(camera) || registry.cameras[camera.index].rig.controller.type != CameraController::Type::NONE);
        if (controlled)
            updateCamera(camera, cameraInput);
        // The engine's camera input grabs the mouse while it drags, and lets go of only the grab it took itself, so an
        // application's own grab is left alone, and a camera that stops being moved still gets the pointer back.
        const bool grab = controlled && inputState->camera.dragActive;
        if (grab != engineMouseGrab)
        {
            setMouseGrab(grab);
            engineMouseGrab = grab;
        }
        // The attached loader finishes what is ready, within its budget, before anything binds the bindless set or the
        // geometry pool.
        if (loads)
            loads->finishLoads();
        // No frame is being recorded, so what was destroyed since the last frame waits only for work already submitted.
        // Here, and not after the fence wait only, so frees go on while frames are skipped (a minimized window).
        ctx.tagReleases({.fence = frameFence, .value = frameFenceValue}, {.fence = computeFence, .value = computeFenceValue});
        ctx.collectReleases();

        if (!updateSwapchain())
            return false;

        // Wait for the GPU to finish the previous frame in this slot before its command buffers are reused.
        auto &currentFrame = frameResources[frameCount];
        if (!ctx.waitForFence(frameFence, currentFrame.fenceValue))
            return false;
        if (asyncCompute && !ctx.waitForFence(computeFence, currentFrame.computeFenceValue))
            return false;
        if (readbackComputeWait > 0 && ctx.getFenceValue(frameFence) >= readbackComputeWait)
            readbackComputeWait = 0;
        ctx.collectReleases();
        // Before this frame's endFrame records new picks into the slot, and before syncGpuScene gives its rows away.
        readPicks(static_cast<uint32_t>(frameCount));

        // After the wait: they read what the slot's last frame measured and rewrite the tables it read.
        updateFrameStats();
        if (!ui)
            updateStatsOverlay();
#if defined(RASM_IMGUI)
        if (ui)
            buildUi(camera);
#endif
        syncGpuScene();

        // B4. Each window that can show gets an image; one that can't (minimized, out of date) sits this frame out, and
        // its passes are skipped. A frame with no image at all is skipped.
        bool anyAcquired = false;
        ctx.beginSwapchainFrame();
        for (uint32_t i = 0; i < MAX_WINDOWS; ++i)
        {
            const Window &window = windows[i];
            WindowFrame &frame = windowFrames[i];
            frame.acquired = false;
            frame.rendered = false;
            if (!window.handle.isValid() || window.minimized || window.pixelWidth == 0 || window.pixelHeight == 0 || frame.outOfDate)
                continue;
            const auto status = ctx.acquireNextImage(frame.imageIdx, window.handle);
            if (status == SwapchainStatus::OUT_OF_DATE)
                frame.outOfDate = true;
            // SUBOPTIMAL: the image is acquired and still usable, recreate after this frame. FAILURE: a new swapchain
            // starts from a clean state.
            if (status == SwapchainStatus::SUBOPTIMAL || status == SwapchainStatus::FAILURE)
                frame.outOfDate = true;
            frame.acquired = status == SwapchainStatus::OK || status == SwapchainStatus::SUBOPTIMAL;
            anyAcquired = anyAcquired || frame.acquired;
        }
        if (!anyAcquired)
            return false;

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
        recordingFrame = true;
        return true;
    }

    void Renderer::endFrame()
    {
        ZoneScoped;

        recordingFrame = false;

        // The rest of the frame is graphics work, and the frame's last graphics submit presents.
        if (useQueue(QueueType::GRAPHICS))
        {
            auto &currentFrame = frameResources[frameCount];

            // Present needs the image in PRESENT_SRC, so a window no pass drew is cleared.
            const auto renderTarget = currentRenderTarget;
            const uint32_t face = currentFace;
            for (uint32_t i = 0; i < MAX_WINDOWS; ++i)
            {
                if (!windowFrames[i].acquired || windowFrames[i].rendered)
                    continue;
                resetRenderTarget();
                currentWindow = windows[i].handle;
                beginPass("Clear");
                endPass();
            }
            resetRenderTarget();
            // The UI and the overlay are the main window's.
            if (windowFrames[MAIN_WINDOW_INDEX].acquired)
            {
#if defined(RASM_IMGUI)
                drawUiFallback();
#endif
                if (!ui)
                    drawStatsOverlay();
            }
            setRenderTarget(renderTarget, face);
            recordPicks();
            recordReadbacks();
            // An offscreen swapchain image is never presented; the next acquire of it drops what it holds.
            for (uint32_t i = 0; i < MAX_WINDOWS && platform->type() != PlatformType::HEADLESS; ++i)
            {
                if (windowFrames[i].acquired)
                    ctx.transitionImageLayout(currentFrame.commandBuffer, windowFrames[i].swapchain.imageHandles[windowFrames[i].imageIdx], TextureUsage::PRESENT_SRC);
            }
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
        finishPickSubmit(presenting);
        finishEnvironmentBakes();
        for (uint32_t i = 0; i < MAX_WINDOWS && presenting; ++i)
        {
            if (!windowFrames[i].acquired)
                continue;
            const auto status = ctx.present(windows[i].handle);
            if (status == SwapchainStatus::SUBOPTIMAL || status == SwapchainStatus::OUT_OF_DATE)
                windowFrames[i].outOfDate = true;
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
    bool Renderer::useQueue(QueueType queue)
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
    uint64_t Renderer::openBatchValue(QueueType queue) const
    {
        return batchValues[static_cast<uint32_t>(queue)] + 1;
    }

    // The waiter's open batch starts only once the queue reaches the value. A value of the queue's open batch
    // closes that batch first, so every wait is on a batch closed, and later submitted, before the waiter's.
    void Renderer::waitForQueue(QueueType waiter, QueueType queue, uint64_t value)
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
    void Renderer::waitForQueueWork(QueueType waiter, QueueType queue)
    {
        waitForQueue(waiter, queue, openBatchValue(queue));
    }

    void Renderer::closeBatch(QueueType queue)
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
    bool Renderer::submitBatches()
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

    void Renderer::beginPass(const std::string &name, const AttachmentOps &ops)
    {
        auto currentFrame = frameResources[frameCount];
        frameStats->passTimer = beginGpuTimer(currentFrame.commandBuffer, name);
        WindowFrame &window = frameOf(currentWindow);
        // A window with no image this frame (minimized, out of date, removed): the pass draws nothing.
        if (!currentRenderTarget.isValid() && (!window.acquired || (currentWindow.isValid() && !isLive(currentWindow))))
        {
            ctx.beginEmptyPass();
            return;
        }
        uint32_t width = window.swapchain.width;
        uint32_t height = window.swapchain.height;
        TextureHandle colors[MAX_COLOR_ATTACHMENTS] = {window.swapchain.imageHandles[window.imageIdx]};
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

    void Renderer::endPass()
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
        else if (frameOf(currentWindow).acquired)
        {
            // endFrame moves it to PRESENT_SRC.
            frameOf(currentWindow).rendered = true;
        }
        endGpuTimer(currentFrame.commandBuffer, frameStats->passTimer);
    }

    void Renderer::setRenderTarget(const RenderTargetHandle &renderTarget, uint32_t face)
    {
        this->currentRenderTarget = renderTarget;
        this->currentFace = face;
    }

    void Renderer::resetRenderTarget()
    {
        this->currentRenderTarget = RenderTargetHandle{};
        this->currentFace = 0;
        this->currentWindow = WindowHandle{};
    }

    bool Renderer::recreateSwapchain()
    {
        return recreateSwapchain(WindowHandle{});
    }

    bool Renderer::recreateSwapchain(WindowHandle window)
    {
        if (window.isValid() && !isLive(window))
        {
            spdlog::error("recreateSwapchain: the window is invalid or was removed.");
            return false;
        }
        WindowFrame &frame = frameOf(window);
        auto status = ctx.recreateSwapchain(windowOf(window));
        if (status != SwapchainStatus::OK)
        {
            // A 0x0 surface keeps the old swapchain, while a failure may have destroyed it with its images.
            frame.swapchain.imageHandles = ctx.getSwapchainImages(window);
            return false;
        }
        if (!createSwapchainResources(window))
            return false;
        spdlog::info("Recreated swapchain: {}x{}", frame.swapchain.width, frame.swapchain.height);
        return true;
    }

    // Called at startup, for a new window, and after a successful swapchain recreation, all with the device idle. The
    // targets sized from the swapchain and the cameras' aspect follow the main window.
    bool Renderer::createSwapchainResources(WindowHandle window)
    {
        Swapchain &swapchain = frameOf(window).swapchain;
        swapchain.imageHandles = ctx.getSwapchainImages(window);
        swapchain.imageFormat = ctx.getSwapchainImageFormat(window);

        // Size everything from the swapchain extent. It is in pixels and can differ from the
        // window's logical size on HiDPI displays (B17).
        auto extent = ctx.getSwapchainExtent(window);
        swapchain.width = extent.x;
        swapchain.height = extent.y;

        if (window.isValid() && window.index != MAIN_WINDOW_INDEX)
            return true;
        ctx.resizeRenderTargets();
        updateCameraAspectRatio(static_cast<float>(swapchain.width) / static_cast<float>(swapchain.height));
        return true;
    }

    void Renderer::render(SceneHandle scene, CameraHandle camera)
    {
        // The renderer is made last in the constructor, so an engine that failed to start has none.
        const View view = {.scene = scene, .camera = camera};
        render(&view, 1);
    }

    void Renderer::render(const View *views, uint32_t viewCount)
    {
        if (viewGraph)
            viewGraph->render(views, viewCount);
    }

    void Renderer::setInspection(const Inspection &value)
    {
        if (!inspector)
            spdlog::warn("setInspection: the UI is off (RendererConfig::ui), so nothing is shown.");
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

    const Inspection &Renderer::getInspection() const
    {
        return inspection;
    }

#if !defined(RASM_IMGUI)
    // Built without Dear ImGui: the UI is always off.
    bool Renderer::addUiPass(RenderGraph &, const RenderTargetHandle *, uint32_t)
    {
        return false;
    }

    void Renderer::updateInspectPass(RenderGraph &) {}

    void Renderer::setUiCallback(std::function<void()>)
    {
        spdlog::warn("setUiCallback: RASM was built without Dear ImGui (RASM_IMGUI=OFF), so the callback never runs.");
    }

    uint64_t Renderer::getUiTexture(const TextureHandle &)
    {
        return 0;
    }
#endif

    bool Renderer::running() const
    {
        return isRunning;
    }

    VRAMStats Renderer::queryMemoryStats()
    {
        return ctx.queryVRAMStats();
    }

    const FrameResources &Renderer::getCurrentFrameResources()
    {
        return frameResources[frameCount % MAX_FRAMES_IN_FLIGHT];
    }

    RendererConfig Renderer::getConfig() const
    {
        return config;
    }

    const Window &Renderer::getMainWindow() const
    {
        return windows[MAIN_WINDOW_INDEX];
    }

    RenderContext &Renderer::getRenderContext()
    {
        return ctx;
    }

    DescriptorSetHandle Renderer::getBindlessDescriptorSet()
    {
        return bindlessDescriptorSet;
    }

    uint32_t Renderer::addBindlessTexture(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, SamplerType sampler, uint32_t slot)
    {
        auto txtID = ctx.updateBindlessDescriptorSet(bindlessSet, texture, sampler, slot);
        if (txtID == UINT32_MAX)
            spdlog::error("Failed to add bindless texture.");
        return txtID;
    }

    uint32_t Renderer::getBindlessTextureIndex(const TextureHandle &texture)
    {
        return ctx.getBindlessTextureIndex(texture);
    }

    PoolStats Renderer::getPoolStats() const
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
                .releasesWaiting = ctx.getReleasesWaiting(),
                .points = geometry.points};
    }

    bool Renderer::isLive(const EntityHandle &entity) const
    {
        return registry.entityHandles.isAlive(entity.index, entity.generation);
    }

    bool Renderer::isLive(const MeshHandle &mesh) const
    {
        return registry.meshHandles.isAlive(mesh.index, mesh.generation);
    }

    bool Renderer::isLive(const MaterialHandle &material) const
    {
        return registry.materialHandles.isAlive(material.index, material.generation);
    }

    bool Renderer::isAlive(EntityHandle entity) const
    {
        return isLive(entity);
    }

    bool Renderer::isLive(const CameraHandle &camera) const
    {
        return registry.cameraHandles.isAlive(camera.index, camera.generation);
    }

    bool Renderer::isLive(const LightHandle &light) const
    {
        return registry.lightHandles.isAlive(light.index, light.generation);
    }

    bool Renderer::isLive(const SceneHandle &scene) const
    {
        return registry.sceneHandles.isAlive(scene.index, scene.generation);
    }

    bool Renderer::isLive(const EnvironmentHandle &environment) const
    {
        return registry.environmentHandles.isAlive(environment.index, environment.generation);
    }

    TextureHandle Renderer::getRenderTargetTexture(const RenderTargetHandle &renderTarget, uint32_t attachment)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        if (attachment >= rt.colorCount)
            return {};
        return rt.attachments[frameCount % MAX_FRAMES_IN_FLIGHT].color[attachment];
    }

    TextureHandle Renderer::getRenderTargetDepthTexture(const RenderTargetHandle &renderTarget)
    {
        return ctx.getRenderTarget(renderTarget).attachments[frameCount % MAX_FRAMES_IN_FLIGHT].depth;
    }

    uint32_t Renderer::getRenderTargetStorageIndex(const RenderTargetHandle &renderTarget, uint32_t attachment)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        if (attachment >= rt.colorCount)
            return UINT32_MAX;
        return rt.storageSlots[frameCount % MAX_FRAMES_IN_FLIGHT][attachment];
    }

    TextureHandle Renderer::getPreviousRenderTargetTexture(const RenderTargetHandle &renderTarget, uint32_t attachment)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        if (!rt.history || attachment >= rt.colorCount)
            return {};
        return rt.attachments[(frameCount + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT].color[attachment];
    }

} // namespace rasm
