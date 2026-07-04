// clang-format off
#pragma once

#include <string>
#include <unordered_set>

#include "rasm/core/entity.h"

namespace rasm {
    class Engine;

    class Scene
    {
    friend class Engine;
    public:
                                    Scene();
                                    Scene(Engine* owner, SceneHandle sceneHandle);
                                    ~Scene();
        Entity                      createEntity(const std::string& name);
        [[nodiscard]] bool          isValid() const;
        [[nodiscard]] SceneHandle   id() const;

    protected:
        SceneHandle         handle{};
        std::unordered_map<EntityHandle, Entity, HandleHash> entities;
        Engine*             engine = nullptr;
    };

}
// clang-format on