#include "rasm/scene.h"
#include "rasm/engine.h"
#include "renderer/environmentBake.h"
#include "renderer/gpuScene.h"
#include "renderer/lightClusters.h"
#include "renderer/renderer.h"

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

        // An entity is in a scene once: added twice, it would be drawn twice.
        if (!addEntityToDrawList(scene, entity))
        {
            spdlog::warn("Entity {} is already in scene {}.", entity.index, scene.index);
            return;
        }
        sceneRef.entities.push_back(entity);
    }

    // The entity stays; the scene's draw list is rebuilt without it, and drops it from Scene::entities then.
    void Engine::removeEntityFromScene(SceneHandle scene, EntityHandle entity)
    {
        if (!isLive(scene) || !isLive(entity))
        {
            spdlog::error("removeEntityFromScene: invalid or destroyed scene or entity handle.");
            return;
        }
        const auto bit = static_cast<uint16_t>(1u << scene.index);
        EntityDrawState &state = gpuScene->entities[entity.index];
        if ((state.scenes & bit) == 0)
        {
            spdlog::warn("removeEntityFromScene: entity {} is not in scene {}.", entity.index, scene.index);
            return;
        }
        state.scenes = static_cast<uint16_t>(state.scenes & ~bit);
        gpuScene->drawLists[scene.index].rebuild = true;
    }

    void Engine::removeLightFromScene(SceneHandle scene, LightHandle light)
    {
        if (!isLive(scene) || !isLive(light))
        {
            spdlog::error("removeLightFromScene: invalid or destroyed scene or light handle.");
            return;
        }
        const auto bit = static_cast<uint16_t>(1u << scene.index);
        auto &lightScenes = gpuLights->lightScenes;
        if (light.index >= lightScenes.size() || (lightScenes[light.index] & bit) == 0)
        {
            spdlog::warn("removeLightFromScene: light {} is not in scene {}.", light.index, scene.index);
            return;
        }
        lightScenes[light.index] = static_cast<uint16_t>(lightScenes[light.index] & ~bit);
        gpuLights->lists[scene.index].rebuild = true;
    }

    // True when a render graph holds a view of the scene or camera, which would draw it after it is gone: the caller
    // refuses then. Engine::render's own graph lets go first.
    bool Engine::viewHolds(SceneHandle scene, CameraHandle camera, const char *caller)
    {
        if (renderer)
            renderer->release(scene, camera);
        for (uint32_t i = 0; i < MAX_SCENE_VIEWS; ++i)
        {
            const SceneView &view = gpuScene->views[i];
            if (view.graphs == 0 || !((scene.isValid() && view.scene == scene) || (camera.isValid() && view.camera == camera)))
                continue;
            spdlog::error("{}: {} graph(s) draw view {} (scene {}, camera {}) with it. Destroy those render graphs first, or let them go out of scope.", caller,
                          view.graphs, i, view.scene.index, view.camera.index);
            return true;
        }
        return false;
    }

    void Engine::destroyScene(SceneHandle scene)
    {
        if (!isLive(scene))
        {
            spdlog::error("destroyScene: scene handle {{{}, {}}} is invalid or was destroyed.", scene.index, scene.generation);
            return;
        }
        if (viewHolds(scene, {}, "destroyScene"))
            return;

        // Its index goes to the next scene, so no entity or light may keep its bit.
        const auto bit = static_cast<uint16_t>(1u << scene.index);
        Scene &sceneRef = registry.scenes[scene.index];
        for (EntityHandle entity : sceneRef.entities)
        {
            if (isLive(entity) && entity.index < gpuScene->entities.size())
                gpuScene->entities[entity.index].scenes = static_cast<uint16_t>(gpuScene->entities[entity.index].scenes & ~bit);
        }
        for (LightHandle light : sceneRef.lights)
        {
            if (light.index < gpuLights->lightScenes.size())
                gpuLights->lightScenes[light.index] = static_cast<uint16_t>(gpuLights->lightScenes[light.index] & ~bit);
        }
        SceneDrawList &drawList = gpuScene->drawLists[scene.index];
        for (BufferHandle buffer : drawList.buffers)
        {
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
        }
        drawList = {};
        SceneLightList &lightList = gpuLights->lists[scene.index];
        for (BufferHandle buffer : lightList.buffers)
        {
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
        }
        lightList = {};
        sceneRef = {};
        registry.sceneHandles.release(scene.index, scene.generation);
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
