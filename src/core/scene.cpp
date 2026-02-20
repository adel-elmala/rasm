#include "rasm/core/scene.h"
#include "rasm/core/engine.h"

namespace rasm
{

    Scene::Scene() {}
    Scene::Scene(Engine* owner, SceneHandle Scene)
        : engine(owner), handle(Scene) {}
    Scene::~Scene() {}

    bool Scene::isValid() const {
        return handle.isValid();
    }

    SceneHandle Scene::id() const {
        return handle;
    }

    Entity Scene::createEntity(const std::string &name)
    {
        if (engine == nullptr) {
            return Entity();
        }
        return engine->createEntity(*this, name);
    }

}
