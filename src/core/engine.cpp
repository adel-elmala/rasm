#include "rasm/core/engine.h"

namespace rasm
{

    Engine::Engine(const EngineConfig &config): config(config) {
        // In a real implementation, this is where we'd initialize the window, graphics context, etc.
    }
    Engine::~Engine() {}

    SceneHandle Engine::createScene()
    {
        const SceneId id{nextSceneIndex++, 1};
        return SceneHandle(this, id);
    }

    MeshHandle Engine::loadMesh(const std::string &path)
    {
        (void)path;
        return MeshHandle{nextMeshIndex++, 1};
    }

    TextureHandle Engine::loadTexture(const std::string &path)
    {
        (void)path;
        return TextureHandle{nextTextureIndex++, 1};
    }

    Material Engine::createMaterial(MaterialTemplate type)
    {
        (void)type;
        const MaterialHandle id{nextMaterialIndex++, 1};
        return Material(id);
    }

    void Engine::beginFrame() {}

    void Engine::endFrame() {
        ++frameCount;
        // Prevent sample boilerplate from running forever.
        if (frameCount > 300) {
            isRunning = false;
        }
    }

    void Engine::render(const SceneHandle& scene, const Entity& camera) {
        (void)scene;
        (void)camera;
    }

    bool Engine::running() const {
        return isRunning;
    }

    Entity Engine::createEntity(const SceneHandle& scene, const std::string& name) {
        if (!scene.isValid()) {
            return Entity();
        }
        (void)name;
        const EntityHandle id{nextEntityIndex++, 1};
        return Entity(id);
    }

}
