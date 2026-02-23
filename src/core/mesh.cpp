#include "rasm/core/mesh.h"

namespace rasm
{
    Mesh::Mesh() {}
    
    Mesh::Mesh(MeshHandle meshHandle) : handle(meshHandle) {}

    Mesh::~Mesh() {}

    MeshHandle Mesh::id() const {
        return handle;
    }
}
