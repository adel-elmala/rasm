// clang-format off
#pragma once

#include "rasm/animation.h"
#include "assets/meshData.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinygltf { class Model; struct Primitive; }

namespace rasm
{

    MorphDelta                          packMorphDelta(const glm::vec3& position, const glm::vec3& normal, const glm::vec3& tangent);
    glm::vec3                           getMorphNormal(const MorphDelta& delta);
    glm::vec3                           getMorphTangent(const MorphDelta& delta);
    uint32_t                            getMorphTargetCount(const tinygltf::Model& model, int mesh);
    uint32_t                            readMorphTargets(const tinygltf::Model& model, const tinygltf::Primitive& primitive, size_t vertexCount,
                                                         const std::string& primitiveName, std::vector<MorphDelta>& out);
    std::unordered_map<int, uint32_t>   importMorphWeights(const tinygltf::Model& model, std::vector<SubGLTFRaw>& subMeshes, AnimationRaw& raw);

}
// clang-format on
