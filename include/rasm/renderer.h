// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/assetLoader.h"
#include "rasm/camera.h"
#include "rasm/cameraInput.h"
#include "rasm/entity.h"
#include "rasm/scene.h"
#include "rasm/material.h"
#include "rasm/render/renderGraph.h"
#include "rasm/window.h"
#include "rasm/input.h"
#include "rasm/light.h"
#include "rasm/mesh.h"
#include "rasm/readback.h"
#include "rasm/tileStreamer.h"
#include "rasm/view.h"
#include "rasm/handle.h"
#include "rasm/render/renderContext.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rasm
{

    class GeometryPool;
    class LoadHooks;
    class Platform;
    struct InputState;
    class ViewGraph;
    class ShaderCompiler;
    struct GpuScene;
    struct GpuLights;
    struct GpuAnimation;
    struct Environments;
    struct SceneDrawList;
    struct FrameStatsCollector;
    struct StatsOverlay;
    struct GLTFRaw;
    struct Ui;
    struct Inspector;
    struct Readbacks;
    struct InspectEntry;
    struct TaaState;
    namespace gfx { struct SceneData; }

    constexpr uint32_t MAX_CAMERAS          = 32;
    constexpr uint32_t MAX_GPU_TIMERS       = 64;
    // Output formats the tone-map pass keeps pipelines for at once: the swapchain's and its view textures'.
    constexpr uint32_t MAX_TONE_MAP_FORMATS = 4;

    struct GpuTimer
    {
        std::string                 name;
        float                       milliseconds        = 0.0f;
    };

    struct FrameStats
    {
        float                       cpuFrameMs                  = 0.0f;
        float                       gpuFrameMs                  = 0.0f;
        float                       uploadGpuMs                 = 0.0f;     // GPU time of the upload copies that finished since the last frame, on their queues
        GpuTimer                    gpuTimers[MAX_GPU_TIMERS - 1];
        uint32_t                    gpuTimerCount               = 0;
        uint32_t                    draws                       = 0;
        uint32_t                    drawsVisible                = 0;
        uint64_t                    triangles                   = 0;
        uint64_t                    trianglesVisible            = 0;
        uint64_t                    trianglesVisibleLod0        = 0;
        uint64_t                    trianglesVisibleLod         = 0;    // the visible draws' triangles at their LOD, before meshlet culling
        uint32_t                    drawsPerLod[MAX_MESH_LODS]  = {};
        uint32_t                    meshletsVisible             = 0;    // meshlets drawn, in every view
        uint32_t                    meshletsCulled              = 0;    // meshlets of visible draws that the frustum, the normal cone or the Hi-Z culled
        uint32_t                    meshletDrawsWhole           = 0;    // draws past a view's RendererConfig::maxViewMeshlets, drawn whole
        uint32_t                    lightsGlobal                = 0;    // directional lights and lights without a range, at every pixel
        uint32_t                    lightsVisible               = 0;    // local lights in a view's frustum
        uint32_t                    lightsDropped               = 0;    // lights left out of a full cluster (MAX_CLUSTER_LIGHTS) or screen tile
        VRAMStats                   memory                      = {};
    };

    // The renderer alone: the scene objects, the GPU scene and the frame, drawn into the application's windows
    // (RendererConfig::nativeWindow, addWindow with WindowDesc::native), or into offscreen images without one. It
    // opens no window, reads no events (the application sends them with addInputEvent) and starts no SDL. Engine is a
    // Renderer that opens its own windows with SDL.
    class Renderer
    {
        friend class RenderGraph;
        friend class ViewGraph;
        friend struct AssetLoader::Impl;
        friend struct TileStreamer::Impl;

    public:
        explicit                    Renderer(const RendererConfig& config);
        virtual                     ~Renderer();
                                    Renderer(const Renderer&) = delete;
        Renderer&                   operator=(const Renderer&) = delete;
        SceneHandle                 createScene();
        EntityHandle                createEntity(const std::string& name, MeshHandle mesh = MeshHandle{}, MaterialHandle material = MaterialHandle{}, Transform transform = Transform{});
        CameraHandle                createCamera(CameraProjection projection, Transform transform);
        LightHandle                 createLight(LightType type, glm::vec3 color = glm::vec3(1.0f), glm::dvec3 position = glm::dvec3(1.0), glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f), float intensity = 1.0f, float range = 10.0f, float innerConeAngle = 0.0f, float outerConeAngle = SPOT_OUTER_CONE_ANGLE);
        Light                       getLight(LightHandle light) const;
        void                        updateLight(LightHandle light, const Light& values);
        void                        addEntityToScene(SceneHandle scene, EntityHandle entity);
        void                        addCameraToScene(SceneHandle scene, CameraHandle camera);
        void                        addLightToScene(SceneHandle scene, LightHandle light);
        void                        removeEntityFromScene(SceneHandle scene, EntityHandle entity);
        void                        removeLightFromScene(SceneHandle scene, LightHandle light);
        // Destroying an object makes its handle invalid at once; what the GPU may still use (geometry, textures, bindless
        // slots) is freed once it has finished the frames in flight, and its rows, ranges and index are reused. What
        // used it falls back: an entity of a destroyed mesh draws nothing, a destroyed material's users take the entity's or
        // the default material, a destroyed environment leaves its scenes without one.
        void                        destroyEntity(EntityHandle entity);
        // Also destroys the materials and textures its glTF import made, which belong to it.
        void                        destroyMesh(MeshHandle mesh);
        void                        destroyMaterial(MaterialHandle material);
        void                        destroyLight(LightHandle light);
        void                        destroyEnvironment(EnvironmentHandle environment);
        // Refused while a render graph draws a view of it: destroy the graph first. Renderer::render lets go by itself.
        void                        destroyCamera(CameraHandle camera);
        void                        destroyScene(SceneHandle scene);
        PoolStats                   getPoolStats() const;
        // The attached AssetLoader's (zero without one).
        LoadStats                   getLoadStats() const;
        void                        setSceneEnvironment(SceneHandle scene, const SceneEnvironment& environment);
        void                        setAnimationTime(double seconds);
        void                        playAnimation(EntityHandle entity, const AnimationPlay& play);
        void                        stopAnimation(EntityHandle entity);
        std::vector<AnimationClip>  getAnimations(MeshHandle mesh) const;
        int32_t                     findAnimation(MeshHandle mesh, const std::string& name) const;
        void                        setMorphWeights(EntityHandle entity, const float* weights, uint32_t count);
        void                        clearMorphWeights(EntityHandle entity);
        std::vector<std::string>    getMorphTargetNames(MeshHandle mesh) const;
        MeshHandle                  createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<uint32_t> colors = {});
        // A mesh, texture or environment from an AssetLoader is in: its data is on the GPU (an environment: baked).
        // Made with createMesh or createTexture, it is in at once.
        bool                        isLoaded(MeshHandle mesh) const;
        bool                        isLoaded(TextureHandle texture) const;
        bool                        isLoaded(EnvironmentHandle environment) const;
        // Blocks until every load of the attached AssetLoader is in; returns at once without one.
        void                        waitForLoads();
        MaterialHandle              createMaterial(MaterialType type, std::vector<TextureHandle> textures = {}, bool doubleSided = false);
        void                        updateMaterial(MaterialHandle material, const Material& values);
        TextureHandle               createTexture(const TextureDesc& desc);
        BufferHandle                createBuffer(const BufferDesc& desc);
        RenderTargetHandle          createRenderTarget(const RenderTargetDesc& desc);
        // The handle is invalid at once; the GPU memory and the bindless slot are freed once the GPU has finished every
        // frame that may use them, so the call never waits. Materials that use the texture lose it (that slot samples
        // nothing), and a load that has not finished is dropped. For textures from loadTexture or createTexture.
        void                        destroyTexture(const TextureHandle& texture);
        void                        destroyBuffer(const BufferHandle& buffer);
        void                        destroyRenderTarget(const RenderTargetHandle& renderTarget);
        // A fixed-size target (no swapchainScale, not a cube) at a new size, for a view whose window part or texture
        // changed. Its texture handles and bindless slots stay, so materials and UI images that show it keep working;
        // its contents do not. A new size waits once for the frames in flight, as a window resize does; the same size
        // costs nothing. Not while a frame records (between beginFrame and endFrame).
        bool                        resizeRenderTarget(const RenderTargetHandle& renderTarget, uint32_t width, uint32_t height);
        RenderGraph                 createRenderGraph();
        // Compiles Slang source with vertMain and fragMain entry points. The pipeline has no vertex input and
        // renders to the swapchain's color format with D32_SFLOAT depth (reversed-Z, compare GREATER_OR_EQUAL), and
        // returns invalid on failure.
        // The name is a label, so two pipelines can share one.
        PipelineHandle              createPipeline(const std::string& name, const std::string& shaderSource);
        PipelineHandle              createPipeline(const std::string& name, const std::string& shaderSource, const RenderTargetHandle& renderTarget);
        PipelineHandle              createComputePipeline(const std::string& name, const std::string& shaderSource, const std::string& entryPoint = "compMain");
        const Material&             getMaterial(const MaterialHandle& handle);
        Transform                   getTransform(EntityHandle entity);
        Transform                   getCameraTransform(CameraHandle camera);
        CameraProjection            getCameraProjection(CameraHandle camera);
        CameraController            getCameraController(CameraHandle camera);
        float                       getCameraExposure(CameraHandle camera);
        float                       getCameraBloomStrength(CameraHandle camera);
        glm::mat4                   getProjectionMatrix(CameraHandle camera);
        // The camera's projection fitted to another aspect ratio. The camera itself does not change.
        glm::mat4                   getProjectionMatrix(CameraHandle camera, float aspect);
        glm::dmat4                  getViewMatrix(CameraHandle camera);
        void                        updateCameraAspectRatio(float aspect);
        void                        updateCamera(CameraHandle camera, const CameraInput& input);
        void                        updateTransform(EntityHandle entity, Transform transform);
        void                        setCameraTransform(CameraHandle camera, Transform transform);
        void                        setCameraProjection(CameraHandle camera, CameraProjection projection);
        void                        setCameraController(CameraHandle camera, CameraController controller);
        void                        setCameraExposure(CameraHandle camera, float exposure);
        // The share of bloom in the tone-mapped image, 0 to 1: at 0 the bloom passes dispatch nothing.
        void                        setCameraBloomStrength(CameraHandle camera, float strength);
        // The camera's next frame starts TAA over, without the frames before: call it on a camera cut.
        void                        resetCameraHistory(CameraHandle camera);
        // Polls unless pollEvents ran, then moves the camera with its controller unless it is NONE or invalid. Returns
        // false when the frame must be skipped (minimized, swapchain out of date); then do not record anything or call endFrame.
        [[nodiscard]] bool          beginFrame(CameraHandle camera);
        // Presents the swapchain image, cleared if no pass rendered to it this frame.
        void                        endFrame();
        void                        beginPass(const std::string& name = "Pass", const AttachmentOps& ops = {});
        void                        endPass();
        void                        render(SceneHandle scene, CameraHandle camera);
        // One frame of several views (split screen, a minimap over the scene, view textures for materials or the UI). A
        // view keeps its targets, culling and TAA history while its camera and output stay in the list, wherever it
        // is in the list. Views draw in list order: a later rect goes over an earlier one, and a view samples this
        // frame's image of the view textures listed before it. The first view in the window (no output target) takes
        // the built-in controller (CameraController::Type::NONE turns it off).
        void                        render(const View* views, uint32_t viewCount);
        // sampled: view textures (color targets other passes draw this frame) that the scene's materials sample. The
        // draws run after the passes that draw them, so they see this frame's image; without it a material sees this
        // frame's or the last frame's, whichever the graph's order gives.
        bool                        addScenePasses(RenderGraph& graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget, const SceneShadow* shadows = nullptr, uint32_t shadowCount = 0,
                                                   const RenderTargetHandle* sampled = nullptr, uint32_t sampledCount = 0);
        SceneShadow                 addShadowPasses(RenderGraph& graph, SceneHandle scene, CameraHandle camera, const ShadowDesc& desc);
        // Resolves a multisampled target's color 0 into a new single-sample target (weighted by brightness, so bright
        // edges stay smooth), which addBloomPasses and addToneMapPass read. Returns invalid on failure.
        RenderTargetHandle          addMsaaResolvePass(RenderGraph& graph, const RenderTargetHandle& source, CameraHandle camera);
        // Temporal anti-aliasing of a scene target with a MOTION_FORMAT second color, which addScenePasses drew in this
        // graph: its view is jittered from now on, and the pass blends each frame into a history target, which it
        // returns for addBloomPasses and addToneMapPass. Returns invalid on failure.
        RenderTargetHandle          addTaaPass(RenderGraph& graph, const RenderTargetHandle& source, CameraHandle camera);
        // Returns the bloom target for addToneMapPass, or invalid on failure.
        RenderTargetHandle          addBloomPasses(RenderGraph& graph, const RenderTargetHandle& source, CameraHandle camera);
        // Maps the HDR source to display colors into the output: a rect of the swapchain, or of a view texture (one color,
        // no depth). Passes that draw one output run in the order they were added, so a later rect goes over an earlier one.
        bool                        addToneMapPass(RenderGraph& graph, const RenderTargetHandle& source, CameraHandle camera, const RenderTargetHandle& bloom = {}, const ViewOutput& output = {});
        // Draws the frame's Dear ImGui windows onto the swapchain: add it after addToneMapPass. Without it endFrame draws
        // them in a render pass of its own. Returns false when the UI is off (RendererConfig::ui). shown: view textures the
        // UI shows (ImGui::Image), so it draws after the passes that draw them this frame.
        bool                        addUiPass(RenderGraph& graph, const RenderTargetHandle* shown = nullptr, uint32_t shownCount = 0);
        // Runs every frame between ImGui::NewFrame and ImGui::Render, after the input and before the scene is uploaded,
        // so what it changes shows in the same frame. Needs RendererConfig::ui.
        void                        setUiCallback(std::function<void()> callback);
        // The ImTextureID that shows a sampled texture with ImGui::Image, or 0 while the texture loads. A loaded texture
        // without a bindless slot gets one.
        uint64_t                    getUiTexture(const TextureHandle& texture);
        void                        setDebugView(DebugView view);
        void                        setCullingFrozen(bool frozen);
        void                        setMeshletPath(MeshletPath path);
        MeshletPath                 getMeshletPath() const;
        // What the RASM window's Buffers section shows, as picking it there does (RendererConfig::ui). The pass that
        // copies it runs only while that section is open.
        void                        setInspection(const Inspection& inspection);
        const Inspection&           getInspection() const;
        // Copies the image this frame shows (the swapchain's, with the UI) to the CPU at the end of the frame. Call it
        // before or during the frame you want: the copy goes into the next endFrame. Needs a headless renderer or a
        // surface that allows copies; returns invalid otherwise.
        ReadbackHandle              readFrame(ReadbackFormat format = ReadbackFormat::RGBA8_SRGB);
        // The same for another window. A window with no image in the frame that would copy it (minimized) is copied in a
        // later frame.
        ReadbackHandle              readFrame(WindowHandle window, ReadbackFormat format = ReadbackFormat::RGBA8_SRGB);
        // The same for one layer and mip of a readable texture (TextureDesc::readable), such as a readable render target's
        // color or depth for this frame slot (getRenderTargetTexture). A depth reads only as RGBA32_FLOAT.
        ReadbackHandle              readTexture(const TextureHandle& texture, ReadbackFormat format = ReadbackFormat::RGBA8_SRGB, uint32_t layer = 0, uint32_t mip = 0);
        // True once takeReadback would not wait: the GPU finished the frame that copied it, or the copy failed. With
        // MAX_FRAMES_IN_FLIGHT frames that is usually two frames after the request; poll it to avoid a stall.
        bool                        isReadbackReady(ReadbackHandle readback);
        // Converts the copy into image and frees it. Blocks until the GPU has finished the frame that copied it when it
        // is not ready yet. Returns false when the copy failed, or no frame has copied it yet (then it stays pending).
        bool                        takeReadback(ReadbackHandle readback, ReadbackImage& image);
        bool                        running() const;
        bool                        recreateSwapchain();
        bool                        recreateSwapchain(WindowHandle window);
        void                        setRenderTarget(const RenderTargetHandle& renderTarget, uint32_t face = 0);
        void                        resetRenderTarget();
        DescriptorSetHandle         getBindlessDescriptorSet();
        const FrameResources&       getCurrentFrameResources();
        RendererConfig              getConfig() const;
        const Window&               getMainWindow() const;
        RenderContext&              getRenderContext();
        MeshSize                    getMeshByteSize(const MeshHandle& handle) const;
        VRAMStats                   queryMemoryStats();
        // The first call turns on measuring, so the stats fill in a few frames later.
        const FrameStats&           getFrameStats() const;
        // Times the GPU work recorded between the two calls into the frame's command buffer, for getFrameStats
        // and the stats overlay. Returns UINT32_MAX, which endGpuTimer ignores, when nothing is measured.
        uint32_t                    beginGpuTimer(const CommandBufferHandle& commandBuffer, const std::string& name);
        void                        endGpuTimer(const CommandBufferHandle& commandBuffer, uint32_t timer);
        uint32_t                    addBindlessTexture(const DescriptorSetHandle& bindlessSet, const TextureHandle& texture, SamplerType sampler = SamplerType::LINEAR_REPEAT, uint32_t slot = MAX_BINDLESS_TEXTURES);
        uint32_t                    getBindlessTextureIndex(const TextureHandle& texture);
        // The render target's color texture for the current frame slot, so fetch it every frame.
        TextureHandle               getRenderTargetTexture(const RenderTargetHandle& renderTarget, uint32_t attachment = 0);
        TextureHandle               getPreviousRenderTargetTexture(const RenderTargetHandle& renderTarget, uint32_t attachment = 0);
        TextureHandle               getRenderTargetDepthTexture(const RenderTargetHandle& renderTarget);
        // Slot of binding 1 of the bindless set that holds a storage target's color, the same for the target's life,
        // or UINT32_MAX for a target that is not storage.
        uint32_t                    getRenderTargetStorageIndex(const RenderTargetHandle& renderTarget, uint32_t attachment = 0);

        // window related
        // Another window to draw into (a view's ViewOutput::window, or a pass's writeSwapchain(window)), with a swapchain
        // of its own: an SDL window the SDL platform opens, the application's window (NATIVE, WindowDesc::native), or an
        // offscreen image (HEADLESS). At most MAX_WINDOWS - 1 windows, the main one included. Returns invalid on failure.
        WindowHandle                addWindow(const WindowDesc& desc);
        // Waits for the device to be idle, then destroys the window's swapchain (and the SDL window). Views and passes
        // that name it draw nothing. Not the main window, and not while a frame records.
        void                        removeWindow(WindowHandle window);
        // The window's state, or the main window's for an invalid handle; an empty Window for a removed one.
        const Window&               getWindow(WindowHandle window) const;
        // The frame's input. Only the first call after a beginFrame works: call it before render to read this frame's.
        void                        pollEvents();
        // Input from an application that runs the event loop (its own window, or a script of a headless renderer):
        // queued for the next pollEvents, like the SDL platform's own events.
        void                        addInputEvent(const InputEvent& event);
        // Valid until the next frame's pollEvents.
        const InputEvent*           getInputEvents(uint32_t& count) const;
        CameraInput                 getCameraInput() const;
        UiInputCapture              getUiInputCapture() const;
        float                       getDeltaTime() const;
        // The main window's size in points and in pixels, for NATIVE and HEADLESS; the SDL platform reads its own. A
        // size of 0 skips frames, as a minimized window does.
        void                        setWindowSize(uint32_t width, uint32_t height, uint32_t pixelWidth, uint32_t pixelHeight);
        void                        setWindowSize(WindowHandle window, uint32_t width, uint32_t height, uint32_t pixelWidth, uint32_t pixelHeight);
        // The display's scale setting (1.5 at 150 %), which sizes the UI; for NATIVE when the window moves to another display.
        void                        setWindowContentScale(float scale);
        void                        setWindowContentScale(WindowHandle window, float scale);

    protected:
        // The windows come from platform: Engine gives the SDL one, the public constructor the native or headless one.
                                                                    Renderer(const RendererConfig& config, std::unique_ptr<Platform> platform);

        // A window's swapchain in the frame being made.
        struct WindowFrame
        {
            Swapchain                                               swapchain                               = {};
            uint32_t                                                imageIdx                                = 0;
            bool                                                    acquired                                = false; // the frame has an image of it
            bool                                                    rendered                                = false; // a pass drew it this frame
            bool                                                    outOfDate                               = false; // recreate it before the next acquire
        };

        bool                                                        createSwapchainResources(WindowHandle window);
        bool                                                        updateSwapchain();
        bool                                                        isLive(const WindowHandle& window) const;
        WindowFrame&                                                frameOf(const WindowHandle& window);
        Window&                                                     windowOf(const WindowHandle& window);
        bool                                                        useQueue(QueueType queue);
        uint64_t                                                    openBatchValue(QueueType queue) const;
        void                                                        waitForQueue(QueueType waiter, QueueType queue, uint64_t value);
        void                                                        waitForQueueWork(QueueType waiter, QueueType queue);
        void                                                        closeBatch(QueueType queue);
        bool                                                        submitBatches();

        RendererConfig                                              config                                  = {};
        FrameResources                                              frameResources[MAX_FRAMES_IN_FLIGHT]    = {}; // Double buffering
        FenceHandle                                                 frameFence                              = {};
        uint64_t                                                    frameFenceValue                         = 0;  // last value a graphics submit signaled
        FenceHandle                                                 computeFence                            = {};
        uint64_t                                                    computeFenceValue                       = 0;
        struct ClosedBatch
        {
            QueueType                                               queue                                   = QueueType::GRAPHICS;
            CommandBufferHandle                                     commandBuffer;
            uint64_t                                                waits[QUEUE_COUNT]                      = {};
            uint64_t                                                value                                   = 0;
        };

        std::vector<CommandBufferHandle>                            batchCommandBuffers[MAX_FRAMES_IN_FLIGHT][QUEUE_COUNT];
        uint32_t                                                    batchesUsed[QUEUE_COUNT]                = {};
        bool                                                        batchOpen[QUEUE_COUNT]                  = {};
        uint64_t                                                    batchWaits[QUEUE_COUNT][QUEUE_COUNT]    = {};
        uint64_t                                                    batchValues[QUEUE_COUNT]                = {};
        std::vector<ClosedBatch>                                    closedBatches;
        QueueType                                                   recordingQueue                          = QueueType::GRAPHICS;
        bool                                                        recordingFailed                         = false; // the frame is dropped at endFrame
        bool                                                        recordingFrame                          = false; // between a beginFrame that returned true and its endFrame
        bool                                                        asyncCompute                            = false;
        uint64_t                                                    readbackComputeWait                     = 0;  // graphics value of the last frame with readback copies, which compute batches wait for
        std::unique_ptr<GeometryPool>                               geometryPool                            = {};
        std::unique_ptr<GpuScene>                                   gpuScene                                = {};
        std::unique_ptr<GpuLights>                                  gpuLights                               = {};
        std::unique_ptr<GpuAnimation>                               gpuAnimation                            = {};
        std::unique_ptr<Environments>                               environments                            = {};
        std::unique_ptr<FrameStatsCollector>                        frameStats                              = {};
        std::unique_ptr<StatsOverlay>                               statsOverlay                            = {};
        std::unique_ptr<Ui>                                         ui                                      = {};
        std::unique_ptr<ViewGraph>                                  viewGraph                               = {};
        struct ToneMapPipelines
        {
            Format                                                  format                                  = Format::UNKNOWN;
            PipelineHandle                                          toneMap                                 = {};
            PipelineHandle                                          debugDisplay                            = {};
            PipelineHandle                                          debugHeat                               = {};
        };
        ToneMapPipelines                                            toneMapPipelines[MAX_TONE_MAP_FORMATS]  = {}; // by output format, made the first time a pass draws one
        PipelineHandle                                              msaaResolvePipeline                     = {};
        PipelineHandle                                              taaPipeline                             = {};
        PipelineHandle                                              bloomFirstDownPipeline                  = {};
        PipelineHandle                                              bloomDownPipeline                       = {};
        PipelineHandle                                              bloomUpPipeline                         = {};
        RenderContext                                               ctx;
        std::unique_ptr<ShaderCompiler>                             shaderCompiler                          = {};
        std::unique_ptr<Platform>                                   platform                                = {}; // its windows and events: SDL, the application's windows, or none
        Window                                                      windows[MAX_WINDOWS]                    = {}; // by handle index; MAIN_WINDOW_INDEX is the main window
        WindowFrame                                                 windowFrames[MAX_WINDOWS]               = {};
        HandlePool                                                  windowHandles                           = HandlePool(MAX_WINDOWS);
        WindowHandle                                                currentWindow                           = {}; // the window a swapchain pass draws, set like currentRenderTarget
        std::unique_ptr<InputState>                                 inputState                              = {};
        std::vector<InputEvent>                                     inputEvents                             = {}; // the last pollEvents', until the next one; keeps its capacity
        std::vector<InputEvent>                                     pendingInputEvents                      = {}; // added since the last pollEvents; keeps its capacity
        CameraInput                                                 cameraInput                             = {}; // the main window's, from the last pollEvents
        bool                                                        inputPolled                             = false; // pollEvents ran since the last beginFrame
        DescriptorSetLayoutHandle                                   bindlessDescriptorSetLayout             = {};
        DescriptorSetHandle                                         bindlessDescriptorSet                   = {};
        MaterialHandle                                              defaultMaterial                         = {}; // used for invalid material handles
        RenderTargetHandle                                          currentRenderTarget                     = {};
        uint32_t                                                    currentFace                             = 0;  // of a cube target
        uint64_t                                                    frameCount                              = 0;
        float                                                       deltaTime                               = 0.0f; // the frame's step: RendererConfig::fixedDeltaTime, else frameTime
        float                                                       frameTime                               = 0.0f; // seconds since the previous frame
        std::chrono::steady_clock::time_point                       lastFrameTime                           = {};
        bool                                                        isRunning                               = false;
        bool                                                        graphDumpRequested                      = false; // F3: the next graph that runs logs its dump
        std::string                                                 graphDump                               = {};    // the last one, for the RASM window
        DebugView                                                   debugView                               = DebugView::NONE;
        bool                                                        cullingFrozen                           = false;
        Inspection                                                  inspection                              = {};
        std::unique_ptr<Inspector>                                  inspector                               = {};
        std::unique_ptr<Readbacks>                                  readbacks                               = {};

    private:
        static constexpr Format     DEPTH_FORMAT    = Format::D32_SFLOAT;

        PipelineHandle              createGraphicsPipeline(const std::string& name, const std::string& shaderSource, const Format* colorFormats, Format depthFormat, const char* vertexEntryPoint, const char* fragmentEntryPoint, BlendMode blendMode, bool depthWrite, uint32_t samples = 1, bool alphaToCoverage = false, bool depthTest = true);
        PipelineHandle              createMeshScenePipeline(const std::string& name, const std::string& shaderFile, const std::string& defines, const RenderTargetHandle& renderTarget, const char* meshEntryPoint, const char* fragmentEntryPoint, BlendMode blendMode, bool depthWrite, bool alphaToCoverage = false);
        PipelineHandle              createScenePipeline(const std::string& name, const std::string& shaderFile, const std::string& defines, const RenderTargetHandle& renderTarget, const char* vertexEntryPoint, const char* fragmentEntryPoint, BlendMode blendMode, bool depthWrite, bool alphaToCoverage = false, bool depthTest = true);
        EnvironmentHandle           createEnvironment(const std::string& name);
        bool                        setEnvironmentImage(EnvironmentHandle environment, const std::string& name, const EnvironmentRaw& raw);
        void                        textureLoaded(TextureHandle texture);
        bool                        createEnvironments();
        void                        recordEnvironmentBakes();
        void                        finishEnvironmentBakes();
        void                        drawSky(uint32_t view);
        PipelineHandle              getToneMapPipeline(Format format, DebugView shown);
        RenderTargetHandle          addTaaPass(RenderGraph& graph, const RenderTargetHandle& source, CameraHandle camera, RenderTargetHandle history, std::shared_ptr<TaaState> state);
        bool                        bloomPipelinesReady() const;
        bool                        bloomRuns(CameraHandle camera) const;
        bool                        createBloomPipelines();
        bool                        createGpuScene();
        void                        syncGpuScene();
        void                        queueInstanceRows(EntityHandle entity);
        void                        writeInstanceRows(EntityHandle entity, bool moved = true);
        void                        updateDrawModes(EntityHandle entity);
        void                        refreshEntitiesAfterMaterialChange();
        void                        writeMaterialRow(MaterialHandle material);
        bool                        addEntityToDrawList(SceneHandle scene, EntityHandle entity);
        void                        markDrawListsDirty(EntityHandle entity);
        void                        rebuildDrawList(SceneHandle scene);
        void                        compactSceneEntities(SceneHandle scene);
        void                        uploadDrawList(SceneDrawList& drawList);
        bool                        createGpuLights();
        void                        writeLightRow(LightHandle light);
        void                        markLightListsDirty(LightHandle light);
        void                        syncGpuLights();
        void                        createViewClusters(uint32_t view);
        void                        releaseViewClusters(uint32_t view);
        void                        writeSceneLights(uint32_t view, SceneHandle scene, gfx::SceneData& sceneData);
        void                        recordLightClusters(uint32_t view, uint64_t sceneDataAddress);
        bool                        createGpuAnimation();
        uint64_t                    getMeshAnimationBytes(const GLTFRaw& raw) const;
        uint64_t                    getAnimationPoolUsed() const;
        bool                        uploadMeshAnimation(MeshHandle mesh, const GLTFRaw& raw);
        void                        startPendingAnimation(EntityHandle entity);
        void                        startAnimation(EntityHandle entity, const AnimationPlay& play, uint32_t firstSync);
        void                        poseAtRest(EntityHandle entity, uint32_t firstSync);
        bool                        takeAnimationRow(EntityHandle entity, uint32_t firstSync);
        bool                        growAnimationBuffers(uint32_t transformCount, uint32_t poseCount, uint32_t jointCount, uint32_t skinnedCount, uint32_t firstSync);
        int32_t                     getAnimationRow(EntityHandle entity, uint32_t subMesh) const;
        bool                        animationCanMirror(MeshHandle mesh, uint32_t subMesh) const;
        void                        writeAnimationRow(EntityHandle entity);
        void                        writeMorphWeightRows(EntityHandle entity);
        void                        syncGpuAnimation();
        void                        addAnimationPass(RenderGraph& graph);
        void                        recordAnimation();
        void                        recordSkinning();
        bool                        addSceneViewPasses(RenderGraph& graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget, uint32_t face, const SceneShadow* shadows, uint32_t shadowCount, bool depthOnly, const RenderTargetHandle* sampled, uint32_t sampledCount);
        uint32_t                    createSceneView(SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget, uint32_t face, const SceneShadow* shadows, uint32_t shadowCount, bool depthOnly);
        void                        releaseSceneView(uint32_t view);
        void                        prepareSceneView(uint32_t view);
        void                        recordSceneCulling(uint32_t view, uint32_t phase);
        void                        recordMeshletCulling(uint32_t view, uint32_t phase);
        bool                        createMeshletPipelines(uint32_t view, bool opaque, bool mask);
        void                        recordHiz(uint32_t view);
        void                        recordBlendSort(uint32_t view);
        void                        drawSceneView(uint32_t view, uint32_t phase);
        void                        drawTransparent(uint32_t view);
        void                        drawCullingGhosts(uint32_t view);
        void                        drawFrozenFrustums(CameraHandle camera);
        DebugView                   getDrawnDebugView(CameraHandle camera) const;
        void                        createFrameStats();
        void                        updateFrameStats();
        bool                        createStatsOverlay();
        void                        updateStatsOverlay();
        void                        drawStatsOverlay();
        bool                        createUi();
        void                        destroyUi();
        void                        buildUi(CameraHandle camera);
        void                        drawUi();
        void                        drawUiFallback();
        void                        drawEngineWindow();
        void                        updateInspectPass(RenderGraph& graph);
        void                        recordInspect(const InspectEntry& entry);
        void                        drawBuffersSection();
        void                        drawTablesSection();
        void                        recordReadbacks();
        void                        finishReadbackSubmit(bool submitted);

        LoadHooks*                                                              loads               = nullptr;  // the attached AssetLoader's
        std::unordered_set<TextureHandle, HandleHash>                           loadingTextures;    // made by a loader, not filled yet: isLoaded false

        bool                        isLive(const EntityHandle& entity) const;
        bool                        isLive(const MeshHandle& mesh) const;
        bool                        isLive(const MaterialHandle& material) const;
        bool                        isLive(const CameraHandle& camera) const;
        bool                        isLive(const LightHandle& light) const;
        bool                        isLive(const SceneHandle& scene) const;
        bool                        isLive(const EnvironmentHandle& environment) const;
        void                        releaseEntityRows(EntityHandle entity);
        void                        releaseEntityAnimation(EntityHandle entity);
        void                        releaseMeshAnimation(MeshHandle mesh);
        void                        detachMaterial(MaterialHandle material);
        void                        detachTexture(const TextureHandle& texture);
        bool                        viewHolds(SceneHandle scene, CameraHandle camera, const char* caller) const;

        // Each object vector is indexed by handle index and holds a slot for every index its pool handed out.
        struct Registry
        {
            HandlePool               entityHandles          = HandlePool(MAX_ENTITIES);
            HandlePool               meshHandles;
            HandlePool               materialHandles;                                       // RendererConfig::maxMaterials + 1, set by the constructor
            HandlePool               cameraHandles          = HandlePool(MAX_CAMERAS);
            HandlePool               lightHandles;                                          // RendererConfig::maxLights + 1, set by the constructor
            HandlePool               sceneHandles           = HandlePool(MAX_SCENES);
            HandlePool               environmentHandles;
            HandlePool               readbackHandles;
            std::vector<Entity>      entities;
            std::vector<Material>    materials;
            std::vector<Camera>      cameras;
            std::vector<Mesh>        meshes;
            std::vector<Light>       lights;
            std::vector<Scene>       scenes;

            std::vector<uint32_t>                                       materialUsers;      // by material index: entities made with it
            std::vector<MeshHandle>                                     materialOwners;     // by material index: the mesh whose glTF import made it
        };

        Registry registry = {};
    };

}
// clang-format on
