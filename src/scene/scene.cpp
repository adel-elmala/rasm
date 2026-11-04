#include "rasm/scene.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    SceneHandle Engine::createScene()
    {
        const SceneHandle id = handleManager.getNextSceneHandle();

        if (id.index >= MAX_SCENES)
        {
            spdlog::warn("Exceeded maximum number of scenes.");
            return {};
        }

        auto scene = Scene{.handle = id,
                           .entities = {},
                           .entityCount = 0,
                           .cameras = {},
                           .cameraCount = 0,
                           .lights = {},
                           .lightCount = 0};

        registry.scenes.resize(id.index + 1);
        registry.scenes[id.index] = scene;

        currentScene = id;

        return id;
    }

    void Engine::addEntityToScene(SceneHandle scene, EntityHandle entity)
    {
        if (!scene.isValid() || scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for adding entity.");
            return;
        }

        if (!entity.isValid())
        {
            spdlog::error("Invalid entity handle for adding to scene.");
            return;
        }

        auto &sceneRef = registry.scenes[scene.index];

        if (sceneRef.entityCount + 1 > MAX_ENTITIES_PER_SCENE)
        {
            spdlog::warn("Exceeded maximum entities per scene.");
            return;
        }

        sceneRef.entities[sceneRef.entityCount++] = entity;
    }

    void Engine::addCameraToScene(SceneHandle scene, CameraHandle camera)
    {
        if (!scene.isValid() || scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for adding camera.");
            return;
        }

        if (!camera.isValid())
        {
            spdlog::error("Invalid camera handle for adding to scene.");
            return;
        }

        auto &sceneRef = registry.scenes[scene.index];

        if (sceneRef.cameraCount + 1 > MAX_CAMERAS_PER_SCENE)
        {
            spdlog::warn("Exceeded maximum cameras per scene.");
            return;
        }

        sceneRef.cameras[sceneRef.cameraCount++] = camera;
    }

    void Engine::addLightToScene(SceneHandle scene, LightHandle light)
    {
        if (!scene.isValid() || scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for adding light.");
            return;
        }

        if (!light.isValid())
        {
            spdlog::error("Invalid light handle for adding to scene.");
            return;
        }

        auto &sceneRef = registry.scenes[scene.index];

        if (sceneRef.lightCount + 1 > MAX_LIGHTS_PER_SCENE)
        {
            spdlog::warn("Exceeded maximum lights per scene.");
            return;
        }

        sceneRef.lights[sceneRef.lightCount++] = light;
    }

} // namespace rasm
