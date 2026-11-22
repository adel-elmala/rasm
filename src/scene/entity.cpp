#include "rasm/engine.h"
#include "rasm/entity.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    EntityHandle Engine::createEntity(const std::string &name, MeshHandle mesh, MaterialHandle material, Transform transform)
    {
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

        if (mesh.isValid())
            queueInstanceRows(id);

        return id;
    }

    void Engine::updateTransform(EntityHandle entity, Transform transform)
    {
        if (!isLive(entity))
        {
            spdlog::error("Invalid entity handle for updating transform.");
            return;
        }

        registry.entities[entity.index].transform = transform;
        writeInstanceRows(entity);
    }

    Transform Engine::getTransform(EntityHandle entity)
    {
        if (!isLive(entity))
        {
            spdlog::error("Invalid entity handle for getting transform.");
            return Transform{};
        }

        return registry.entities[entity.index].transform;
    }

}
