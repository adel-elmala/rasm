#pragma once

#include "rasm/core/rasm.h"

namespace rasm {

    struct Mesh
    {
        MeshHandle handle{};

        Mesh() = default;
        explicit Mesh(MeshHandle meshHandle) : handle(meshHandle) {}
    };

}
