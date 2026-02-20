#pragma once

#include <string>

#include "rasm/core/rasm.h"
#include "rasm/core/entity.h"

namespace rasm {
    class Engine;

    class Scene
    {
    private:
        Engine* engine = nullptr;
        SceneHandle handle{};

    public:
        Scene();
        Scene(Engine* owner, SceneHandle Scene);
        ~Scene();

        [[nodiscard]] bool isValid() const;
        [[nodiscard]] SceneHandle id() const;
        Entity createEntity(const std::string& name);
    };

}
