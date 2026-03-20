// clang-format off
#pragma once

#include <any>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include "rasm/core/rasm.h"

namespace rasm
{

    class Entity
    {
    public:
                            Entity();
        explicit            Entity(EntityHandle entityHandle);
        explicit            Entity(EntityHandle entityHandle, const std::string &name);
                            ~Entity();
        [[nodiscard]]       EntityHandle id() const;
        [[nodiscard]] bool  isValid() const;

        template <typename T, typename... Args>
        T &addComponent(Args &&...args)
        {
            static_assert(!std::is_pointer_v<T>, "Components must be value types, not raw pointers.");
            static_assert(!std::is_reference_v<T>, "Components must be value types, not references.");
            if (!storage)
            {
                storage = std::make_shared<ComponentStorage>();
            }

            const std::type_index key(typeid(T));
            storage->components[key] = T(std::forward<Args>(args)...);
            return std::any_cast<T &>(storage->components[key]);
        }

        template <typename T>
        [[nodiscard]] bool hasComponent() const
        {
            if (!storage)
            {
                return false;
            }
            const std::type_index key(typeid(T));
            return storage->components.find(key) != storage->components.end();
        }

        template <typename T>
        T &getComponent()
        {
            if (!storage)
            {
                throw std::runtime_error("Entity has no component storage.");
            }
            const std::type_index key(typeid(T));
            auto it = storage->components.find(key);
            if (it == storage->components.end())
            {
                throw std::runtime_error("Requested component is not attached to this entity.");
            }
            return std::any_cast<T &>(it->second);
        }

    private:

        // `storage` is a std::shared_ptr<ComponentStorage>. This means that copying an
        // Entity instance will cause the original and the copy to share the same
        // underlying ComponentStorage (and thus the same components). This shared
        // ownership is intentional: Entity is a lightweight handle to component
        // storage, not a deep-value object.
        struct ComponentStorage
        {
            std::unordered_map<std::type_index, std::any> components;
        };
        std::shared_ptr<ComponentStorage> storage;
        EntityHandle                      handle{};
        std::string                       name;

    };

}
// clang-format on