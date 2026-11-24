#include "assets/morphImport.h"
#include "assets/gltfAccessor.h"

#include "spdlog/spdlog.h"

#include "tiny_gltf.h"

#include "glm/gtc/packing.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace rasm
{
    namespace
    {
        // The attributes a target can move, in the order of MorphDelta's parts.
        const char *const MORPH_ATTRIBUTES[] = {"POSITION", "NORMAL", "TANGENT"};
    }

    namespace
    {
        // A target's attribute as one vec3 per vertex (TANGENT deltas are xyz), or false when it is missing or malformed.
        bool readTargetAttribute(const tinygltf::Model &model, const std::map<std::string, int> &target, const char *name, size_t vertexCount,
                                 const std::string &primitiveName, size_t targetIndex, std::vector<glm::vec3> &out)
        {
            const auto it = target.find(name);
            if (it == target.end())
                return false;
            std::vector<float> values;
            uint32_t components = 0;
            if (!readFloatAccessor(model, it->second, values, components) || components != 3 || values.size() != vertexCount * 3)
            {
                spdlog::warn("glTF {}: ignoring the invalid {} of morph target {}.", primitiveName, name, targetIndex);
                return false;
            }
            out.resize(vertexCount);
            for (size_t v = 0; v < vertexCount; ++v)
                out[v] = glm::vec3(values[v * 3], values[v * 3 + 1], values[v * 3 + 2]);
            return true;
        }
    }

    // Positions stay floats: a half float keeps 11 bits, too few for a large mesh's small moves. Normal and tangent
    // deltas are at most 2 long and are renormalized after the mix, so halves are enough for them.
    MorphDelta packMorphDelta(const glm::vec3 &position, const glm::vec3 &normal, const glm::vec3 &tangent)
    {
        return {.position = position,
                .normalTangent = {glm::packHalf2x16(glm::vec2(normal.x, normal.y)), glm::packHalf2x16(glm::vec2(normal.z, tangent.x)),
                                  glm::packHalf2x16(glm::vec2(tangent.y, tangent.z))}};
    }

    glm::vec3 getMorphNormal(const MorphDelta &delta)
    {
        const glm::vec2 xy = glm::unpackHalf2x16(delta.normalTangent[0]);
        return glm::vec3(xy, glm::unpackHalf2x16(delta.normalTangent[1]).x);
    }

    glm::vec3 getMorphTangent(const MorphDelta &delta)
    {
        return glm::vec3(glm::unpackHalf2x16(delta.normalTangent[1]).y, glm::unpackHalf2x16(delta.normalTangent[2]));
    }

    // glTF asks every primitive of a mesh for the same number of targets; the largest is taken.
    uint32_t getMorphTargetCount(const tinygltf::Model &model, int mesh)
    {
        if (mesh < 0 || static_cast<size_t>(mesh) >= model.meshes.size())
            return 0;
        size_t count = 0;
        for (const tinygltf::Primitive &primitive : model.meshes[mesh].primitives)
            count = std::max(count, primitive.targets.size());
        return static_cast<uint32_t>(count);
    }

    // The primitive's morph targets, target-major (ObjRaw::morphs), with sparse accessors already applied. A missing or
    // malformed attribute of a target moves nothing. Returns the target count, 0 when no target moves any vertex.
    uint32_t readMorphTargets(const tinygltf::Model &model, const tinygltf::Primitive &primitive, size_t vertexCount, const std::string &primitiveName,
                              std::vector<MorphDelta> &out)
    {
        out.clear();
        if (primitive.targets.empty())
            return 0;
        const size_t targetCount = std::min<size_t>(primitive.targets.size(), MAX_MORPH_TARGETS);
        if (primitive.targets.size() > MAX_MORPH_TARGETS)
            spdlog::warn("glTF {}: {} morph targets, the first {} are kept.", primitiveName, primitive.targets.size(), MAX_MORPH_TARGETS);

        out.assign(targetCount * vertexCount, packMorphDelta(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)));
        bool moves = false;
        for (size_t t = 0; t < targetCount; ++t)
        {
            std::vector<glm::vec3> attributes[3];
            for (size_t a = 0; a < 3; ++a)
                readTargetAttribute(model, primitive.targets[t], MORPH_ATTRIBUTES[a], vertexCount, primitiveName, t, attributes[a]);
            for (size_t v = 0; v < vertexCount; ++v)
            {
                glm::vec3 delta[3] = {glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)};
                for (size_t a = 0; a < 3; ++a)
                {
                    if (!attributes[a].empty())
                        delta[a] = attributes[a][v];
                    moves = moves || delta[a] != glm::vec3(0.0f);
                }
                out[t * vertexCount + v] = packMorphDelta(delta[0], delta[1], delta[2]);
            }
        }
        if (!moves)
        {
            out.clear();
            return 0;
        }
        return static_cast<uint32_t>(targetCount);
    }

    // Each node that draws a morphed mesh gets its weights once in raw.morphWeights, at rest: the node's weights, else
    // the mesh's, else 0 (glTF). Its primitives use them from sub.firstWeight on. The names come from the mesh's
    // extras.targetNames when it has one per target. Returns each such glTF node's first weight.
    std::unordered_map<int, uint32_t> importMorphWeights(const tinygltf::Model &model, std::vector<SubGLTFRaw> &subMeshes, AnimationRaw &raw)
    {
        std::unordered_map<int, uint32_t> firstWeights;
        for (SubGLTFRaw &sub : subMeshes)
        {
            if (sub.objRaw.morphTargetCount == 0 || sub.node < 0 || static_cast<size_t>(sub.node) >= model.nodes.size())
                continue;
            auto found = firstWeights.find(sub.node);
            if (found == firstWeights.end())
            {
                const tinygltf::Node &node = model.nodes[sub.node];
                const tinygltf::Mesh &mesh = model.meshes[node.mesh];
                const uint32_t count = getMorphTargetCount(model, node.mesh);
                const std::vector<double> *weights = node.weights.size() == count ? &node.weights : mesh.weights.size() == count ? &mesh.weights
                                                                                                                                 : nullptr;
                if (!weights && (!node.weights.empty() || !mesh.weights.empty()))
                    spdlog::warn("glTF node {}: its weights do not match its {} morph targets, so they start at 0.", sub.node, count);
                const tinygltf::Value names = mesh.extras.IsObject() && mesh.extras.Has("targetNames") ? mesh.extras.Get("targetNames") : tinygltf::Value();
                const bool named = names.IsArray() && names.ArrayLen() == count;
                found = firstWeights.emplace(sub.node, static_cast<uint32_t>(raw.morphWeights.size())).first;
                for (uint32_t t = 0; t < count; ++t)
                {
                    raw.morphWeights.push_back(weights ? static_cast<float>((*weights)[t]) : 0.0f);
                    const bool hasName = named && names.Get(static_cast<int>(t)).IsString();
                    raw.morphTargetNames.push_back(hasName ? names.Get(static_cast<int>(t)).Get<std::string>() : "target " + std::to_string(raw.morphTargetNames.size()));
                }
            }
            sub.firstWeight = static_cast<int32_t>(found->second);
        }
        return firstWeights;
    }
}
