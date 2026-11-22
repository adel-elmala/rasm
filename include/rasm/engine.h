// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/entity.h"
#include "rasm/scene.h"
#include "rasm/material.h"
#include "rasm/render/renderGraph.h"
#include "rasm/window.h"
#include "rasm/light.h"
#include "rasm/mesh.h"
#include "rasm/handle.h"
#include "rasm/render/renderContext.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinygltf { class Model; }

namespace rasm
{

    class GeometryPool;
    class Renderer;
    class ShaderCompiler;
    struct GpuScene;
    struct GpuLights;
    struct GpuAnimation;
    struct Environments;
    struct SceneDrawList;
    struct FrameStatsCollector;
    struct StatsOverlay;
    struct Ui;
    namespace gfx { struct SceneData; }

    constexpr uint32_t MAX_MATERIALS        = 256;
    constexpr uint32_t MAX_CAMERAS          = 8;
    constexpr uint32_t MAX_GPU_TIMERS       = 64;
    constexpr Format   SCENE_COLOR_FORMAT   = Format::R16G16B16A16_SFLOAT;
    // A scene target's optional second color: each pixel's motion since the last frame, in UV units, for addTaaPass.
    constexpr Format   MOTION_FORMAT        = Format::R16G16_SFLOAT;

    struct GpuTimer
    {
        std::string                 name;
        float                       milliseconds        = 0.0f;
    };

    struct FrameStats
    {
        float                       cpuFrameMs                  = 0.0f;
        float                       gpuFrameMs                  = 0.0f;
        GpuTimer                    gpuTimers[MAX_GPU_TIMERS - 1];
        uint32_t                    gpuTimerCount               = 0;
        uint32_t                    draws                       = 0;
        uint32_t                    drawsVisible                = 0;
        uint64_t                    triangles                   = 0;
        uint64_t                    trianglesVisible            = 0;
        uint64_t                    trianglesVisibleLod0        = 0;
        uint32_t                    drawsPerLod[MAX_MESH_LODS]  = {};
        uint32_t                    lightsGlobal                = 0;    // directional lights and lights without a range, at every pixel
        uint32_t                    lightsVisible               = 0;    // local lights in a view's frustum
        uint32_t                    lightsDropped               = 0;    // lights left out of a full cluster (MAX_CLUSTER_LIGHTS) or screen tile
        VRAMStats                   memory                      = {};
    };

    class Engine
    {
        friend class RenderContext;
        friend class RenderGraph;

    public:
                                    Engine(const EngineConfig& config);
                                    ~Engine();
        SceneHandle                 createScene();
        EntityHandle                createEntity(const std::string& name, MeshHandle mesh = MeshHandle{}, MaterialHandle material = MaterialHandle{}, Transform transform = Transform{});
        CameraHandle                createCamera(CameraProjection projection, Transform transform);
        LightHandle                 createLight(LightType type, glm::vec3 color = glm::vec3(1.0f), glm::vec3 position = glm::vec3(1.0f), glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f), float intensity = 1.0f, float range = 10.0f, float innerConeAngle = 0.0f, float outerConeAngle = SPOT_OUTER_CONE_ANGLE);
        Light                       getLight(LightHandle light) const;
        void                        updateLight(LightHandle light, const Light& values);
        void                        addEntityToScene(SceneHandle scene, EntityHandle entity);
        void                        addCameraToScene(SceneHandle scene, CameraHandle camera);
        void                        addLightToScene(SceneHandle scene, LightHandle light);
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
        MeshHandle                  loadMesh(const std::string& path);
        MeshHandle                  loadGLTF(const std::string& path);
        // format names the slot: R8G8B8A8_SRGB for color (base color, emissive), R8G8B8A8_UNORM for data (normal,
        // metallic-roughness, occlusion). A .ktx2 file gets the best GPU format the device has for that slot.
        TextureHandle               loadTexture(const std::string& path, Format format = Format::R8G8B8A8_SRGB);
        EnvironmentHandle           loadEnvironment(const std::string& path);
        bool                        isLoaded(MeshHandle mesh) const;
        bool                        isLoaded(TextureHandle texture) const;
        bool                        isLoaded(EnvironmentHandle environment) const;
        void                        waitForLoads();
        MaterialHandle              createMaterial(MaterialType type, std::vector<TextureHandle> textures = {}, bool doubleSided = false);
        void                        updateMaterial(MaterialHandle material, const Material& values);
        TextureHandle               createTexture(const TextureDesc& desc);
        BufferHandle                createBuffer(const BufferDesc& desc);
        RenderTargetHandle          createRenderTarget(const RenderTargetDesc& desc);
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
        glm::mat4                   getViewMatrix(CameraHandle camera);
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
        // Returns false when the frame must be skipped (minimized, swapchain out of date); then do not
        // record anything or call endFrame.
        [[nodiscard]] bool          beginFrame(CameraHandle camera);
        // Presents the swapchain image, cleared if no pass rendered to it this frame.
        void                        endFrame();
        void                        beginPass(const std::string& name = "Pass", const AttachmentOps& ops = {});
        void                        endPass();
        void                        render(SceneHandle scene, CameraHandle camera);
        bool                        addScenePasses(RenderGraph& graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget, const SceneShadow* shadows = nullptr, uint32_t shadowCount = 0);
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
        bool                        addToneMapPass(RenderGraph& graph, const RenderTargetHandle& source, CameraHandle camera, const RenderTargetHandle& bloom = {});
        // Draws the frame's Dear ImGui windows onto the swapchain: add it after addToneMapPass. Without it endFrame draws
        // them in a render pass of its own. Returns false when the UI is off (EngineConfig::ui).
        bool                        addUiPass(RenderGraph& graph);
        // Runs every frame between ImGui::NewFrame and ImGui::Render, after the input and before the scene is uploaded,
        // so what it changes shows in the same frame. Needs EngineConfig::ui.
        void                        setUiCallback(std::function<void()> callback);
        // The ImTextureID that shows a sampled texture with ImGui::Image, or 0 while the texture loads. A loaded texture
        // without a bindless slot gets one.
        uint64_t                    getUiTexture(const TextureHandle& texture);
        bool                        running() const;
        bool                        recreateSwapchain();
        void                        setRenderTarget(const RenderTargetHandle& renderTarget, uint32_t face = 0);
        void                        resetRenderTarget();
        DescriptorSetHandle         getBindlessDescriptorSet();
        const FrameResources&       getCurrentFrameResources();
        EngineConfig                getConfig() const;
        std::string                 resolveAssetPath(const std::string& path) const;
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
        Window                      createWindow(uint32_t width, uint32_t height);
        void                        destroyWindow(const Window& window);
        void                        pollEvents(CameraHandle camera);

