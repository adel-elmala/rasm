// clang-format off
#pragma once

#include "rasm/core/types.h"
#include "rasm/core/transform.h"

#include <string>

namespace rasm
{

    constexpr uint32_t MAX_ENTITIES = 1024;

    // an entity represent a model in the scene, containing references to its mesh and material.
    struct Entity
    {
        EntityHandle handle{};
        std::string name;
        MeshHandle mesh{};
        MaterialHandle material{};
        Transform transform{};
    };

}
// clang-format on