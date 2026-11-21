// clang-format off
#pragma once

#include "assets/meshImport.h"

#include <cstdint>
#include <string>
#include <vector>

rasm::GLTFRaw   importMeshText(const std::string& contents, const std::string& extension, uint64_t supportedTextureFormats = 0);
std::string     addView(std::vector<unsigned char>& buffer, const void* data, size_t size);
std::string     base64(const std::vector<unsigned char>& bytes);
rasm::GLTFRaw   importGltf(const std::vector<unsigned char>& buffer, const std::string& views, const std::string& rest);
// clang-format on
