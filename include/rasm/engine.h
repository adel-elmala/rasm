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
#include "renderer/shaderCompiler.h"      // TODO(D2): private header (src/), needed until Engine hides its members
#include "rasm/handle.h"
#include "rasm/profiler.h"
#include "rasm/render/renderContext.h"
#include "renderer/uberMaterialTypes.h"   // TODO(D2): private header (src/), needed for convertToGfxMaterial

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinygltf { struct Model; }
namespace tinygltf { struct Node; }

namespace rasm {

    constexpr uint32_t MAX_MATERIALS = 256;
    constexpr uint32_t MAX_SHADERS   = 256;
    constexpr uint32_t MAX_CAMERAS   = 8;

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
        LightHandle                 createLight(LightType type, glm::vec3 color = glm::vec3(1.0f), glm::vec3 position = glm::vec3(1.0f), glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f), float intensity = 1.0f, float range = 10.0f);
        void                        addEntityToScene(SceneHandle scene, EntityHandle entity);
        void                        addCameraToScene(SceneHandle scene, CameraHandle camera);
        void                        addLightToScene(SceneHandle scene, LightHandle light);
        MeshHandle                  createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
        MeshHandle                  loadMesh(const std::string& path);
        MeshHandle                  loadGLTF(const std::string& path);
        BufferHandle                uploadMesh(const MeshHandle& handle);
        // Color images default to sRGB; pass R8G8B8A8_UNORM for normal/MR/AO data.
        TextureHandle               loadTexture(const std::string& path, Format format = Format::R8G8B8A8_SRGB);
        ShaderHandle                createShader(const std::string& shaderSource);
        MaterialHandle              createMaterial(MaterialType type, std::vector<TextureHandle> textures = {}, ShaderHandle shader = ShaderHandle{});
        std::vector<MaterialHandle> loadMaterials(MeshHandle meshHandle);
        MaterialHandle              loadMaterial(const tinygltf::Model& model, int materialIndex);
        TextureHandle               createTexture(const ResourceDesc& desc);
        BufferHandle                createBuffer(const ResourceDesc& desc);
        RenderTargetHandle          createRenderTarget(const ResourceDesc& desc);
        RenderGraph                 createRenderGraph();
        const Material&             getMaterial(const MaterialHandle &handle);
        gfx::Material               convertToGfxMaterial(const Material &mat);
        Transform                   getTransform(EntityHandle entity);
        Transform                   getCameraTransform(CameraHandle camera);
        CameraProjection            getCameraProjection(CameraHandle camera);
        CameraController            getCameraController(CameraHandle camera);
        glm::mat4                   getProjectionMatrix(CameraHandle camera);
        glm::mat4                   getViewMatrix(CameraHandle camera);
        void                        updateCameraAspectRatio(float aspect);
        void                        updateCamera(CameraHandle camera, const CameraInput &input);
        void                        updateTransform(EntityHandle entity, Transform transform);
        void                        setCameraTransform(CameraHandle camera, Transform transform);
        void                        setCameraProjection(CameraHandle camera, CameraProjection projection);
        void                        setCameraController(CameraHandle camera, CameraController controller);
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
        const Window&               getMainWindow() const;
        RenderContext&              getRenderContext();
        MeshSize                    getMeshByteSize(const MeshHandle &handle) const;
        void                        queryMemoryStats();
        uint32_t                    addBindlessTexture(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, uint32_t slot = MAX_BINDLESS_TEXTURES);
        uint32_t                    getBindlessTextureIndex(const TextureHandle &texture);
        RenderTargetAttachments     getRenderTargetAttachments(const RenderTargetHandle &renderTarget);
        void                        updateUniforms(PreparedScene &preparedScene);

        // window related
        Window                      createWindow(uint32_t width, uint32_t height);
        VkSurfaceKHR                createSurfaceVk(const Window& window, VkInstance instance) const;
        void                        destroyWindow(const Window& window);
        void                        pollEvents(CameraHandle camera);


        std::vector<SubGLTFRaw>        extractGLTFRaws(const tinygltf::Model &model, uint32_t meshIndex, const glm::mat4 &globalTransform);
        std::vector<SubGLTFRaw>        traverseNodes(const tinygltf::Model &model, int nodeIndex, const glm::mat4 &parentTransform);
        GLTFRaw                        processGLTF(const tinygltf::Model &model);
        GLTFRaw                        processMesh(const Mesh &mesh);

    protected:
        EngineConfig                                                config                                  = {};
        Profiler                                                    profiler                                = {};
        FrameResources                                              frameResources[MAX_FRAMES_IN_FLIGHT]    = {}; // Double buffering
        Swapchain                                                   swapchain                               = {};
        TextureHandle                                               depthTexture                            = {};
        std::unordered_map<SceneHandle, CompiledScene, HandleHash>  compiledScenes                          = {}; // TODO: Delete when no longer needed
        std::unordered_map<SceneHandle, PreparedScene, HandleHash>  preparedScenes                          = {};
        RenderContext                                               ctx                                     = {};
        ShaderCompiler                                              shaderCompiler                          = {};
        HandleManager                                               handleManager                           = {};
        Window                                                      mainWindow                              = {};
        DescriptorSetLayoutHandle                                   bindlessDescriptorSetLayout             = {};
        DescriptorPoolHandle                                        bindlessDescriptorPool                  = {};
        DescriptorSetHandle                                         bindlessDescriptorSet                   = {};
        SceneHandle                                                 currentScene                            = {};
        RenderTargetHandle                                          currentRenderTarget                     = {};
        uint64_t                                                    frameCount                              = 0;
        uint32_t                                                    imageIdx                                = 0;
        bool                                                        isRunning                               = false;

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
