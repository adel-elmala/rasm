#include "rasm/core/entity.h"

namespace rasm
{

    Entity::Entity()
        : storage(std::make_shared<ComponentStorage>()) {}

    Entity::Entity(EntityHandle entityHandle)
        : handle(entityHandle), storage(std::make_shared<ComponentStorage>()) {}

    Entity::Entity(EntityHandle entityHandle, const std::string &name)
        : handle(entityHandle), name(name), storage(std::make_shared<ComponentStorage>()) {}
    
    Entity::~Entity() {}

    EntityHandle Entity::id() const {
        return handle;
    }

    bool Entity::isValid() const {
        return handle.isValid();
    }

}
