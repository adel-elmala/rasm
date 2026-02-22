#include "rasm/core/engine.h"

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
        } else {
            // TODO: Implement OBJ loading
            spdlog::error("OBJ loading not implemented yet: {}", path);
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
        (void)type;
        const MaterialHandle id{nextHandle.material++, 1};
        return Material(id);
    }

    void Engine::beginFrame() {}

    void Engine::endFrame()
    {
        ++frameCount;
        // Prevent sample boilerplate from running forever.
        if (frameCount > 300)
        {
            isRunning = false;
        }
    }

    void Engine::render(const Scene &scene, const Entity &camera)
    {
        (void)scene;
        (void)camera;
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
        (void)name;
        const EntityHandle id{nextHandle.entity++, 1};
        return Entity(id);
    }

}
