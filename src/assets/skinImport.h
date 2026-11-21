// clang-format off
#pragma once

#include "rasm/animation.h"
#include "rasm/mesh.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tinygltf { class Model; struct Primitive; }

namespace rasm
{

    int                     getNodeSkin(const tinygltf::Model& model, int node);
    bool                    readSkinVertices(const tinygltf::Model& model, const tinygltf::Primitive& primitive, size_t vertexCount, uint32_t jointCount,
                                             const std::string& primitiveName, std::vector<SkinVertex>& out);
    std::vector<glm::vec4>  getJointSpheres(const std::vector<Vertex>& vertices, const std::vector<SkinVertex>& skin, uint32_t jointCount);
    void                    importSkinJoints(const tinygltf::Model& model, const std::vector<int>& keptNodes, std::vector<SubGLTFRaw>& subMeshes, AnimationRaw& raw);

}
// clang-format on
