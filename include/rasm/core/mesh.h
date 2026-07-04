// clang-format off
#pragma once

#include "rasm/core/types.h"

namespace rasm
{

    class Mesh
    {
    public:
                        Mesh();
        explicit        Mesh(MeshHandle meshHandle);
                        ~Mesh();
        [[nodiscard]]   MeshHandle id() const;

    private:
        MeshHandle handle{};
    };

}
// clang-format on