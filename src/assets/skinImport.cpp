#include "assets/skinImport.h"
#include "assets/gltfAccessor.h"

#include "spdlog/spdlog.h"

#include "tiny_gltf.h"

#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rasm
{
    namespace
    {
        struct Influence
        {
            uint32_t joint = 0;
            float weight = 0.0f;
        };

        // A vertex keeps this many influences, the most the skin stream holds.
        constexpr size_t SKIN_INFLUENCES = 4;
        constexpr uint32_t MAX_SKIN_JOINTS = UINT16_MAX + 1;
        constexpr float WEIGHT_ONE = 65535.0f;
    }

    namespace
    {
        // The 4 largest influences, scaled to add up to 1 and quantized so the UNORM16 weights add up to 65535 exactly:
        // the rounding's rest goes to the largest. A vertex with no weight at all follows joint 0.
        SkinVertex packInfluences(std::vector<Influence> &influences)
        {
            std::sort(influences.begin(), influences.end(), [](const Influence &a, const Influence &b)
                      { return a.weight > b.weight; });
            influences.resize(std::min(influences.size(), SKIN_INFLUENCES));
            float sum = 0.0f;
            for (const Influence &influence : influences)
                sum += influence.weight;

            SkinVertex vertex{};
            if (influences.empty() || sum <= 0.0f)
            {
                vertex.weights[0] = UINT16_MAX;
                return vertex;
            }
            int32_t total = 0;
            for (size_t i = 0; i < influences.size(); ++i)
            {
                vertex.joints[i] = static_cast<uint16_t>(influences[i].joint);
                vertex.weights[i] = static_cast<uint16_t>(std::lround(influences[i].weight / sum * WEIGHT_ONE));
                total += vertex.weights[i];
            }
            vertex.weights[0] = static_cast<uint16_t>(vertex.weights[0] + (UINT16_MAX - total));
            return vertex;
        }
    }

    int getNodeSkin(const tinygltf::Model &model, int node)
    {
        if (node < 0 || static_cast<size_t>(node) >= model.nodes.size())
            return -1;
        const int skin = model.nodes[node].skin;
        return skin >= 0 && static_cast<size_t>(skin) < model.skins.size() ? skin : -1;
    }

    // JOINTS_n and WEIGHTS_n come in pairs of 4 influences each. A vertex keeps its 4 largest (one warning per
    // primitive that has more), and a joint index past the skin's joints loses its weight.
    bool readSkinVertices(const tinygltf::Model &model, const tinygltf::Primitive &primitive, size_t vertexCount, uint32_t jointCount, const std::string &primitiveName,
                          std::vector<SkinVertex> &out)
    {
        if (jointCount == 0 || jointCount > MAX_SKIN_JOINTS)
        {
            spdlog::warn("glTF {}: its skin has {} joints (1 to {} are drawn), so it is not skinned.", primitiveName, jointCount, MAX_SKIN_JOINTS);
            return false;
        }
        std::vector<std::vector<float>> joints;
        std::vector<std::vector<float>> weights;
        for (uint32_t set = 0;; ++set)
        {
            const auto jointsIt = primitive.attributes.find("JOINTS_" + std::to_string(set));
            const auto weightsIt = primitive.attributes.find("WEIGHTS_" + std::to_string(set));
            if (jointsIt == primitive.attributes.end() || weightsIt == primitive.attributes.end())
                break;
            std::vector<float> setJoints;
            std::vector<float> setWeights;
            uint32_t jointComponents = 0;
            uint32_t weightComponents = 0;
            if (!readFloatAccessor(model, jointsIt->second, setJoints, jointComponents) || !readFloatAccessor(model, weightsIt->second, setWeights, weightComponents) ||
                jointComponents != 4 || weightComponents != 4 || setJoints.size() != vertexCount * 4 || setWeights.size() != vertexCount * 4)
            {
                spdlog::warn("glTF {}: JOINTS_{} or WEIGHTS_{} is missing or malformed, so it is not skinned.", primitiveName, set, set);
                return false;
            }
            joints.push_back(std::move(setJoints));
            weights.push_back(std::move(setWeights));
        }
        if (joints.empty())
            return false;

        out.resize(vertexCount);
        size_t dropped = 0;
        size_t outOfRange = 0;
        std::vector<Influence> influences;
        for (size_t v = 0; v < vertexCount; ++v)
        {
            influences.clear();
            for (size_t set = 0; set < joints.size(); ++set)
            {
                for (size_t c = 0; c < 4; ++c)
                {
                    const float weight = weights[set][v * 4 + c];
                    const float joint = joints[set][v * 4 + c];
                    if (!(weight > 0.0f) || !std::isfinite(weight))
                        continue;
                    if (!(joint >= 0.0f) || joint >= static_cast<float>(jointCount))
                    {
                        ++outOfRange;
                        continue;
                    }
                    influences.push_back({.joint = static_cast<uint32_t>(joint), .weight = weight});
                }
            }
            if (influences.size() > SKIN_INFLUENCES)
                ++dropped;
            out[v] = packInfluences(influences);
        }
        if (dropped > 0)
            spdlog::warn("glTF {}: {} vertices have more than {} joint weights; each keeps its {} largest.", primitiveName, dropped, SKIN_INFLUENCES, SKIN_INFLUENCES);
        if (outOfRange > 0)
            spdlog::warn("glTF {}: {} joint weights name a joint past the skin's {}; they are dropped.", primitiveName, outOfRange, jointCount);
        return true;
    }

    // Per joint, the sphere around the bind-pose vertices it moves: the center of their box, the radius to the
    // farthest. A skinned vertex is a weighted mix of its joints' matrices applied to it, so each moved sphere holds one
    // point of the mix, and any sphere around all the moved spheres holds the vertex.
    std::vector<glm::vec4> getJointSpheres(const std::vector<Vertex> &vertices, const std::vector<SkinVertex> &skin, uint32_t jointCount)
    {
        std::vector<glm::vec3> minimum(jointCount, glm::vec3(INFINITY));
        std::vector<glm::vec3> maximum(jointCount, glm::vec3(-INFINITY));
        for (size_t v = 0; v < skin.size() && v < vertices.size(); ++v)
        {
            for (size_t i = 0; i < 4; ++i)
            {
                if (skin[v].weights[i] == 0)
                    continue;
                const uint16_t joint = skin[v].joints[i];
                minimum[joint] = glm::min(minimum[joint], vertices[v].pos);
                maximum[joint] = glm::max(maximum[joint], vertices[v].pos);
            }
        }
        std::vector<glm::vec4> spheres(jointCount, glm::vec4(0.0f, 0.0f, 0.0f, -1.0f));
        for (uint32_t j = 0; j < jointCount; ++j)
        {
            if (minimum[j].x <= maximum[j].x)
                spheres[j] = glm::vec4((minimum[j] + maximum[j]) * 0.5f, 0.0f);
        }
        for (size_t v = 0; v < skin.size() && v < vertices.size(); ++v)
        {
            for (size_t i = 0; i < 4; ++i)
            {
                if (skin[v].weights[i] == 0)
                    continue;
                glm::vec4 &sphere = spheres[skin[v].joints[i]];
                sphere.w = std::max(sphere.w, glm::length(vertices[v].pos - glm::vec3(sphere)));
            }
        }
        return spheres;
    }

    // Each skin a sub-mesh uses gets its joints once in raw.joints: their kept nodes and inverse bind matrices
    // (identity without the accessor). A skin with a joint outside the kept nodes leaves its sub-meshes unskinned.
    void importSkinJoints(const tinygltf::Model &model, const std::vector<int> &keptNodes, std::vector<SubGLTFRaw> &subMeshes, AnimationRaw &raw)
    {
        std::unordered_map<int, int64_t> firstJoints;
        for (SubGLTFRaw &sub : subMeshes)
        {
            if (sub.skin < 0 || sub.objRaw.skin.empty())
                continue;
            auto found = firstJoints.find(sub.skin);
            if (found == firstJoints.end())
            {
                const tinygltf::Skin &skin = model.skins[sub.skin];
                std::vector<float> inverseBinds;
                uint32_t components = 16;
                bool valid = skin.inverseBindMatrices < 0 ||
                             (readFloatAccessor(model, skin.inverseBindMatrices, inverseBinds, components) && components == 16 && inverseBinds.size() >= skin.joints.size() * 16);
                if (!valid)
                    spdlog::warn("glTF skin {}: its inverse bind matrices are missing or malformed, so it is not skinned.", sub.skin);
                std::vector<AnimationJoint> joints;
                for (size_t j = 0; valid && j < skin.joints.size(); ++j)
                {
                    const int node = skin.joints[j];
                    if (node < 0 || static_cast<size_t>(node) >= keptNodes.size() || keptNodes[node] < 0)
                    {
                        spdlog::warn("glTF skin {}: joint {} (node {}) is not in the scene, so it is not skinned.", sub.skin, j, node);
                        valid = false;
                        break;
                    }
                    AnimationJoint joint{.node = static_cast<uint32_t>(keptNodes[node])};
                    if (!inverseBinds.empty())
                        joint.inverseBind = glm::make_mat4(inverseBinds.data() + j * 16);
                    joints.push_back(joint);
                }
                found = firstJoints.emplace(sub.skin, valid ? static_cast<int64_t>(raw.joints.size()) : -1).first;
                if (valid)
                    raw.joints.insert(raw.joints.end(), joints.begin(), joints.end());
            }
            if (found->second < 0)
                continue;
            sub.firstJoint = static_cast<uint32_t>(found->second);
            sub.jointCount = static_cast<uint32_t>(model.skins[sub.skin].joints.size());
        }
    }
}
