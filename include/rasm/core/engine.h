// clang-format off
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "rasm/core/types.h"
#include "rasm/core/entity.h"
#include "rasm/core/scene.h"
#include "rasm/core/material.h"
#include "rasm/core/renderGraph.h"
#include "rasm/core/window.h"
#include "rasm/gfx/context.h"
#include "rasm/core/shaderCompiler.h"
#include "rasm/core/mesh.h"

namespace rasm {


    constexpr uint32_t MAX_MATERIALS = 1024;
    constexpr uint32_t MAX_SHADERS = 1024;
    constexpr uint32_t MAX_CAMERAS = 1024;
    constexpr uint32_t MAX_LIGHTS = 1024;

    class Engine {
    friend class Window;
    friend class RenderGraph;


    public:
                                    Engine(const EngineConfig& config);
                                    ~Engine();
        SceneHandle                 createScene();
        void                        addEntityToScene(SceneHandle scene, EntityHandle entity);
        void                        addCameraToScene(SceneHandle scene, CameraHandle camera);
        void                        addLightToScene(SceneHandle scene, LightHandle light);
        MeshHandle                  createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
        MeshHandle                  loadMesh(const std::string& path);
        BufferHandle                uploadMesh(const MeshHandle& handle);
        TextureHandle               loadTexture(const std::string& path);
        MaterialHandle              createMaterial(MaterialType type, TextureHandle textures[] = nullptr, ShaderHandle shader = ShaderHandle{});
        ShaderHandle                createShader(const std::string& shaderSource);
        ResourceHandle              createResource(const ResourceDesc& desc);
        RenderGraph                 createRenderGraph();
        EntityHandle                createEntity(const std::string& name, MeshHandle mesh = MeshHandle{}, MaterialHandle material = MaterialHandle{}, Transform transform = Transform{});
        RenderTargetHandle          createRenderTarget(const std::string& name, uint32_t width = 0, uint32_t height = 0, Format colorFormat = Format::UNKNOWN, Format depthFormat = Format::UNKNOWN);
        CameraHandle                createCamera(CameraProjection projection, Transform transform);
        LightHandle                 createLight(LightType type, glm::vec3 color = glm::vec3(1.0f), float intensity = 1.0f, glm::vec3 position = glm::vec3(1.0f), glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f));
        Transform                   getTransform(EntityHandle entity);
        void                        updateTransform(EntityHandle entity, Transform transform);
        void                        setCameraTransform(CameraHandle camera, Transform transform);
        void                        setCameraProjection(CameraHandle camera, CameraProjection projection);
        void                        beginFrame(CameraHandle camera);
        void                        beginOffscreenFrame(const RenderTargetHandle &renderTarget, CameraHandle camera);
        void                        endOffscreenFrame();
        void                        endFrame();
        void                        beginPass();
        void                        endPass();
        void                        render(SceneHandle scene, CameraHandle camera);
        void                        render(SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget);
        bool                        running() const;
        void                        recreateSwapchain();
        void                        setRenderTarget(const RenderTargetHandle &renderTarget);
        void                        resetRenderTarget();
        void                        setScene(SceneHandle scene);
        SceneHandle                 getScene() const;
        FrameResources&             getCurrentFrameResources();
        EngineConfig                getConfig() const;
        Window&                     getWindow();
        WindowHandle                getMainWindow() const;
        RenderContext&              getRenderContext();
        MeshHandle                  getNextMeshHandle();
        ShaderHandle                getNextShaderHandle();
        BufferHandle                getNextBufferHandle();
        TextureHandle               getNextTextureHandle();
        PipelineHandle              getNextPipelineHandle();
        CommandPoolHandle           getNextCommandPoolHandle();
        CommandBufferHandle         getNextCommandBufferHandle();
        SemaphoreHandle             getNextSemaphoreHandle();
        FenceHandle                 getNextFenceHandle();
        SwapchainHandle             getNextSwapchainHandle();
        RenderTargetHandle          getNextRenderTargetHandle();
        DescriptorSetLayoutHandle   getNextDescriptorSetLayoutHandle();
        DescriptorPoolHandle        getNextDescriptorPoolHandle();
        DescriptorSetHandle         getNextDescriptorSetHandle();
        MaterialHandle              getNextMaterialHandle();
        CameraHandle                getNextCameraHandle();
        EntityHandle                getNextEntityHandle();
        LightHandle                 getNextLightHandle();
        CompiledScene               compileScene(SceneHandle scene);

    protected:
        EngineConfig                                                config;
        FrameResources                                              frameResources[MAX_FRAMES_IN_FLIGHT]; // Double buffering
        Swapchain                                                   swapchain{};
        TextureHandle                                               depthTexture{};
        std::unordered_map<std::string, MeshHandle>                 loadedMeshes;
        std::unordered_map<std::string, TextureHandle>              loadedTextures;
        std::unordered_map<TextureHandle, TextureRaw, HandleHash>   textureData;
        std::unordered_map<SceneHandle, CompiledScene, HandleHash>  compiledScenes;
        RenderContext                                               ctx{};
        ShaderCompiler                                              shaderCompiler{};
        HandleCounters                                              nextHandle{};
        WindowHandle                                                mainWindow{};
        Window                                                      window{};
        DescriptorSetLayoutHandle                                   bindlessDescriptorSetLayout{};
        DescriptorPoolHandle                                        bindlessDescriptorPool{};
        DescriptorSetHandle                                         bindlessDescriptorSet{};
        SceneHandle                                                 currentScene{};
        RenderTargetHandle                                          currentRenderTarget{};
        uint64_t                                                    frameCount = 0;
        uint32_t                                                    imageIdx = 0;
        bool                                                        isRunning = true;
        bool                                                        resized = false;


        struct Registery 
        {
            Entity          entities[MAX_ENTITIES]                    = {};
            Material        materials[MAX_MATERIALS]                  = {};
            Camera          cameras[MAX_CAMERAS]                      = {};
            std::string     shaders[MAX_SHADERS]                      = {};
            Mesh            meshes[MAX_MESHES]                        = {};
            Light           lights[MAX_LIGHTS]                        = {};
            Scene           scenes[MAX_SCENES]                        = {};
        };

        Registery registery = {};
    };

}
// clang-format on
