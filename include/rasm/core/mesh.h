#pragma once

#include <stdint.h>

namespace rasm {

    class Mesh
    {
    private:
        uint32_t handle;
    public:
        Mesh();
        ~Mesh();
    };

}