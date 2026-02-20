#pragma once

#include <string>

#include "rasm/core/rasm.h"
#include "rasm/core/entity.h"

namespace rasm {
    class Engine;

    class SceneHandle
    {
    private:
        Engine* engine = nullptr;
        SceneId handle{};

    public:
        SceneHandle();
        SceneHandle(Engine* owner, SceneId sceneHandle);
        ~SceneHandle();

        [[nodiscard]] bool isValid() const;
        [[nodiscard]] SceneId id() const;
        Entity createEntity(const std::string& name);
    };

}