    protected:
        bool                                                        createSwapchainResources();
        bool                                                        useQueue(QueueType queue);
        uint64_t                                                    openBatchValue(QueueType queue) const;
        void                                                        waitForQueue(QueueType waiter, QueueType queue, uint64_t value);
        void                                                        waitForQueueWork(QueueType waiter, QueueType queue);
        void                                                        closeBatch(QueueType queue);
        bool                                                        submitBatches();

        EngineConfig                                                config                                  = {};
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
        bool                                                        asyncCompute                            = false;
        Swapchain                                                   swapchain                               = {};
        std::unique_ptr<GeometryPool>                               geometryPool                            = {};
        std::unique_ptr<GpuScene>                                   gpuScene                                = {};
        std::unique_ptr<GpuLights>                                  gpuLights                               = {};
        std::unique_ptr<GpuAnimation>                               gpuAnimation                            = {};
        std::unique_ptr<Environments>                               environments                            = {};
        std::unique_ptr<FrameStatsCollector>                        frameStats                              = {};
        std::unique_ptr<StatsOverlay>                               statsOverlay                            = {};
        std::unique_ptr<Ui>                                         ui                                      = {};
        std::unique_ptr<Renderer>                                   renderer                                = {};
        PipelineHandle                                              toneMapPipeline                         = {};
        PipelineHandle                                              msaaResolvePipeline                     = {};
        PipelineHandle                                              taaPipeline                             = {};
        PipelineHandle                                              bloomFirstDownPipeline                  = {};
        PipelineHandle                                              bloomDownPipeline                       = {};
        PipelineHandle                                              bloomUpPipeline                         = {};
        RenderContext                                               ctx;
        std::unique_ptr<ShaderCompiler>                             shaderCompiler                          = {};
        HandleManager                                               handleManager                           = {};
        Window                                                      mainWindow                              = {};
        DescriptorSetLayoutHandle                                   bindlessDescriptorSetLayout             = {};
        DescriptorPoolHandle                                        bindlessDescriptorPool                  = {};
        DescriptorSetHandle                                         bindlessDescriptorSet                   = {};
        MaterialHandle                                              defaultMaterial                         = {}; // used for invalid material handles
        RenderTargetHandle                                          currentRenderTarget                     = {};
        uint32_t                                                    currentFace                             = 0;  // of a cube target
        uint64_t                                                    frameCount                              = 0;
        uint32_t                                                    imageIdx                                = 0;
        float                                                       deltaTime                               = 0.0f; // seconds since the previous frame
        std::chrono::steady_clock::time_point                       lastFrameTime                           = {};
        bool                                                        isRunning                               = false;
        bool                                                        swapchainOutOfDate                      = false;
        bool                                                        swapchainRendered                       = false; // a pass rendered to the swapchain this frame
        bool                                                        graphDumpRequested                      = false; // F3: the next graph that runs logs its dump
        std::string                                                 graphDump                               = {};    // the last one, for the RASM window

