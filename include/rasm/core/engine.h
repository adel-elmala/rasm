// clang-format off
#pragma once

#include <cstdint>
#include <string>

#include "rasm/core/types.h"
#include "rasm/core/rasm.h"
#include "rasm/core/entity.h"
#include "rasm/core/scene.h"
#include "rasm/core/material.h"
#include "rasm/core/renderGraph.h"
#include "rasm/core/window.h"
#include "rasm/gfx/context.h"

namespace rasm {

    class Engine {
    public:
                        Engine(const EngineConfig& config);
                        ~Engine();
        Scene           createScene();
        MeshHandle      loadMesh(const std::string& path);
        TextureHandle   loadTexture(const std::string& path);
        Material        createMaterial(MaterialTemplate type);
        RenderGraph     createRenderGraph();
        Entity          createEntity(const Scene& scene, const std::string& name);
        void            beginFrame();
        void            endFrame();
        void            render(Scene& scene, Entity& camera);
        bool            running() const;
        EngineConfig    getConfig() const;
        Window&         getWindow() const;
        WindowHandle    getMainWindow() const;
        RenderContext&  getRenderContext() const;
        BufferHandle    getNextBufferHandle();
        TextureHandle   getNextTextureHandle();
        ShaderHandle    getNextShaderHandle();

    private:
        EngineConfig                                    config;
        std::unordered_map<std::string, MeshHandle>     loadedMeshes;
        std::unordered_map<std::string, TextureHandle>  loadedTextures;
        std::vector<TextureRaw>                         textureData;
        std::vector<MeshRaw>                            meshData;
        RenderContext                                   ctx{};
        HandleCounters                                  nextHandle{};
        WindowHandle                                    mainWindow{};
        Window                                          window{};
        uint64_t                                        frameCount = 0;
        bool                                            isRunning = true;
    };

}
// clang-format on
