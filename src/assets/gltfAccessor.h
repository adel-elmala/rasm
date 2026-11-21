// clang-format off
#pragma once

#include <cstdint>
#include <vector>

namespace tinygltf { class Model; }

namespace rasm
{
    bool    readFloatAccessor(const tinygltf::Model& model, int accessorIndex, std::vector<float>& out, uint32_t& components);
    bool    readIndexAccessor(const tinygltf::Model& model, int accessorIndex, std::vector<uint32_t>& out);
}
// clang-format on