    private:
        static constexpr Format     DEPTH_FORMAT    = Format::D32_SFLOAT;

        struct PendingMeshLoad
        {
            MeshHandle                  handle;
            std::string                 path;
            std::future<GLTFRaw>        result;
        };

        struct PendingTextureLoad
        {
            TextureHandle               handle;
            std::string                 path;
            std::future<TextureRaw>     result;
        };

        struct PendingEnvironmentLoad
        {
            EnvironmentHandle               handle;
            std::string                     path;
            std::future<EnvironmentRaw>     result;
        };

        PipelineHandle              createGraphicsPipeline(const std::string& name, const std::string& shaderSource, const Format* colorFormats, Format depthFormat, const char* vertexEntryPoint, const char* fragmentEntryPoint, BlendMode blendMode, bool depthWrite, uint32_t samples = 1, bool alphaToCoverage = false);
        PipelineHandle              createScenePipeline(const std::string& name, const std::string& shaderFile, const std::string& defines, const RenderTargetHandle& renderTarget, const char* vertexEntryPoint, const char* fragmentEntryPoint, BlendMode blendMode, bool depthWrite, bool alphaToCoverage = false);
        MaterialHandle              loadMaterial(const tinygltf::Model& model, int materialIndex, GltfImportCache& cache);
        void                        finishMeshLoad(PendingMeshLoad& load);
        void                        finishTextureLoad(PendingTextureLoad& load);
        bool                        fillTextureLevels(TextureHandle texture, const TextureRaw& raw);
        void                        finishEnvironmentLoad(PendingEnvironmentLoad& load);
        bool                        createEnvironments();
        void                        recordEnvironmentBakes();
        void                        finishEnvironmentBakes();
        void                        drawSky(uint32_t view);
        bool                        bloomPipelinesReady() const;
        bool                        bloomRuns(CameraHandle camera) const;
        bool                        createBloomPipelines();
        void                        finishLoads(bool wait);
        bool                        createGpuScene();
        void                        syncGpuScene();
        void                        queueInstanceRows(EntityHandle entity);
        void                        writeInstanceRows(EntityHandle entity, bool moved = true);
        void                        updateDrawModes(EntityHandle entity);
        void                        refreshEntitiesAfterMaterialChange();
        void                        writeMaterialRow(MaterialHandle material);
        void                        addEntityToDrawList(SceneHandle scene, EntityHandle entity);
        void                        markDrawListsDirty(EntityHandle entity);
        void                        rebuildDrawList(SceneHandle scene);
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
        bool                        addSceneViewPasses(RenderGraph& graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget, uint32_t face, const SceneShadow* shadows, uint32_t shadowCount, bool depthOnly);
        uint32_t                    createSceneView(SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget, uint32_t face, const SceneShadow* shadows, uint32_t shadowCount, bool depthOnly);
        void                        releaseSceneView(uint32_t view);
        void                        prepareSceneView(uint32_t view);
        void                        recordSceneCulling(uint32_t view, uint32_t phase);
        void                        recordHiz(uint32_t view);
        void                        recordBlendSort(uint32_t view);
        void                        drawSceneView(uint32_t view);
        void                        drawTransparent(uint32_t view);
        void                        createFrameStats();
        void                        updateFrameStats();
        bool                        createStatsOverlay();
        void                        updateStatsOverlay();
        void                        drawStatsOverlay();
        bool                        createUi();
        void                        destroyUi();
        void                        buildUi();
        void                        drawUi();
        void                        drawUiFallback();
        void                        drawEngineWindow();

        std::vector<PendingMeshLoad>                                            pendingMeshes;
        std::unordered_map<TextureHandle, PendingTextureLoad, HandleHash>       pendingTextures;
        std::vector<PendingEnvironmentLoad>                                     pendingEnvironments;

        struct Registry
        {
            std::vector<Entity>      entities;
            std::vector<Material>    materials;
            std::vector<Camera>      cameras;
            std::vector<Mesh>        meshes;
            std::vector<Light>       lights;
            std::vector<Scene>       scenes;

            std::unordered_map<std::string, MeshHandle>                 loadedMeshes;
            std::unordered_map<std::string, TextureHandle>              loadedTextures;
            std::unordered_map<TextureHandle, uint32_t, HandleHash>     bindlessTextureIndexMap; // slot in the bindless descriptor set
        };

        Registry registry = {};
    };

}
// clang-format on
