#pragma once

#include <string>

#include "rasm/core/entity.h"

namespace rasm {

    class Scene
    {
    private:
    public:
        Scene();
        ~Scene();

        Entity createEntity(const std::string& name);

    };

}