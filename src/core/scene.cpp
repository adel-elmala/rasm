#include "rasm/core/scene.h"
#include "rasm/core/engine.h"

namespace rasm
{

    Scene::Scene() {}
    Scene::Scene(Engine* owner, SceneHandle sceneHandle)
        : engine(owner), handle(sceneHandle) {}
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
        auto entity = engine->createEntity(*this, name);
        entities.push_back(entity);

        return entity;
    }

}
