#include "rasm/core/engine.h"
#include "rasm/core/mesh.h"
#include "rasm/core/material.h"
#include "rasm/core/light.h"
#include "rasm/core/math.h"
#include "rasm/core/entity.h"
#include "rasm/core/camera.h"
#include "rasm/core/renderGraph.h"
#include "rasm/gfx/context.h"

#include "spdlog/spdlog.h"

#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace rasm
{

    Engine::Engine(const EngineConfig &config) : config(config)
    {
        // In a real implementation, this is where we'd initialize the window, graphics context, etc.
        spdlog::info("Engine initialized with config: appName={}, windowWidth={}, windowHeight={}, enableValidation={}",
                     config.appName, config.windowWidth, config.windowHeight, config.enableValidation);

        this->window = Window(this, config.windowWidth, config.windowHeight);
        this->mainWindow = this->window.createWindow();

        ctx = RenderContext(this);
        if (!ctx.initialize(config.preferredBackend))
        {
            spdlog::error("Failed to initialize render context.");
            ctx.cleanup();
            isRunning = false;
        }
    }

    Engine::~Engine()
    {
        // Clean up resources, free memory, etc.
        for (auto &tex : textureData)
        {
            stbi_image_free(tex.data);
        }

        for (auto &mesh : meshData)
        {
            if (mesh.type == MeshRaw::MeshType::GLTF)
            {
                // tinygltf::Model doesn't require explicit cleanup.
            }

            if (mesh.type == MeshRaw::MeshType::OBJ)
            {
                // If we had implemented OBJ loading, we would clean up any allocated resources here.
            }
        }

        ctx.cleanup();

        this->window.destroyWindow(this->mainWindow);
        spdlog::info("Engine shutdown, cleaned up resources.");
    }

    Scene Engine::createScene()
    {
        const SceneHandle id{nextHandle.scene++, 1};
        return Scene(this, id);
    }

    MeshHandle Engine::loadMesh(const std::string &path)
    {
        // find if the mesh is already loaded, if so return existing handle
        if (loadedMeshes.find(path) != loadedMeshes.end())
        {
            return loadedMeshes[path];
        }
        // find if the model extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedMeshExtensions.find(extension) == supportedMeshExtensions.end())
        {
            spdlog::error("Unsupported mesh format: {}", extension);
            return MeshHandle{};
        }

        if (extension == "gltf" || extension == "glb")
        {
            tinygltf::TinyGLTF loader;
            tinygltf::Model model;

            std::string err;
            std::string warn;
            bool ret = false;
            if (extension == "gltf")
            {
                ret = loader.LoadASCIIFromFile(&model, &err, &warn, path);
            }
            else
            {
                ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
            }

            if (!warn.empty())
            {
                spdlog::warn("GLTF loader warning: {}", warn);
            }
            if (!err.empty())
            {
                spdlog::error("GLTF loader error: {}", err);
            }
            if (!ret)
            {
                spdlog::error("Failed to load GLTF model: {}", path);
                return MeshHandle{};
            }

            MeshRaw mesh{};
            mesh.type = MeshRaw::MeshType::GLTF;
            mesh.data = std::move(model);
            meshData.push_back(mesh);
        }
        else
        {
            // TODO: Implement OBJ loading
            spdlog::error("OBJ loading not implemented yet: {}", path);
            return MeshHandle{};
        }

        auto handle = MeshHandle{nextHandle.mesh++, 1};
        loadedMeshes[path] = handle;

        return handle;
    }

    TextureHandle Engine::loadTexture(const std::string &path)
    {
        // find if the texture is already loaded, if so return existing handle
        if (loadedTextures.find(path) != loadedTextures.end())
        {
            return loadedTextures[path];
        }

        // find if the texture extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedTextureExtensions.find(extension) == supportedTextureExtensions.end())
        {
            spdlog::error("Unsupported texture format: {}", extension);
            return TextureHandle{};
        }

        int width, height, nChannels;
        unsigned char *data = stbi_load(path.c_str(), &width, &height, &nChannels, 0);

        if (!data)
        {
            spdlog::error("Failed to load texture: {}", path);
            return TextureHandle{};
        }

        TextureRaw tex{};
        tex.width = width;
        tex.height = height;
        tex.channels = nChannels;
        tex.data = data;

        textureData.push_back(tex);

        auto handle = TextureHandle{nextHandle.texture++, 1};
        loadedTextures[path] = handle;

        return handle;
    }

    Material Engine::createMaterial(MaterialTemplate type)
    {
        const MaterialHandle id{nextHandle.material++, 1};
        return Material(id);
    }

    RenderGraph Engine::createRenderGraph()
    {
        RenderGraph graph(this);
        return graph;
    }

    void Engine::beginFrame() {}

    void Engine::endFrame()
    {
        ++frameCount;
    }

    void Engine::render(Scene &scene, Entity &camera)
    {
        this->window.pollEvents();

        for (Entity &entity : scene.entities)
        {
            if (entity.hasComponent<Mesh>() && entity.hasComponent<Material>() && entity.hasComponent<Transform>())
            {
                // In a real implementation, this is where we'd issue draw calls to the GPU.
                // For this example, we'll just log the entity ID and its mesh/material.
                auto &meshComp = entity.getComponent<Mesh>();
                auto &matComp = entity.getComponent<Material>();
                spdlog::info("Rendering Entity {} with Mesh {} and Material {}",
                             entity.id().index, meshComp.id().index, matComp.id().index);
            }
            else if (entity.hasComponent<Light>())
            {
                auto &lightComp = entity.getComponent<Light>();
                spdlog::info("Light Entity {}, Color: ({}, {}, {}), Intensity: {}",
                             entity.id().index, lightComp.color.r, lightComp.color.g, lightComp.color.b, lightComp.intensity);
            }
        }

        auto &cameraComp = camera.getComponent<Camera>();
        spdlog::info("Camera Entity {}, Type: {}",
                     camera.id().index,
                     cameraComp.type == CameraType::PERSPECTIVE ? "Perspective" : "Orthographic");
    }

    bool Engine::running() const
    {
        return isRunning;
    }

    Entity Engine::createEntity(const Scene &scene, const std::string &name)
    {
        if (!scene.isValid())
        {
            return Entity();
        }
        const EntityHandle id{nextHandle.entity++, 1};
        return Entity(id, name);
    }

    EngineConfig Engine::getConfig() const
    {
        return config;
    }

    Window &Engine::getWindow()
    {
        return window;
    }

    WindowHandle Engine::getMainWindow() const
    {
        return mainWindow;
    }

    RenderContext &Engine::getRenderContext()
    {
        return ctx;
    }

    BufferHandle Engine::getNextBufferHandle()
    {
        return BufferHandle{nextHandle.buffer++, 1};
    }

    TextureHandle Engine::getNextTextureHandle()
    {
        return TextureHandle{nextHandle.texture++, 1};
    }

    ShaderHandle Engine::getNextShaderHandle()
    {
        return ShaderHandle{nextHandle.shader++, 1};
    }

    PipelineHandle Engine::getNextPipelineHandle()
    {
        return PipelineHandle{nextHandle.pipeline++, 1};
    }

    CommandPoolHandle Engine::getNextCommandPoolHandle()
    {
        return CommandPoolHandle{nextHandle.commandPool++, 1};
    }

    CommandBufferHandle Engine::getNextCommandBufferHandle()
    {
        return CommandBufferHandle{nextHandle.commandBuffer++, 1};
    }

    SemaphoreHandle Engine::getNextSemaphoreHandle()
    {
        return SemaphoreHandle{nextHandle.semaphore++, 1};
    }

    FenceHandle Engine::getNextFenceHandle()
    {
        return FenceHandle{nextHandle.fence++, 1};
    }

    SwapchainHandle Engine::getNextSwapchainHandle()
    {
        return SwapchainHandle{nextHandle.swapchain++, 1};
    }
}
