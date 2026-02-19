#pragma once

#include <string>

#include "rasm/core/rasm.h"
#include "rasm/core/material.h"


namespace rasm {

    class Engine {
    public:
        Engine(const EngineConfig& config);
        ~Engine();

        SceneHandle createScene();
        MeshHandle loadMesh(const std::string& path);
        TextureHandle loadTexture(const std::string& path);
        Material createMaterial(MaterialTemplate type);

        void beginFrame();
        void endFrame();
        void render(SceneHandle scene, Entity camera);
        bool running() const;
    };

}