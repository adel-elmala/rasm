// clang-format off
#pragma once

#include "rasm/core/types.h"
#include "rasm/core/entity.h"
#include "rasm/core/scene.h"
#include "rasm/core/material.h"
#include "rasm/core/renderGraph.h"
#include "rasm/core/window.h"
#include "rasm/core/light.h"
#include "rasm/core/mesh.h"
#include "rasm/core/shaderCompiler.h"
#include "rasm/core/handle.h"
#include "rasm/core/profiler.h"
#include "rasm/gfx/context.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rasm {

    constexpr uint32_t MAX_MATERIALS = 256;
    constexpr uint32_t MAX_SHADERS = 256;
    constexpr uint32_t MAX_CAMERAS = 8;
    constexpr uint32_t MAX_LIGHTS = 128;

    class Engine {
        friend class RenderGraph;
        friend class RenderContext;

    public:
                                    Engine(const EngineConfig& config);
                                    ~Engine();
        SceneHandle                 createScene();
        CompiledScene               compileScene(SceneHandle scene);
        PreparedScene               prepareScene(SceneHandle scene);
        EntityHandle                createEntity(const std::string& name, MeshHandle mesh = MeshHandle{}, MaterialHandle material = MaterialHandle{}, Transform transform = Transform{});
        CameraHandle                createCamera(CameraProjection projection, Transform transform);
        LightHandle                 createLight(LightType type, glm::vec3 color = glm::vec3(1.0f), float intensity = 1.0f, glm::vec3 position = glm::vec3(1.0f), glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f));
        void                        addEntityToScene(SceneHandle scene, EntityHandle entity);
        void                        addCameraToScene(SceneHandle scene, CameraHandle camera);
        void                        addLightToScene(SceneHandle scene, LightHandle light);
        MeshHandle                  createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
        MeshHandle                  loadMesh(const std::string& path);
        BufferHandle                uploadMesh(const MeshHandle& handle);
        TextureHandle               loadTexture(const std::string& path);
        ShaderHandle                createShader(const std::string& shaderSource);
        MaterialHandle              createMaterial(MaterialType type, std::vector<TextureHandle> textures = {}, ShaderHandle shader = ShaderHandle{});
        TextureHandle               createTexture(const ResourceDesc& desc);
        BufferHandle                createBuffer(const ResourceDesc& desc);
        RenderTargetHandle          createRenderTarget(const ResourceDesc& desc);
        RenderGraph                 createRenderGraph();
        const Material&             getMaterial(const MaterialHandle &handle);
        Transform                   getTransform(EntityHandle entity);
        Transform                   getCameraTransform(CameraHandle camera);
        CameraProjection            getCameraProjection(CameraHandle camera);
        void                        updateTransform(EntityHandle entity, Transform transform);
        void                        setCameraTransform(CameraHandle camera, Transform transform);
        void                        setCameraProjection(CameraHandle camera, CameraProjection projection);
        void                        beginFrame(CameraHandle camera);
        void                        beginOffscreenFrame(const RenderTargetHandle& renderTarget, CameraHandle camera);
        void                        endOffscreenFrame();
        void                        endFrame();
        void                        beginPass();
        void                        endPass();
        void                        render(SceneHandle scene, CameraHandle camera);
        void                        render2(SceneHandle scene, CameraHandle camera);
        void                        render(SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget);
        bool                        running() const;
        void                        recreateSwapchain();
        void                        setRenderTarget(const RenderTargetHandle& renderTarget);
        void                        resetRenderTarget();
        void                        setScene(SceneHandle scene);
        SceneHandle                 getScene() const;
        DescriptorSetHandle         getBindlessDescriptorSet();
        const FrameResources&       getCurrentFrameResources();
        EngineConfig                getConfig() const;
        WindowHandle                getMainWindow() const;
        RenderContext&              getRenderContext();
        MeshSize                    getMeshByteSize(const MeshHandle &handle) const;
        std::vector<GltfRaw>        processMesh(const Mesh &mesh);
        void                        queryMemoryStats();
        uint32_t                    addBindlessTexture(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, uint32_t slot = MAX_BINDLESS_TEXTURES);
        uint32_t                    getBindlessTextureIndex(const TextureHandle &texture);
        RenderTargetAttachments     getRenderTargetAttachments(const RenderTargetHandle &renderTarget);

        // window related
        WindowHandle                createWindow(uint32_t width, uint32_t height);
        VkSurfaceKHR                createSurfaceVk(WindowHandle handle, VkInstance instance) const;
        void                        destroyWindow(WindowHandle handle);
        void                        pollEvents(CameraHandle camera);

    protected:
        EngineConfig                                                config;
        Profiler                                                    profiler{};
        FrameResources                                              frameResources[MAX_FRAMES_IN_FLIGHT]; // Double buffering
        Swapchain                                                   swapchain{};
        TextureHandle                                               depthTexture{};
        std::unordered_map<SceneHandle, CompiledScene, HandleHash>  compiledScenes; // TODO: Delete when no longer needed
        std::unordered_map<SceneHandle, PreparedScene, HandleHash>  preparedScenes;
        RenderContext                                               ctx{};
        ShaderCompiler                                              shaderCompiler{};
        HandleManager                                               handleManager{};
        WindowHandle                                                mainWindow{};
        DescriptorSetLayoutHandle                                   bindlessDescriptorSetLayout{};
        DescriptorPoolHandle                                        bindlessDescriptorPool{};
        DescriptorSetHandle                                         bindlessDescriptorSet{};
        SceneHandle                                                 currentScene{};
        RenderTargetHandle                                          currentRenderTarget{};
        uint64_t                                                    frameCount = 0;
        uint32_t                                                    imageIdx = 0;
        bool                                                        isRunning = true;
        bool                                                        resized = false;

    private:
        struct Registery
        {
            std::vector<Entity>      entities;
            std::vector<Material>    materials;
            std::vector<Camera>      cameras;
            std::vector<std::string> shaders;
            std::vector<Mesh>        meshes;
            std::vector<Light>       lights;
            std::vector<Scene>       scenes;

            std::unordered_map<std::string, MeshHandle>                 loadedMeshes;
            std::unordered_map<std::string, TextureHandle>              loadedTextures;
            std::unordered_map<TextureHandle, TextureRaw, HandleHash>   textureData;
            std::unordered_map<TextureHandle, uint32_t, HandleHash>     bindlessTextureIndexMap; // slot in the bindless descriptor set
        };

        Registery registery = {};
    };

}
// clang-format on
