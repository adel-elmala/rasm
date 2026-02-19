#include "rasm/core/engine.h"

namespace rasm
{

    Engine::Engine(const EngineConfig &config) {}
    Engine::~Engine() {}

    SceneHandle Engine::createScene()
    {
        return SceneHandle();
    }
    MeshHandle Engine::loadMesh(const std::string &path)
    {
        return MeshHandle();
    }
    TextureHandle Engine::loadTexture(const std::string &path)
    {
        return TextureHandle();
    }
    Material Engine::createMaterial(MaterialTemplate type)
    {
        return Material();
    }

    void Engine::beginFrame() {}
    void Engine::endFrame() {}
    void Engine::render(SceneHandle scene, Entity camera) {}
    bool Engine::running() const { return true; }

}