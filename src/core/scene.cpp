#include "rasm/core/scene.h"
#include "rasm/core/engine.h"

namespace rasm
{

    SceneHandle::SceneHandle() {}
    SceneHandle::SceneHandle(Engine* owner, SceneId sceneHandle)
        : engine(owner), handle(sceneHandle) {}
    SceneHandle::~SceneHandle() {}

    bool SceneHandle::isValid() const {
        return handle.isValid();
    }

    SceneId SceneHandle::id() const {
        return handle;
    }

    Entity SceneHandle::createEntity(const std::string &name)
    {
        if (engine == nullptr) {
            return Entity();
        }
        return engine->createEntity(*this, name);
    }

}
