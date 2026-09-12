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

namespace rasm {


    class Engine {
    friend class Window;

    public:
                                    Engine(const EngineConfig& config);
                                    ~Engine();
        Scene                       createScene();
        MeshHandle                  createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
        MeshHandle                  loadMesh(const std::string& path);
        BufferHandle                uploadMesh(const MeshHandle& handle);
        TextureHandle               loadTexture(const std::string& path);
        Material*                   createMaterial(MaterialTemplate type);
        ResourceHandle              createResource(const ResourceDesc& desc);
        RenderGraph                 createRenderGraph();
        Entity                      createEntity(const Scene& scene, const std::string& name);
        RenderTargetHandle          createRenderTarget(const std::string& name, uint32_t width = 0, uint32_t height = 0, Format colorFormat = Format::UNKNOWN, Format depthFormat = Format::UNKNOWN);
        void                        beginFrame(Entity &camera);
        void                        beginOffscreenFrame(const RenderTargetHandle &renderTarget, Entity &camera);
        void                        endOffscreenFrame(const RenderTargetHandle &renderTarget);
        void                        endFrame();
        void                        render(Scene& scene, Entity& camera);
        void                        render(Scene& scene, Entity& camera, const RenderTargetHandle& renderTarget);
        bool                        running() const;
        void                        recreateSwapchain();
        EngineConfig                getConfig() const;
        Window&                     getWindow();
        WindowHandle                getMainWindow() const;
        RenderContext&              getRenderContext();
        MeshHandle                  getNextMeshHandle();
        BufferHandle                getNextBufferHandle();
        TextureHandle               getNextTextureHandle();
        ShaderHandle                getNextShaderHandle();
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
        CompiledScene               compileScene(Scene& scene);

    protected:
        EngineConfig                                                config;
        FrameResources                                              frameResources[MAX_FRAMES_IN_FLIGHT]; // Double buffering
        Swapchain                                                   swapchain{};
        TextureHandle                                               depthTexture{};
        std::unordered_map<std::string, MeshHandle>                 loadedMeshes;
        std::unordered_map<std::string, TextureHandle>              loadedTextures;
        std::unordered_map<TextureHandle, TextureRaw, HandleHash>   textureData;
        std::unordered_map<MeshHandle, MeshRaw, HandleHash>         meshData;
        std::unordered_map<MaterialHandle, Material*, HandleHash>   loadedMaterials;
        std::unordered_map<SceneHandle, CompiledScene, HandleHash>  compiledScenes;
        RenderContext                                               ctx{};
        ShaderCompiler                                              shaderCompiler{};
        HandleCounters                                              nextHandle{};
        WindowHandle                                                mainWindow{};
        Window                                                      window{};
        DescriptorSetLayoutHandle                                   bindlessDescriptorSetLayout{};
        DescriptorPoolHandle                                        bindlessDescriptorPool{};
        DescriptorSetHandle                                         bindlessDescriptorSet{};
        uint64_t                                                    frameCount = 0;
        uint32_t                                                    imageIdx = 0;
        bool                                                        isRunning = true;
        bool                                                        resized = false;
    };

}
// clang-format on
