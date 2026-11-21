// clang-format off
#pragma once

#include "assets/meshImport.h"

#include <cstdint>
#include <string>

rasm::GLTFRaw   importMeshText(const std::string& contents, const std::string& extension, uint64_t supportedTextureFormats = 0);
// clang-format on
