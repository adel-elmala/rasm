#include "rasm/renderer.h"
#include "rasm/entity.h"
#include "renderer/gpuScene.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    EntityHandle Renderer::createEntity(const std::string &name, MeshHandle mesh, MaterialHandle material, Transform transform)
    {
        if (mesh.isValid() && !isLive(mesh))
        {
            spdlog::error("createEntity: the mesh of entity '{}' was destroyed; the entity has none.", name);
            mesh = {};
        }
        if (material.isValid() && !isLive(material))
        {
            spdlog::error("createEntity: the material of entity '{}' was destroyed; the entity uses its mesh's or the default material.", name);
            material = {};
        }
        const HandleSlot slot = registry.entityHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::warn("Exceeded maximum number of entities ({} alive, MAX_ENTITIES).", registry.entityHandles.liveCount());
            return {};
        }
        const EntityHandle id{slot.index, slot.generation};

        auto entity = Entity{.handle = id,
                             .name = name,
                             .mesh = mesh,
                             .material = material,
                             .transform = transform};

        registry.entities.resize(registry.entityHandles.size());
        registry.entities[id.index] = entity;
        if (gpuScene->entities.size() <= id.index)
            gpuScene->entities.resize(id.index + 1);
        gpuScene->entities[id.index] = {};
        if (mesh.isValid())
            ++registry.meshes[mesh.index].users;
        if (material.isValid())
        {
            registry.materialUsers.resize(registry.materials.size(), 0);
            ++registry.materialUsers[material.index];
        }

        if (mesh.isValid())
            queueInstanceRows(id);

        return id;
    }

    void Renderer::updateTransform(EntityHandle entity, Transform transform)
    {
        if (!isLive(entity))
        {
            spdlog::error("Invalid entity handle for updating transform.");
            return;
        }

        registry.entities[entity.index].transform = transform;
        writeInstanceRows(entity);
    }

    // Its rows and animation ranges are free for another entity at once (releaseEntityRows), and the draw lists of its
    // scenes are rebuilt without it; they drop it from Scene::entities then.
    void Renderer::destroyEntity(EntityHandle entity)
    {
        if (!isLive(entity))
        {
            spdlog::error("destroyEntity: entity handle {{{}, {}}} is invalid or was destroyed.", entity.index, entity.generation);
            return;
        }
        releaseEntityRows(entity);
        EntityDrawState &state = gpuScene->entities[entity.index];
        // Only an entity that moved can be on the motion lists, which syncGpuScene walks every frame without checks.
        if (state.movedSync != UINT32_MAX)
        {
            std::erase(gpuScene->movedEntities, entity);
            std::erase(gpuScene->settlingEntities, entity);
        }
        for (uint32_t i = 0; i < MAX_SCENES; ++i)
        {
            if ((state.scenes & (1u << i)) != 0)
                gpuScene->drawLists[i].rebuild = true;
        }
        state = {};
        const Entity &entityRef = registry.entities[entity.index];
        if (isLive(entityRef.mesh))
            --registry.meshes[entityRef.mesh.index].users;
        if (isLive(entityRef.material))
            --registry.materialUsers[entityRef.material.index];
        registry.entities[entity.index] = {};
        registry.entityHandles.release(entity.index, entity.generation);
    }

    Transform Renderer::getTransform(EntityHandle entity)
    {
        if (!isLive(entity))
        {
            spdlog::error("Invalid entity handle for getting transform.");
            return Transform{};
        }

        return registry.entities[entity.index].transform;
    }

}
