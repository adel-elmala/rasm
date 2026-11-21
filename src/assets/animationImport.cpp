#include "assets/animationImport.h"
#include "assets/gltfAccessor.h"

#include "spdlog/spdlog.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rasm
{
    namespace
    {
        bool getPath(const std::string &name, AnimationPath &path)
        {
            if (name == "translation")
                path = AnimationPath::TRANSLATION;
            else if (name == "rotation")
                path = AnimationPath::ROTATION;
            else if (name == "scale")
                path = AnimationPath::SCALE;
            else
                return false;
            return true;
        }

        bool getInterpolation(const std::string &name, AnimationInterpolation &interpolation)
        {
            if (name.empty() || name == "LINEAR")
                interpolation = AnimationInterpolation::LINEAR;
            else if (name == "STEP")
                interpolation = AnimationInterpolation::STEP;
            else if (name == "CUBICSPLINE")
                interpolation = AnimationInterpolation::CUBICSPLINE;
            else
                return false;
            return true;
        }

        // Reads a sampler's key times and its values as vec4s (a quaternion's x, y, z, w; w = 0 for a translation or
        // a scale). Returns an empty string on success, else what is wrong.
        std::string readSampler(const tinygltf::Model &model, const tinygltf::AnimationSampler &source, AnimationPath path, AnimationSampler &out)
        {
            if (!getInterpolation(source.interpolation, out.interpolation))
                return "its interpolation " + source.interpolation + " is unknown";

            uint32_t components = 0;
            if (!readFloatAccessor(model, source.input, out.times, components) || components != 1 || out.times.empty())
                return "its key times are missing or malformed";
            for (size_t k = 0; k < out.times.size(); ++k)
            {
                if (!std::isfinite(out.times[k]) || (k > 0 && out.times[k] < out.times[k - 1]))
                    return "its key times do not increase";
            }

            std::vector<float> values;
            const uint32_t expected = path == AnimationPath::ROTATION ? 4 : 3;
            if (!readFloatAccessor(model, source.output, values, components) || components != expected)
                return "its values are missing or have the wrong type";
            const size_t perKey = out.interpolation == AnimationInterpolation::CUBICSPLINE ? 3 : 1;
            if (values.size() != out.times.size() * perKey * components)
                return "it has " + std::to_string(values.size() / components) + " values for " + std::to_string(out.times.size()) + " keys";

            out.values.resize(values.size() / components);
            for (size_t i = 0; i < out.values.size(); ++i)
            {
                const float *v = values.data() + i * components;
                out.values[i] = glm::vec4(v[0], v[1], v[2], components == 4 ? v[3] : 0.0f);
            }
            return {};
        }
    }

    // Keeps the part of the node tree that a clip can move: a node is kept when a clip moves it or one of its parents,
    // and it holds a mesh or is the parent of a kept node. A kept node whose parent is not kept hangs below a node
    // that holds that parent's world matrix, so the GPU never walks static nodes.
    AnimationRaw importAnimations(const tinygltf::Model &model, const GltfNodeWalk &walk, std::vector<SubGLTFRaw> &subMeshes)
    {
        AnimationRaw raw;
        const size_t nodeCount = model.nodes.size();
        if (model.animations.empty() || walk.parents.size() != nodeCount)
            return raw;

        auto isMovable = [&](int node)
        {
            return node >= 0 && static_cast<size_t>(node) < nodeCount && walk.parents[node] != GLTF_NODE_NOT_VISITED;
        };

        std::vector<bool> animated(nodeCount, false);
        for (const auto &animation : model.animations)
        {
            for (const auto &channel : animation.channels)
            {
                AnimationPath path;
                if (!isMovable(channel.target_node) || !getPath(channel.target_path, path))
                    continue;
                if (model.nodes[channel.target_node].matrix.size() == 16)
                {
                    spdlog::warn("glTF: node {} has a matrix, so its animation is ignored (glTF animates translation, rotation and scale only).", channel.target_node);
                    continue;
                }
                animated[channel.target_node] = true;
            }
        }

        std::vector<bool> hasMesh(nodeCount, false);
        for (const auto &sub : subMeshes)
        {
            if (sub.node >= 0 && static_cast<size_t>(sub.node) < nodeCount)
                hasMesh[sub.node] = true;
        }

        std::vector<bool> moved(nodeCount, false);
        for (int node : walk.order)
        {
            const int parent = walk.parents[node];
            moved[node] = animated[node] || (parent >= 0 && moved[parent]);
        }
        std::vector<bool> kept(nodeCount, false);
        for (auto it = walk.order.rbegin(); it != walk.order.rend(); ++it)
        {
            const int node = *it;
            if (hasMesh[node] && moved[node])
                kept[node] = true;
            const int parent = walk.parents[node];
            if (kept[node] && parent >= 0 && moved[parent])
                kept[parent] = true;
        }

        std::vector<int> keptIndex(nodeCount, -1);
        std::unordered_map<int, int> staticParents;
        for (int node : walk.order)
        {
            if (!kept[node])
                continue;
            const int parent = walk.parents[node];
            int keptParent = -1;
            if (parent >= 0 && kept[parent])
            {
                keptParent = keptIndex[parent];
            }
            else if (parent >= 0 && walk.globals[parent] != glm::mat4(1.0f))
            {
                auto found = staticParents.find(parent);
                if (found == staticParents.end())
                {
                    found = staticParents.emplace(parent, static_cast<int>(raw.nodes.size())).first;
                    raw.nodes.push_back({.rest = walk.globals[parent]});
                }
                keptParent = found->second;
            }

            const tinygltf::Node &source = model.nodes[node];
            AnimationNode animationNode{.parent = keptParent, .rest = walk.locals[node]};
            if (source.translation.size() == 3)
                animationNode.translation = glm::vec3(source.translation[0], source.translation[1], source.translation[2]);
            if (source.rotation.size() == 4)
                animationNode.rotation = glm::vec4(source.rotation[0], source.rotation[1], source.rotation[2], source.rotation[3]);
            if (source.scale.size() == 3)
                animationNode.scale = glm::vec3(source.scale[0], source.scale[1], source.scale[2]);
            keptIndex[node] = static_cast<int>(raw.nodes.size());
            raw.nodes.push_back(animationNode);
        }
        if (raw.nodes.empty())
            return raw;

        for (auto &sub : subMeshes)
        {
            if (sub.node >= 0 && static_cast<size_t>(sub.node) < nodeCount)
                sub.animatedNode = keptIndex[sub.node];
        }

        // Every glTF animation becomes a clip with the same index, also one left without channels, so clip indices
        // match the file.
        for (size_t a = 0; a < model.animations.size(); ++a)
        {
            const tinygltf::Animation &animation = model.animations[a];
            AnimationClipRaw clip;
            clip.info.name = animation.name.empty() ? "animation " + std::to_string(a) : animation.name;
            std::vector<int> samplers(animation.samplers.size(), -1);
            for (const auto &channel : animation.channels)
            {
                AnimationPath path;
                if (!isMovable(channel.target_node) || !animated[channel.target_node] || keptIndex[channel.target_node] < 0)
                    continue;
                if (!getPath(channel.target_path, path))
                {
                    spdlog::debug("glTF: clip {} animates {} of node {}, which RASM ignores.", clip.info.name, channel.target_path, channel.target_node);
                    continue;
                }
                if (channel.sampler < 0 || static_cast<size_t>(channel.sampler) >= animation.samplers.size())
                {
                    spdlog::warn("glTF: clip {} has a channel with sampler {} out of range; it is skipped.", clip.info.name, channel.sampler);
                    continue;
                }
                if (samplers[channel.sampler] < 0)
                {
                    AnimationSampler sampler;
                    const std::string error = readSampler(model, animation.samplers[channel.sampler], path, sampler);
                    if (!error.empty())
                    {
                        spdlog::warn("glTF: clip {} skips the {} of node {}: {}.", clip.info.name, channel.target_path, channel.target_node, error);
                        continue;
                    }
                    clip.info.duration = std::max(clip.info.duration, sampler.times.back());
                    samplers[channel.sampler] = static_cast<int>(clip.samplers.size());
                    clip.samplers.push_back(std::move(sampler));
                }
                clip.channels.push_back({.node = static_cast<uint32_t>(keptIndex[channel.target_node]), .path = path, .sampler = static_cast<uint32_t>(samplers[channel.sampler])});
            }
            raw.clips.push_back(std::move(clip));
        }
        return raw;
    }
}
