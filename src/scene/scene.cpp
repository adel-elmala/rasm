#include "rasm/scene.h"
#include "rasm/engine.h"
#include "renderer/environmentBake.h"
#include "renderer/lightClusters.h"

#include "spdlog/spdlog.h"

#include <cmath>

namespace rasm
{
    SceneHandle Engine::createScene()
    {
        const HandleSlot slot = registry.sceneHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::warn("Exceeded maximum number of scenes ({} alive, MAX_SCENES).", registry.sceneHandles.liveCount());
            return {};
        }
        const SceneHandle id{slot.index, slot.generation};

        registry.scenes.resize(registry.sceneHandles.size());
        registry.scenes[id.index] = Scene{.handle = id};

        return id;
    }

    void Engine::addEntityToScene(SceneHandle scene, EntityHandle entity)
    {
        if (!isLive(scene))
        {
            spdlog::error("Invalid scene handle for adding entity.");
            return;
        }

        if (!isLive(entity))
        {
            spdlog::error("Invalid entity handle for adding to scene.");
            return;
        }

        auto &sceneRef = registry.scenes[scene.index];

        if (sceneRef.entities.size() >= MAX_ENTITIES_PER_SCENE)
        {
            spdlog::warn("Exceeded maximum entities per scene.");
            return;
        }

        sceneRef.entities.push_back(entity);
        addEntityToDrawList(scene, entity);
    }

    void Engine::addCameraToScene(SceneHandle scene, CameraHandle camera)
    {
        if (!isLive(scene))
        {
            spdlog::error("Invalid scene handle for adding camera.");
            return;
        }

        if (!isLive(camera))
        {
            spdlog::error("Invalid camera handle for adding to scene.");
            return;
        }

        auto &sceneRef = registry.scenes[scene.index];

        if (sceneRef.cameras.size() >= MAX_CAMERAS_PER_SCENE)
        {
            spdlog::warn("Exceeded maximum cameras per scene.");
            return;
        }

        sceneRef.cameras.push_back(camera);
    }

    void Engine::addLightToScene(SceneHandle scene, LightHandle light)
    {
        if (!isLive(scene))
        {
            spdlog::error("Invalid scene handle for adding light.");
            return;
        }

        if (!isLive(light))
        {
            spdlog::error("Invalid light handle {} for adding to scene.", light.index);
            return;
        }

        // A light is in a scene once: added twice, it would light the scene twice.
        auto &lightScenes = gpuLights->lightScenes;
        if (lightScenes.size() <= light.index)
            lightScenes.resize(light.index + 1, 0);
        const auto bit = static_cast<uint16_t>(1u << scene.index);
        if ((lightScenes[light.index] & bit) != 0)
        {
            spdlog::warn("Light {} is already in scene {}.", light.index, scene.index);
            return;
        }
        lightScenes[light.index] |= bit;
        registry.scenes[scene.index].lights.push_back(light);
        gpuLights->lists[scene.index].rebuild = true;
    }

    void Engine::setSceneEnvironment(SceneHandle scene, const SceneEnvironment &environment)
    {
        if (!isLive(scene))
        {
            spdlog::error("Invalid scene handle for setting the environment.");
            return;
        }
        // An invalid handle is no environment; a handle loadEnvironment never gave is a mistake.
        if (environment.environment.isValid() && !isLive(environment.environment))
        {
            spdlog::error("Invalid environment handle for scene {}.", scene.index);
            return;
        }
        if (!(environment.intensity >= 0.0f) || !std::isfinite(environment.intensity) || !std::isfinite(environment.rotation))
        {
            spdlog::error("An environment needs a finite intensity of at least 0 and a finite rotation.");
            return;
        }
        registry.scenes[scene.index].environment = environment;
    }
} // namespace rasm
