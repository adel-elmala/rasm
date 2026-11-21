// clang-format off
#pragma once

#include "rasm/mesh.h"

#include <cstdint>
#include <string>

namespace rasm
{
    GLTFRaw     importMeshFile(const std::string& path, uint64_t supportedTextureFormats);
    int         getBasisuSource(const tinygltf::Model& model, int textureIndex);
}
// clang-format on
