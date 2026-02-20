#pragma once

#include <cstdint>
#include <string>

#include "rasm/core/rasm.h"
#include "rasm/core/entity.h"
#include "rasm/core/scene.h"
#include "rasm/core/material.h"


namespace rasm {

    class Engine {
    public:
        Engine(const EngineConfig& config);
        ~Engine();

        Scene createScene();
        MeshHandle loadMesh(const std::string& path);
        TextureHandle loadTexture(const std::string& path);
        Material createMaterial(MaterialTemplate type);

        void beginFrame();
        void endFrame();
        void render(const Scene& scene, const Entity& camera);
        bool running() const;

        Entity createEntity(const Scene& scene, const std::string& name);

    private:
        EngineConfig config;
        uint64_t nextSceneIndex = 1;
        uint64_t nextEntityIndex = 1;
        uint64_t nextMeshIndex = 1;
        uint64_t nextTextureIndex = 1;
        uint64_t nextMaterialIndex = 1;
        uint64_t frameCount = 0;
        bool isRunning = true;
    };

}
