#include "rasm/core/engine.h"
#include "rasm/core/entity.h"

namespace rasm
{
    EntityHandle Engine::createEntity(const std::string &name, MeshHandle mesh, MaterialHandle material, Transform transform)
    {
        const EntityHandle id = handleManager.getNextEntityHandle();

        if (id.index >= MAX_ENTITIES)
        {
            spdlog::warn("Exceeded maximum number of entities.");
            return {};
        }

        auto entity = Entity{.handle = id,
                             .name = name,
                             .mesh = mesh,
                             .material = material,
                             .transform = transform};

        registery.entities.resize(id.index + 1);
        registery.entities[id.index] = entity;

        return id;
    }

    void Engine::updateTransform(EntityHandle entity, Transform transform)
    {
        if (!entity.isValid() || entity.index >= registery.entities.size())
        {
            spdlog::error("Invalid entity handle for updating transform.");
            return;
        }

        registery.entities[entity.index].transform = transform;
    }

    Transform Engine::getTransform(EntityHandle entity)
    {
        if (!entity.isValid() || entity.index >= registery.entities.size())
        {
            spdlog::error("Invalid entity handle for getting transform.");
            return Transform{};
        }

        return registery.entities[entity.index].transform;
    }

}
