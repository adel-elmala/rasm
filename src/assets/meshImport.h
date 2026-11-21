// clang-format off
#pragma once

#include "rasm/mesh.h"
#include "assets/ktxImage.h"

#include <cstdint>
#include <string>

namespace rasm
{
    GLTFRaw     importMeshFile(const std::string& path, uint64_t supportedTextureFormats);
    int         getBasisuSource(const tinygltf::Model& model, int textureIndex);
    uint64_t    getKtxSlotKey(int image, KtxSlot slot);
    uint64_t    getKtxImageKey(int image, Format format);
}
// clang-format on
