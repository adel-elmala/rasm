#include "rasm/core/engine.h"
#include "rasm/core/entity.h"

namespace rasm
{
    EntityHandle Engine::createEntity(const std::string &name, MeshHandle mesh, MaterialHandle material, Transform transform)
    {
        const EntityHandle id = getNextEntityHandle();

        auto entity = Entity{.handle = id,
                             .name = name,
                             .mesh = mesh,
                             .material = material,
                             .transform = transform};

        if (id.index >= MAX_ENTITIES)
        {
            spdlog::error("Exceeded maximum number of entities.");
            isRunning = false;
            return {};
        }

        registery.entities.resize(id.index + 1);
        registery.entities[id.index] = entity;

        return id;
    }

    void Engine::updateTransform(EntityHandle entity, Transform transform)
    {
        if (entity.index >= registery.entities.size() || !registery.entities[entity.index].handle.isValid())
        {
            spdlog::error("Invalid entity handle for updating transform.");
            isRunning = false;
            return;
        }

        registery.entities[entity.index].transform = transform;
    }

    Transform Engine::getTransform(EntityHandle entity)
    {
        if (entity.index >= registery.entities.size() || !registery.entities[entity.index].handle.isValid())
        {
            spdlog::error("Invalid entity handle for getting transform.");
            isRunning = false;
            return Transform{};
        }

        return registery.entities[entity.index].transform;
    }

}
