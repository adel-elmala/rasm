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
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinygltf { class Model; }

namespace rasm
{

    class GeometryPool;
    class ShaderCompiler;
    struct GpuScene;
    struct SceneDrawList;
    struct FrameStatsCollector;
    struct StatsOverlay;

    constexpr uint32_t MAX_MATERIALS    = 256;
    constexpr uint32_t MAX_CAMERAS      = 8;
    constexpr uint32_t MAX_GPU_TIMERS   = 16;

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
        VRAMStats                   memory                      = {};
    };

    class Engine
    {
        friend class RenderContext;

    public:
                                    Engine(const EngineConfig& config);
                                    ~Engine();
        SceneHandle                 createScene();
        EntityHandle                createEntity(const std::string& name, MeshHandle mesh = MeshHandle{}, MaterialHandle material = MaterialHandle{}, Transform transform = Transform{});
        CameraHandle                createCamera(CameraProjection projection, Transform transform);
        LightHandle                 createLight(LightType type, glm::vec3 color = glm::vec3(1.0f), glm::vec3 position = glm::vec3(1.0f), glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f), float intensity = 1.0f, float range = 10.0f);
        void                        addEntityToScene(SceneHandle scene, EntityHandle entity);
        void                        addCameraToScene(SceneHandle scene, CameraHandle camera);
        void                        addLightToScene(SceneHandle scene, LightHandle light);
        MeshHandle                  createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
        MeshHandle                  loadMesh(const std::string& path);
        MeshHandle                  loadGLTF(const std::string& path);
        // Color images default to sRGB; pass R8G8B8A8_UNORM for normal/MR/AO data.
        TextureHandle               loadTexture(const std::string& path, Format format = Format::R8G8B8A8_SRGB);
        bool                        isLoaded(MeshHandle mesh) const;
        bool                        isLoaded(TextureHandle texture) const;
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
        PipelineHandle              createComputePipeline(const std::string& name, const std::string& shaderSource, const std::string& entryPoint = "compMain");
        const Material&             getMaterial(const MaterialHandle& handle);
        Transform                   getTransform(EntityHandle entity);
        Transform                   getCameraTransform(CameraHandle camera);
        CameraProjection            getCameraProjection(CameraHandle camera);
        CameraController            getCameraController(CameraHandle camera);
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
        // Returns false when the frame must be skipped (minimized, swapchain out of date); then do not
        // record anything or call endFrame.
        [[nodiscard]] bool          beginFrame(CameraHandle camera);
        // Presents the swapchain image, cleared if no pass rendered to it this frame.
        void                        endFrame();
        void                        beginPass(const std::string& name = "Pass", const AttachmentOps& ops = {});
        void                        endPass();
        void                        render(SceneHandle scene, CameraHandle camera);
        // Records a scene into the current pass, for render graph passes, with the camera fitted to the pass's
        // aspect ratio. A frame can make MAX_SCENE_DRAWS_PER_FRAME calls. A render target must use the swapchain's
        // color format and D32_SFLOAT depth, which are the formats of the scene's pipeline; drawing into
        // any other logs an error and leaves the pass cleared.
        void                        drawScene(SceneHandle scene, CameraHandle camera);
        bool                        running() const;
        bool                        recreateSwapchain();
        void                        setRenderTarget(const RenderTargetHandle& renderTarget);
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
        TextureHandle               getRenderTargetTexture(const RenderTargetHandle& renderTarget);
        TextureHandle               getPreviousRenderTargetTexture(const RenderTargetHandle& renderTarget);

        // window related
        Window                      createWindow(uint32_t width, uint32_t height);
        void                        destroyWindow(const Window& window);
        void                        pollEvents(CameraHandle camera);

    protected:
        bool                                                        createSwapchainResources();

        EngineConfig                                                config                                  = {};
        FrameResources                                              frameResources[MAX_FRAMES_IN_FLIGHT]    = {}; // Double buffering
        FenceHandle                                                 frameFence                              = {};
        uint64_t                                                    frameFenceValue                         = 0;  // last value a frame submit signaled
        Swapchain                                                   swapchain                               = {};
        std::unique_ptr<GeometryPool>                               geometryPool                            = {};
        std::unique_ptr<GpuScene>                                   gpuScene                                = {};
        std::unique_ptr<FrameStatsCollector>                        frameStats                              = {};
        std::unique_ptr<StatsOverlay>                               statsOverlay                            = {};
        RenderContext                                               ctx;
        std::unique_ptr<ShaderCompiler>                             shaderCompiler                          = {};
        HandleManager                                               handleManager                           = {};
        Window                                                      mainWindow                              = {};
        DescriptorSetLayoutHandle                                   bindlessDescriptorSetLayout             = {};
        DescriptorPoolHandle                                        bindlessDescriptorPool                  = {};
        DescriptorSetHandle                                         bindlessDescriptorSet                   = {};
        MaterialHandle                                              defaultMaterial                         = {}; // used for invalid material handles
        RenderTargetHandle                                          currentRenderTarget                     = {};
        uint64_t                                                    frameCount                              = 0;
        uint32_t                                                    imageIdx                                = 0;
        uint32_t                                                    sceneDrawCount                          = 0; // drawScene calls this frame
        float                                                       deltaTime                               = 0.0f; // seconds since the previous frame
        std::chrono::steady_clock::time_point                       lastFrameTime                           = {};
        bool                                                        isRunning                               = false;
        bool                                                        swapchainOutOfDate                      = false;
        bool                                                        swapchainRendered                       = false; // a pass rendered to the swapchain this frame

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

        MaterialHandle              loadMaterial(const tinygltf::Model& model, int materialIndex, GltfImportCache& cache);
        void                        finishMeshLoad(PendingMeshLoad& load);
        void                        finishTextureLoad(PendingTextureLoad& load);
        void                        finishLoads(bool wait);
        bool                        createGpuScene();
        void                        syncGpuScene();
        void                        queueInstanceRows(EntityHandle entity);
        void                        writeInstanceRows(EntityHandle entity);
        void                        updateCullModes(EntityHandle entity);
        void                        refreshEntitiesAfterMaterialChange();
        void                        writeMaterialRow(MaterialHandle material);
        void                        addEntityToDrawList(SceneHandle scene, EntityHandle entity);
        void                        markDrawListsDirty(EntityHandle entity);
        void                        rebuildDrawList(SceneHandle scene);
        void                        uploadDrawList(SceneDrawList& drawList);
        bool                        recordCulling(bool& recorded);
        void                        recordOcclusionCulling(uint64_t sceneDataAddress, uint32_t call, TextureHandle colorTexture, TextureHandle depthTexture, uint32_t width, uint32_t height, uint32_t levelCount);
        void                        createFrameStats();
        void                        updateFrameStats();
        uint32_t                    beginCullingTimer(const CommandBufferHandle& commandBuffer);
        bool                        createStatsOverlay();
        void                        updateStatsOverlay();
        void                        drawStatsOverlay();

        std::vector<PendingMeshLoad>                                            pendingMeshes;
        std::unordered_map<TextureHandle, PendingTextureLoad, HandleHash>       pendingTextures;

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
