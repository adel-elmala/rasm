// clang-format off
#pragma once

#include "rasm/animation.h"
#include "rasm/mesh.h"

#include <vector>

namespace tinygltf { class Model; }

namespace rasm
{

    constexpr int GLTF_NODE_NOT_VISITED = -2;

    struct GltfNodeWalk
    {
        std::vector<int>            order;          // the visited nodes, parents before children
        std::vector<int>            parents;        // per glTF node: its parent, -1 for a root, GLTF_NODE_NOT_VISITED
        std::vector<glm::mat4>      locals;
        std::vector<glm::mat4>      globals;
    };

    AnimationRaw    importAnimations(const tinygltf::Model& model, const GltfNodeWalk& walk, std::vector<SubGLTFRaw>& subMeshes);

}
// clang-format on
