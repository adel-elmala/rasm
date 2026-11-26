#include "renderer/animation.h"
#include "rasm/renderer.h"
#include "assets/meshData.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/animationTime.h"
#include "renderer/geometryPool.h"
#include "renderer/gpuScene.h"
#include "renderer/renderSpace.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace rasm
{
    namespace
    {
        // Byte offsets of a mesh's parts in the animation pool, each 16-byte aligned.
        struct PoolLayout
        {
            uint64_t nodes = 0;
            uint64_t samplers = 0;
            uint64_t channels = 0;
            uint64_t subMeshes = 0;
            uint64_t keys = 0;
            uint64_t joints = 0;
            uint64_t spheres = 0;
            uint64_t weights = 0;
            uint64_t radii = 0;
            uint64_t size = 0;
            uint32_t samplerCount = 0;
        };

        constexpr uint32_t FIRST_TRANSFORM_CAPACITY = 64;
        constexpr uint32_t FIRST_POSE_CAPACITY = 256;
        constexpr uint32_t FIRST_JOINT_CAPACITY = 256;
        constexpr uint32_t FIRST_SKINNED_CAPACITY = 4096;
        // A joint matrix, three rows.
        constexpr uint64_t JOINT_BYTES = 3 * sizeof(glm::vec4);
        // The Skin pass's group: 64 of an entity's skinned vertices.
        constexpr uint32_t SKIN_GROUP_SIZE = 64;
        // The Skin pass dispatches 64 vertices of an entity per group in y.
        constexpr uint32_t MAX_SKIN_GROUPS = 65535;
        // A node's local and world matrices, three rows each.
        constexpr uint64_t POSE_BYTES_PER_NODE = 6 * sizeof(glm::vec4);
        // The Animate pass dispatches a group per table row in x only.
        constexpr uint32_t MAX_ANIMATE_GROUPS = 65535;
        // A morph weight that setMorphWeights does not set: the clip's, or the rest weight, counts. A quiet NaN, which
        // the Animate pass compares by its bits (animationTypes.slang), so no fast-math rule can drop the test.
        constexpr uint32_t NO_WEIGHT_OVERRIDE = 0x7FC00000;
    }

    namespace
    {
        uint64_t alignTo16(uint64_t value)
        {
            return (value + 15) & ~uint64_t{15};
        }

        uint32_t alignToSkinGroup(uint32_t value)
        {
            return (value + SKIN_GROUP_SIZE - 1) / SKIN_GROUP_SIZE * SKIN_GROUP_SIZE;
        }

        // Per clip and the two rows after them: three channels per node, then one per morph weight.
        uint64_t getChannelRow(const AnimationRaw &animation)
        {
            return animation.nodes.size() * 3 + animation.morphWeights.size();
        }

        PoolLayout getPoolLayout(const AnimationRaw &animation, uint32_t subMeshCount, uint32_t sphereCount, uint32_t radiusCount)
        {
            PoolLayout layout;
            uint64_t keyFloats = 0;
            for (const auto &clip : animation.clips)
            {
                layout.samplerCount += static_cast<uint32_t>(clip.samplers.size());
                for (const auto &sampler : clip.samplers)
                    keyFloats += sampler.times.size() + sampler.values.size() * 4;
            }
            const uint64_t nodeCount = animation.nodes.size();
            layout.nodes = alignTo16(sizeof(gfx::AnimationMeshData));
            layout.samplers = alignTo16(layout.nodes + nodeCount * sizeof(gfx::AnimationNodeData));
            layout.channels = alignTo16(layout.samplers + layout.samplerCount * sizeof(gfx::AnimationSamplerData));
            layout.subMeshes = alignTo16(layout.channels + (animation.clips.size() + 2) * getChannelRow(animation) * sizeof(int32_t));
            layout.keys = alignTo16(layout.subMeshes + subMeshCount * sizeof(gfx::AnimationSubMeshData));
            layout.joints = alignTo16(layout.keys + keyFloats * sizeof(float));
            layout.spheres = alignTo16(layout.joints + animation.joints.size() * sizeof(gfx::AnimationJointData));
            layout.weights = alignTo16(layout.spheres + sphereCount * sizeof(glm::vec4));
            layout.radii = alignTo16(layout.weights + animation.morphWeights.size() * sizeof(float));
            layout.size = alignTo16(layout.radii + radiusCount * sizeof(float));
            return layout;
        }

        void getRows(const glm::mat4 &matrix, glm::vec4 *rows)
        {
            const glm::mat4 transposed = glm::transpose(matrix);
            for (int i = 0; i < 3; ++i)
                rows[i] = transposed[i];
        }

        // A transform the Animate pass has not written yet (its pipeline failed to compile) holds zeros, so its
        // sub-mesh collapses to a point instead of reading old memory.
        bool clearBuffer(RenderContext &ctx, const BufferHandle &buffer, uint64_t size)
        {
            const std::vector<uint8_t> zeros(size, 0);
            return ctx.fillBuffer(buffer, zeros.data(), zeros.size());
        }

        // A scale key at or below 0 on any axis, or a cubic curve that dips there between two keys, can turn the
        // node's matrix inside out (a negative determinant), which turns its triangles' winding around.
        bool canMirror(const AnimationSampler &sampler)
        {
            const bool cubic = sampler.interpolation == AnimationInterpolation::CUBICSPLINE;
            const size_t keys = sampler.times.size();
            for (size_t k = 0; k < keys; ++k)
            {
                const glm::vec3 value = glm::vec3(sampler.values[cubic ? k * 3 + 1 : k]);
                if (glm::any(glm::lessThanEqual(value, glm::vec3(0.0f))))
                    return true;
                if (!cubic || k + 1 == keys)
                    continue;
                const float span = sampler.times[k + 1] - sampler.times[k];
                const glm::vec3 out = glm::vec3(sampler.values[k * 3 + 2]) * span;
                const glm::vec3 next = glm::vec3(sampler.values[k * 3 + 4]);
                const glm::vec3 in = glm::vec3(sampler.values[k * 3 + 3]) * span;
                for (int step = 1; step < 16; ++step)
                {
                    const float s = static_cast<float>(step) / 16.0f;
                    const float s2 = s * s;
                    const float s3 = s2 * s;
                    const glm::vec3 point = (2.0f * s3 - 3.0f * s2 + 1.0f) * value + (s3 - 2.0f * s2 + s) * out + (-2.0f * s3 + 3.0f * s2) * next + (s3 - s2) * in;
                    if (glm::any(glm::lessThanEqual(point, glm::vec3(0.0f))))
                        return true;
                }
            }
            return false;
        }

        // Per kept node: 1 when a clip can mirror it or a node above it.
        std::vector<uint8_t> getMirroringNodes(const AnimationRaw &animation)
        {
            std::vector<uint8_t> mirrors(animation.nodes.size(), 0);
            for (const AnimationClipRaw &clip : animation.clips)
            {
                for (const AnimationChannel &channel : clip.channels)
                {
                    if (channel.path == AnimationPath::SCALE && canMirror(clip.samplers[channel.sampler]))
                        mirrors[channel.node] = 1;
                }
            }
            for (size_t n = 0; n < animation.nodes.size(); ++n)
            {
                const int32_t parent = animation.nodes[n].parent;
                if (parent >= 0 && mirrors[parent])
                    mirrors[n] = 1;
            }
            return mirrors;
        }

        bool isAnimated(const SubGLTFRaw &sub)
        {
            return sub.animatedNode >= 0 || sub.jointCount > 0;
        }

        // The Skin pass writes its vertices: its joints or its morph targets move them.
        bool isPosed(const SubGLTFRaw &sub)
        {
            return sub.jointCount > 0 || sub.objRaw.morphTargetCount > 0;
        }

        uint32_t countAnimatedSubMeshes(const GLTFRaw &raw)
        {
            if (raw.animation.nodes.empty())
                return 0;
            return static_cast<uint32_t>(std::count_if(raw.subGltfRaws.begin(), raw.subGltfRaws.end(), isAnimated));
        }

        uint32_t countJointSpheres(const GLTFRaw &raw)
        {
            uint32_t count = 0;
            for (const SubGLTFRaw &sub : raw.subGltfRaws)
                count += sub.jointCount;
            return count;
        }

        uint32_t countMorphRadii(const GLTFRaw &raw)
        {
            uint32_t count = 0;
            for (const SubGLTFRaw &sub : raw.subGltfRaws)
                count += sub.objRaw.morphTargetCount;
            return count;
        }
    }

    uint64_t getAnimationPoolBytes(const AnimationRaw &animation, uint32_t subMeshCount, uint32_t sphereCount, uint32_t radiusCount)
    {
        if (animation.nodes.empty() || subMeshCount == 0)
            return 0;
        return getPoolLayout(animation, subMeshCount, sphereCount, radiusCount).size;
    }

    std::vector<uint8_t> buildAnimationPoolData(const AnimationRaw &animation, const gfx::AnimationSubMeshData *subMeshes, uint32_t subMeshCount, const glm::vec4 *spheres,
                                                uint32_t sphereCount, const float *morphRadii, uint32_t radiusCount, uint64_t address)
    {
        const PoolLayout layout = getPoolLayout(animation, subMeshCount, sphereCount, radiusCount);
        const auto nodeCount = static_cast<uint32_t>(animation.nodes.size());
        std::vector<uint8_t> bytes(layout.size, 0);

        uint32_t skinnedVertexCount = 0;
        for (uint32_t k = 0; k < subMeshCount; ++k)
        {
            if (subMeshes[k].vertexCount > 0)
                skinnedVertexCount = std::max(skinnedVertexCount, subMeshes[k].firstSkinnedVertex + subMeshes[k].vertexCount);
        }
        const gfx::AnimationMeshData header{.nodes = address + layout.nodes,
                                            .samplers = address + layout.samplers,
                                            .channels = address + layout.channels,
                                            .subMeshes = address + layout.subMeshes,
                                            .keys = address + layout.keys,
                                            .joints = address + layout.joints,
                                            .jointSpheres = address + layout.spheres,
                                            .weights = address + layout.weights,
                                            .morphRadii = address + layout.radii,
                                            .nodeCount = nodeCount,
                                            .subMeshCount = subMeshCount,
                                            .jointCount = static_cast<uint32_t>(animation.joints.size()),
                                            .skinnedVertexCount = skinnedVertexCount,
                                            .weightCount = static_cast<uint32_t>(animation.morphWeights.size()),
                                            .padding = 0};
        std::memcpy(bytes.data(), &header, sizeof(header));

        for (uint32_t n = 0; n < nodeCount; ++n)
        {
            const AnimationNode &node = animation.nodes[n];
            gfx::AnimationNodeData data{.translation = node.translation, .parent = node.parent, .rotation = node.rotation, .scale = node.scale, .padding = 0};
            getRows(node.rest, data.rest);
            std::memcpy(bytes.data() + layout.nodes + n * sizeof(data), &data, sizeof(data));
        }

        // A row of channels per clip, then one with every clip's channels for ALL_ANIMATION_CLIPS, then one with none:
        // the rest pose (getRestClip).
        const size_t clipCount = animation.clips.size();
        const uint64_t channelRow = getChannelRow(animation);
        std::vector<int32_t> channels((clipCount + 2) * channelRow, -1);
        std::vector<float> keys;
        uint32_t samplerIndex = 0;
        for (size_t c = 0; c < animation.clips.size(); ++c)
        {
            const AnimationClipRaw &clip = animation.clips[c];
            const uint32_t firstSampler = samplerIndex;
            for (const AnimationSampler &sampler : clip.samplers)
            {
                const gfx::AnimationSamplerData data{.firstTime = static_cast<uint32_t>(keys.size()),
                                                     .firstValue = static_cast<uint32_t>(keys.size() + sampler.times.size()),
                                                     .keyCount = static_cast<uint32_t>(sampler.times.size()),
                                                     .interpolation = static_cast<uint32_t>(sampler.interpolation)};
                keys.insert(keys.end(), sampler.times.begin(), sampler.times.end());
                for (const glm::vec4 &value : sampler.values)
                    keys.insert(keys.end(), {value.x, value.y, value.z, value.w});
                std::memcpy(bytes.data() + layout.samplers + samplerIndex * sizeof(data), &data, sizeof(data));
                ++samplerIndex;
            }
            // Two channels on one path of a node: the later one wins.
            for (const AnimationChannel &channel : clip.channels)
            {
                const uint64_t slot = channel.path == AnimationPath::WEIGHTS ? nodeCount * 3 + channel.node : channel.node * 3 + static_cast<uint32_t>(channel.path);
                channels[c * channelRow + slot] = static_cast<int32_t>(firstSampler + channel.sampler);
                channels[clipCount * channelRow + slot] = static_cast<int32_t>(firstSampler + channel.sampler);
            }
        }
        std::memcpy(bytes.data() + layout.channels, channels.data(), channels.size() * sizeof(int32_t));
        std::memcpy(bytes.data() + layout.subMeshes, subMeshes, subMeshCount * sizeof(gfx::AnimationSubMeshData));
        std::memcpy(bytes.data() + layout.keys, keys.data(), keys.size() * sizeof(float));
        for (size_t j = 0; j < animation.joints.size(); ++j)
        {
            gfx::AnimationJointData joint{.node = animation.joints[j].node};
            getRows(animation.joints[j].inverseBind, joint.inverseBind);
            std::memcpy(bytes.data() + layout.joints + j * sizeof(joint), &joint, sizeof(joint));
        }
        if (sphereCount > 0)
            std::memcpy(bytes.data() + layout.spheres, spheres, sphereCount * sizeof(glm::vec4));
        if (!animation.morphWeights.empty())
            std::memcpy(bytes.data() + layout.weights, animation.morphWeights.data(), animation.morphWeights.size() * sizeof(float));
        if (radiusCount > 0)
            std::memcpy(bytes.data() + layout.radii, morphRadii, radiusCount * sizeof(float));
        return bytes;
    }

    bool Renderer::createGpuAnimation()
    {
        if (config.maxAnimatedEntities == 0 || config.maxAnimatedEntities > MAX_ANIMATE_GROUPS)
        {
            spdlog::error("RendererConfig::maxAnimatedEntities is {}; it must be 1 to {}.", config.maxAnimatedEntities, MAX_ANIMATE_GROUPS);
            return false;
        }
        if (config.maxMorphWeights == 0)
        {
            spdlog::error("RendererConfig::maxMorphWeights is 0; it must be at least 1.");
            return false;
        }
        GpuAnimation &animation = *gpuAnimation;
        if (!animation.entities.create(ctx, "Animated Entity Table", sizeof(gfx::AnimatedEntity), config.maxAnimatedEntities))
        {
            spdlog::error("Failed to create the animated entity table of RendererConfig::maxAnimatedEntities = {} rows ({} B each, one copy per frame slot).",
                          config.maxAnimatedEntities, sizeof(gfx::AnimatedEntity));
            return false;
        }
        if (!animation.weightOverrides.create(ctx, "Morph Weight Override Table", sizeof(float), config.maxMorphWeights))
        {
            spdlog::error("Failed to create the morph weight table of RendererConfig::maxMorphWeights = {} rows (4 B each, one copy per frame slot).", config.maxMorphWeights);
            return false;
        }
        animation.pool = ctx.createBuffer({.name = "Animation Pool", .usage = BufferUsage::STORAGE, .size = std::max<uint64_t>(config.animationPoolBytes, 16), .staged = true});
        animation.pose = ctx.createBuffer({.name = "Animation Pose Buffer", .usage = BufferUsage::STORAGE, .size = FIRST_POSE_CAPACITY * POSE_BYTES_PER_NODE});
        animation.poseCapacity = FIRST_POSE_CAPACITY;
        animation.transformCapacity = FIRST_TRANSFORM_CAPACITY;
        animation.jointCapacity = FIRST_JOINT_CAPACITY;
        animation.skinnedCapacity = FIRST_SKINNED_CAPACITY;
        animation.poolRanges.reset(config.animationPoolBytes);
        animation.poseRanges.reset(FIRST_POSE_CAPACITY);
        animation.transformRanges.reset(FIRST_TRANSFORM_CAPACITY);
        animation.jointRanges.reset(FIRST_JOINT_CAPACITY);
        animation.skinnedRanges.reset(FIRST_SKINNED_CAPACITY);
        animation.weightRanges.reset(config.maxMorphWeights);
        bool created = animation.pool.isValid() && animation.pose.isValid();
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            animation.transforms[i] = ctx.createBuffer({.name = "Animated Transform Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = FIRST_TRANSFORM_CAPACITY * sizeof(gfx::InstanceTransform)});
            animation.joints[i] = ctx.createBuffer({.name = "Joint Matrix Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = FIRST_JOINT_CAPACITY * JOINT_BYTES});
            animation.skinnedVertices[i] = ctx.createBuffer({.name = "Skinned Vertex Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = FIRST_SKINNED_CAPACITY * sizeof(Vertex)});
            animation.frames[i] = ctx.createBuffer({.name = "Animation Frame Buffer", .usage = BufferUsage::STORAGE, .size = sizeof(gfx::AnimationFrame), .hostVisible = true});
            animation.weights[i] = ctx.createBuffer({.name = "Morph Weight Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = uint64_t{config.maxMorphWeights} * sizeof(float)});
            created = created && animation.transforms[i].isValid() && animation.joints[i].isValid() && animation.skinnedVertices[i].isValid() && animation.frames[i].isValid() &&
                      animation.weights[i].isValid() &&
                      clearBuffer(ctx, animation.transforms[i], FIRST_TRANSFORM_CAPACITY * sizeof(gfx::InstanceTransform));
        }
        if (!created)
        {
            spdlog::error("Failed to create the animation buffers (RendererConfig::animationPoolBytes = {}).", config.animationPoolBytes);
            return false;
        }
        return true;
    }

    // A mesh's animations take one range of the pool, freed with the mesh once the GPU is done with it.
    uint64_t Renderer::getMeshAnimationBytes(const GLTFRaw &raw) const
    {
        return getAnimationPoolBytes(raw.animation, countAnimatedSubMeshes(raw), countJointSpheres(raw), countMorphRadii(raw));
    }

    uint64_t Renderer::getAnimationPoolUsed() const
    {
        return gpuAnimation->poolRanges.used();
    }

    bool Renderer::uploadMeshAnimation(MeshHandle mesh, const GLTFRaw &raw)
    {
        const uint64_t bytes = getMeshAnimationBytes(raw);
        if (bytes == 0)
            return true;
        GpuAnimation &animation = *gpuAnimation;
        const std::vector<SubMesh> &subMeshes = registry.meshes[mesh.index].subMeshes;
        MeshAnimation info{.nodeCount = static_cast<uint32_t>(raw.animation.nodes.size()),
                           .jointCount = static_cast<uint32_t>(raw.animation.joints.size()),
                           .weightCount = static_cast<uint32_t>(raw.animation.morphWeights.size())};
        std::vector<gfx::AnimationSubMeshData> animated;
        std::vector<glm::vec4> spheres;
        std::vector<float> radii;
        const std::vector<uint8_t> mirrors = getMirroringNodes(raw.animation);
        for (size_t i = 0; i < raw.subGltfRaws.size(); ++i)
        {
            const SubGLTFRaw &sub = raw.subGltfRaws[i];
            // A skinned sub-mesh can mirror when one of its joints can.
            uint8_t mirror = sub.animatedNode >= 0 ? mirrors[sub.animatedNode] : 0;
            for (uint32_t j = 0; j < sub.jointCount; ++j)
                mirror = static_cast<uint8_t>(mirror | mirrors[raw.animation.joints[sub.firstJoint + j].node]);
            info.subMeshMirrors.push_back(mirror);
            info.subMeshTransforms.push_back(isAnimated(sub) ? static_cast<int32_t>(animated.size()) : -1);
            if (!isAnimated(sub))
                continue;
            // A posed sub-mesh starts on a Skin group, so a group never mixes two sub-meshes' morph targets.
            const uint32_t firstSkinnedVertex = isPosed(sub) ? alignToSkinGroup(info.skinnedVertexCount) : 0;
            animated.push_back({.node = sub.animatedNode >= 0 ? static_cast<uint32_t>(sub.animatedNode) : 0,
                                .jointCount = sub.jointCount,
                                .firstJoint = sub.firstJoint,
                                .firstSphere = static_cast<uint32_t>(spheres.size()),
                                .geometryIndex = subMeshes[i].geometry.geometryIndex,
                                .vertexCount = isPosed(sub) ? subMeshes[i].geometry.vertexCount : 0,
                                .firstSkinnedVertex = firstSkinnedVertex,
                                .firstWeight = sub.firstWeight >= 0 ? static_cast<uint32_t>(sub.firstWeight) : 0,
                                .firstMorphRadius = static_cast<uint32_t>(radii.size())});
            spheres.insert(spheres.end(), sub.jointSpheres.begin(), sub.jointSpheres.end());
            const std::vector<float> subRadii = getMorphRadii(sub.objRaw);
            radii.insert(radii.end(), subRadii.begin(), subRadii.end());
            if (isPosed(sub))
                info.skinnedVertexCount = firstSkinnedVertex + subMeshes[i].geometry.vertexCount;
        }
        info.transformCount = static_cast<uint32_t>(animated.size());
        // Every range is a multiple of 16 B, so every offset is too.
        if (!animation.poolRanges.allocate(bytes, info.poolOffset))
        {
            spdlog::error("The animation pool has no free range of {} B ({}/{} B in use, the largest free range {} B). RendererConfig::animationPoolBytes sets its size.", bytes,
                          animation.poolRanges.used(), animation.poolRanges.capacity(), animation.poolRanges.largestFree());
            return false;
        }
        info.poolBytes = bytes;
        info.address = ctx.getBufferDeviceAddress(animation.pool) + info.poolOffset;
        const std::vector<uint8_t> data = buildAnimationPoolData(raw.animation, animated.data(), info.transformCount, spheres.data(), static_cast<uint32_t>(spheres.size()),
                                                                 radii.data(), static_cast<uint32_t>(radii.size()), info.address);
        if (!ctx.fillBuffer(animation.pool, data.data(), data.size(), info.poolOffset))
        {
            animation.poolRanges.free(info.poolOffset, bytes);
            return false;
        }
        if (animation.meshes.size() <= mesh.index)
            animation.meshes.resize(mesh.index + 1);
        animation.meshes[mesh.index] = std::move(info);
        return true;
    }

    void Renderer::setAnimationTime(double seconds)
    {
        if (!std::isfinite(seconds))
        {
            spdlog::error("setAnimationTime: {} is not a time; the clock stays at {}.", seconds, gpuAnimation->clock);
            return;
        }
        gpuAnimation->clock = seconds;
    }

    void Renderer::playAnimation(EntityHandle entity, const AnimationPlay &play)
    {
        if (!isLive(entity))
        {
            spdlog::error("playAnimation: invalid entity handle.");
            return;
        }
        if (!std::isfinite(play.startTime) || !std::isfinite(play.clipTime) || !std::isfinite(play.speed))
        {
            spdlog::error("playAnimation: entity {} gets a start time, clip time or speed that is not finite; it keeps what it plays.", entity.index);
            return;
        }
        const MeshHandle mesh = registry.entities[entity.index].mesh;
        if (!isLive(mesh))
        {
            spdlog::error("playAnimation: entity {} has no mesh.", entity.index);
            return;
        }
        GpuAnimation &animation = *gpuAnimation;
        if (animation.entityStates.size() <= entity.index)
            animation.entityStates.resize(entity.index + 1);
        EntityAnimation &state = animation.entityStates[entity.index];

        // Before the entity has its instance rows, the play waits for them: syncGpuScene starts it then.
        const bool hasRows = entity.index < gpuScene->entities.size() && gpuScene->entities[entity.index].instanceCount > 0;
        if (registry.meshes[mesh.index].state == MeshState::LOADING || !hasRows)
        {
            if (registry.meshes[mesh.index].state == MeshState::FAILED)
            {
                spdlog::error("playAnimation: entity {} has no animation, its mesh failed to load.", entity.index);
                return;
            }
            state.pending = true;
            state.play = play;
            return;
        }
        // The next frame is the first to draw the new pose.
        startAnimation(entity, play, gpuScene->syncCount + 1);
    }

    void Renderer::stopAnimation(EntityHandle entity)
    {
        if (!isLive(entity))
        {
            spdlog::error("stopAnimation: invalid entity handle.");
            return;
        }
        if (gpuAnimation->entityStates.size() <= entity.index)
            return;
        EntityAnimation &state = gpuAnimation->entityStates[entity.index];
        state.pending = false;
        if (!state.playing)
            return;
        state.playing = false;
        // A skinned or morphed mesh goes back to its rest pose from the next frame, a jump with no motion from the frame
        // before.
        if (gpuAnimation->meshes[registry.entities[entity.index].mesh.index].skinnedVertexCount > 0)
        {
            state.rest = true;
            state.resetSync = gpuScene->syncCount + 1;
        }
        writeInstanceRows(entity, false);
    }

    std::vector<AnimationClip> Renderer::getAnimations(MeshHandle mesh) const
    {
        if (!isLive(mesh))
            return {};
        return registry.meshes[mesh.index].animations;
    }

    int32_t Renderer::findAnimation(MeshHandle mesh, const std::string &name) const
    {
        if (!isLive(mesh))
            return -1;
        const std::vector<AnimationClip> &clips = registry.meshes[mesh.index].animations;
        for (size_t i = 0; i < clips.size(); ++i)
        {
            if (clips[i].name == name)
                return static_cast<int32_t>(i);
        }
        return -1;
    }

    // Weights past count follow the clip. A mesh that still loads keeps them until the entity has its row, and then
    // drops those past its weight count. The first call is a jump, with no motion from the frame before; the next ones
    // move the vertices smoothly, as a clip does.
    void Renderer::setMorphWeights(EntityHandle entity, const float *weights, uint32_t count)
    {
        if (!isLive(entity))
        {
            spdlog::error("setMorphWeights: invalid entity handle.");
            return;
        }
        if (count > 0 && !weights)
        {
            spdlog::error("setMorphWeights: entity {} gets {} weights and no pointer to them.", entity.index, count);
            return;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!std::isfinite(weights[i]))
            {
                spdlog::error("setMorphWeights: weight {} of entity {} is not finite; its weights stay as they are.", i, entity.index);
                return;
            }
        }
        const MeshHandle mesh = registry.entities[entity.index].mesh;
        if (!isLive(mesh) || registry.meshes[mesh.index].state == MeshState::FAILED)
        {
            spdlog::error("setMorphWeights: entity {} has no mesh.", entity.index);
            return;
        }
        GpuAnimation &animation = *gpuAnimation;
        const uint32_t weightCount = mesh.index < animation.meshes.size() ? animation.meshes[mesh.index].weightCount : 0;
        if (registry.meshes[mesh.index].state == MeshState::READY && count > weightCount)
        {
            spdlog::error("setMorphWeights: entity {} gets {} weights, and its mesh has {}.", entity.index, count, weightCount);
            return;
        }
        if (animation.entityStates.size() <= entity.index)
            animation.entityStates.resize(entity.index + 1);
        EntityAnimation &state = animation.entityStates[entity.index];
        const bool started = !state.morphOverride;
        state.morphOverride = true;
        state.morphWeights.assign(weights, weights + count);
        // Without its row yet, takeAnimationRow writes them.
        if (state.row == UINT32_MAX)
            return;
        writeMorphWeightRows(entity);
        if (started)
        {
            state.resetSync = gpuScene->syncCount + 1;
            writeAnimationRow(entity);
        }
    }

    void Renderer::clearMorphWeights(EntityHandle entity)
    {
        if (!isLive(entity))
        {
            spdlog::error("clearMorphWeights: invalid entity handle.");
            return;
        }
        if (gpuAnimation->entityStates.size() <= entity.index || !gpuAnimation->entityStates[entity.index].morphOverride)
            return;
        EntityAnimation &state = gpuAnimation->entityStates[entity.index];
        state.morphOverride = false;
        state.morphWeights.clear();
        if (state.row == UINT32_MAX)
            return;
        writeMorphWeightRows(entity);
        state.resetSync = gpuScene->syncCount + 1;
        writeAnimationRow(entity);
    }

    std::vector<std::string> Renderer::getMorphTargetNames(MeshHandle mesh) const
    {
        if (!isLive(mesh))
            return {};
        return registry.meshes[mesh.index].morphTargetNames;
    }

    // One row per weight of the entity: setMorphWeights's value, or NaN, where the Animate pass takes the clip's.
    void Renderer::writeMorphWeightRows(EntityHandle entity)
    {
        GpuAnimation &animation = *gpuAnimation;
        const EntityAnimation &state = animation.entityStates[entity.index];
        const uint32_t weightCount = animation.meshes[registry.entities[entity.index].mesh.index].weightCount;
        for (uint32_t w = 0; w < weightCount; ++w)
        {
            const float value = state.morphOverride && w < state.morphWeights.size() ? state.morphWeights[w] : std::bit_cast<float>(NO_WEIGHT_OVERRIDE);
            animation.weightOverrides.setRow(state.firstWeight + w, &value);
        }
    }

    // From syncGpuScene, once the entity has its instance rows: the frame of this sync draws the pose first.
    void Renderer::startPendingAnimation(EntityHandle entity)
    {
        if (gpuAnimation->entityStates.size() <= entity.index || !gpuAnimation->entityStates[entity.index].pending)
            return;
        EntityAnimation &state = gpuAnimation->entityStates[entity.index];
        state.pending = false;
        startAnimation(entity, state.play, gpuScene->syncCount);
    }

    // A play takes the entity's row on its first pose (takeAnimationRow).
    void Renderer::startAnimation(EntityHandle entity, const AnimationPlay &play, uint32_t firstSync)
    {
        GpuAnimation &animation = *gpuAnimation;
        const MeshHandle mesh = registry.entities[entity.index].mesh;
        const std::vector<AnimationClip> &clips = registry.meshes[mesh.index].animations;
        if (play.clip >= clips.size() && (play.clip != ALL_ANIMATION_CLIPS || clips.empty()))
        {
            spdlog::error("playAnimation: entity {} cannot play clip {}: its mesh has {} clips.", entity.index, play.clip, clips.size());
            return;
        }
        if (!takeAnimationRow(entity, firstSync))
            return;
        EntityAnimation &state = animation.entityStates[entity.index];
        state.play = play;
        state.playing = true;
        state.rest = false;
        state.resetSync = firstSync;
        writeInstanceRows(entity, false);
    }

    // From syncGpuScene, once the entity has its instance rows: an entity of a skinned or morphed mesh that does not start
    // a play is posed by its skeleton's rest pose and its rest weights (glTF), so it takes its row now.
    void Renderer::poseAtRest(EntityHandle entity, uint32_t firstSync)
    {
        GpuAnimation &animation = *gpuAnimation;
        const MeshHandle mesh = registry.entities[entity.index].mesh;
        if (mesh.index >= animation.meshes.size() || animation.meshes[mesh.index].skinnedVertexCount == 0)
            return;
        if (animation.entityStates.size() <= entity.index)
            animation.entityStates.resize(entity.index + 1);
        if (animation.entityStates[entity.index].playing || !takeAnimationRow(entity, firstSync))
            return;
        EntityAnimation &state = animation.entityStates[entity.index];
        state.rest = true;
        state.resetSync = firstSync;
        writeInstanceRows(entity, false);
    }

    // The entity's first pose takes a table row and its ranges of transforms, pose nodes, joints, skinned vertices and
    // morph weights, which it keeps until it or its mesh is destroyed (releaseEntityAnimation).
    bool Renderer::takeAnimationRow(EntityHandle entity, uint32_t firstSync)
    {
        GpuAnimation &animation = *gpuAnimation;
        EntityAnimation &state = animation.entityStates[entity.index];
        const MeshAnimation &info = animation.meshes[registry.entities[entity.index].mesh.index];
        if (state.row == UINT32_MAX)
        {
            if (info.skinnedVertexCount > uint64_t{MAX_SKIN_GROUPS} * SKIN_GROUP_SIZE)
            {
                spdlog::error("Entity {} cannot be posed: its mesh has {} skinned vertices, and the Skin pass takes at most {}.", entity.index, info.skinnedVertexCount,
                              MAX_SKIN_GROUPS * SKIN_GROUP_SIZE);
                return false;
            }
            if (animation.entityRows >= animation.entities.capacity() && animation.freeRows.empty())
            {
                spdlog::error("The animated entity table is full: entity {} cannot be posed ({} rows in use). RendererConfig::maxAnimatedEntities sets its size.",
                              entity.index, animation.entities.capacity());
                return false;
            }
            uint64_t firstWeight = 0;
            if (!animation.weightRanges.allocate(info.weightCount, firstWeight))
            {
                spdlog::error("The morph weight table is full: entity {} cannot be posed ({} weights, {}/{} in use, the largest free range {}). RendererConfig::maxMorphWeights sets its size.",
                              entity.index, info.weightCount, animation.weightRanges.used(), animation.weightRanges.capacity(), animation.weightRanges.largestFree());
                return false;
            }
            // The buffers grow when a range does not fit: by the range, at least twice the capacity.
            uint64_t firstTransform = 0;
            uint64_t firstNode = 0;
            uint64_t firstJoint = 0;
            uint64_t firstSkinned = 0;
            const bool transformFits = animation.transformRanges.allocate(info.transformCount, firstTransform);
            const bool poseFits = animation.poseRanges.allocate(info.nodeCount, firstNode);
            const bool jointFits = animation.jointRanges.allocate(info.jointCount, firstJoint);
            const bool skinnedFits = animation.skinnedRanges.allocate(info.skinnedVertexCount, firstSkinned);
            if (!transformFits || !poseFits || !jointFits || !skinnedFits)
            {
                if (transformFits)
                    animation.transformRanges.free(firstTransform, info.transformCount);
                if (poseFits)
                    animation.poseRanges.free(firstNode, info.nodeCount);
                if (jointFits)
                    animation.jointRanges.free(firstJoint, info.jointCount);
                if (skinnedFits)
                    animation.skinnedRanges.free(firstSkinned, info.skinnedVertexCount);
                if (!growAnimationBuffers(animation.transformCapacity + (transformFits ? 0 : info.transformCount), animation.poseCapacity + (poseFits ? 0 : info.nodeCount),
                                          animation.jointCapacity + (jointFits ? 0 : info.jointCount), animation.skinnedCapacity + (skinnedFits ? 0 : info.skinnedVertexCount),
                                          firstSync) ||
                    !animation.transformRanges.allocate(info.transformCount, firstTransform) || !animation.poseRanges.allocate(info.nodeCount, firstNode) ||
                    !animation.jointRanges.allocate(info.jointCount, firstJoint) || !animation.skinnedRanges.allocate(info.skinnedVertexCount, firstSkinned))
                {
                    animation.weightRanges.free(firstWeight, info.weightCount);
                    spdlog::error("Entity {} cannot be posed: the animation buffers cannot grow.", entity.index);
                    return false;
                }
            }
            if (animation.freeRows.empty())
                state.row = animation.entityRows++;
            else
            {
                state.row = animation.freeRows.back();
                animation.freeRows.pop_back();
            }
            state.firstTransform = static_cast<uint32_t>(firstTransform);
            state.firstNode = static_cast<uint32_t>(firstNode);
            state.firstJoint = static_cast<uint32_t>(firstJoint);
            state.firstSkinnedVertex = static_cast<uint32_t>(firstSkinned);
            state.firstWeight = static_cast<uint32_t>(firstWeight);
            animation.maxSkinnedVertices = std::max(animation.maxSkinnedVertices, info.skinnedVertexCount);
            writeMorphWeightRows(entity);
        }
        return true;
    }

    // The row is written as stopped (clip -1), which the Animate and Skin passes skip, and goes on the free list with the
    // ranges. Both are reused at once: every frame slot has its own copy of the table, and the passes write the ranges
    // anew each frame for the entity that holds them, whose first frame has no motion from the frame before (resetSync).
    void Renderer::releaseEntityAnimation(EntityHandle entity)
    {
        GpuAnimation &animation = *gpuAnimation;
        if (entity.index >= animation.entityStates.size())
            return;
        EntityAnimation &state = animation.entityStates[entity.index];
        if (state.row != UINT32_MAX)
        {
            const MeshAnimation &info = animation.meshes[registry.entities[entity.index].mesh.index];
            const gfx::AnimatedEntity stopped{.clip = -1};
            animation.entities.setRow(state.row, &stopped);
            animation.freeRows.push_back(state.row);
            animation.transformRanges.free(state.firstTransform, info.transformCount);
            animation.poseRanges.free(state.firstNode, info.nodeCount);
            animation.jointRanges.free(state.firstJoint, info.jointCount);
            animation.skinnedRanges.free(state.firstSkinnedVertex, info.skinnedVertexCount);
            animation.weightRanges.free(state.firstWeight, info.weightCount);
        }
        state = {};
    }

    // The pool range holds data the GPU reads in frames still in flight, so it waits for them.
    void Renderer::releaseMeshAnimation(MeshHandle mesh)
    {
        GpuAnimation &animation = *gpuAnimation;
        if (mesh.index >= animation.meshes.size())
            return;
        const MeshAnimation &info = animation.meshes[mesh.index];
        if (info.poolBytes > 0)
        {
            const uint64_t offset = info.poolOffset;
            const uint64_t bytes = info.poolBytes;
            ctx.releaseAfterGpu([this, offset, bytes]()
                                { gpuAnimation->poolRanges.free(offset, bytes); });
        }
        animation.meshes[mesh.index] = {};
    }

    // Both slots' transforms and skinned vertices grow at once, after the GPU is done with them: a frame reads the other
    // slot's as the frame before. Their old rows are gone, so every playing entity starts its motion again in
    // firstSync. A new skinned vertex is not cleared: the Skin pass writes it in the frame its entity starts, before any
    // view reads it, and while the pipelines do not compile its transform holds zeros, which draws it as a point.
    bool Renderer::growAnimationBuffers(uint32_t transformCount, uint32_t poseCount, uint32_t jointCount, uint32_t skinnedCount, uint32_t firstSync)
    {
        GpuAnimation &animation = *gpuAnimation;
        if (transformCount <= animation.transformCapacity && poseCount <= animation.poseCapacity && jointCount <= animation.jointCapacity &&
            skinnedCount <= animation.skinnedCapacity)
            return true;
        ctx.waitForFence(frameFence, frameFenceValue);
        if (asyncCompute)
            ctx.waitForFence(computeFence, computeFenceValue);
        const uint32_t transformCapacity = std::max(transformCount, animation.transformCapacity * 2);
        const uint32_t poseCapacity = std::max(poseCount, animation.poseCapacity * 2);
        const uint32_t jointCapacity = std::max(jointCount, animation.jointCapacity * 2);
        const uint32_t skinnedCapacity = std::max(skinnedCount, animation.skinnedCapacity * 2);
        if (transformCount > animation.transformCapacity || skinnedCount > animation.skinnedCapacity)
        {
            // Set before the resizes, so a resize that fails halfway leaves no entity reading a cleared slot as the
            // frame before.
            for (size_t i = 0; i < animation.entityStates.size(); ++i)
            {
                EntityAnimation &state = animation.entityStates[i];
                if (!state.playing && !state.rest)
                    continue;
                state.resetSync = firstSync;
                writeAnimationRow(registry.entities[i].handle);
            }
        }
        if (transformCount > animation.transformCapacity)
        {
            for (auto &buffer : animation.transforms)
            {
                if (!ctx.resizeBuffer(buffer, uint64_t{transformCapacity} * sizeof(gfx::InstanceTransform)) ||
                    !clearBuffer(ctx, buffer, uint64_t{transformCapacity} * sizeof(gfx::InstanceTransform)))
                    return false;
            }
            animation.transformCapacity = transformCapacity;
            animation.transformRanges.grow(transformCapacity);
        }
        if (skinnedCount > animation.skinnedCapacity)
        {
            for (auto &buffer : animation.skinnedVertices)
            {
                if (!ctx.resizeBuffer(buffer, uint64_t{skinnedCapacity} * sizeof(Vertex)))
                    return false;
            }
            animation.skinnedCapacity = skinnedCapacity;
            animation.skinnedRanges.grow(skinnedCapacity);
            spdlog::info("The skinned vertex buffers hold {} vertices: {:.1f} MiB, {} B per vertex in each of the {} frame slots.", skinnedCapacity,
                         static_cast<double>(skinnedCapacity) * sizeof(Vertex) * MAX_FRAMES_IN_FLIGHT / (1024.0 * 1024.0), sizeof(Vertex), MAX_FRAMES_IN_FLIGHT);
        }
        if (jointCount > animation.jointCapacity)
        {
            for (auto &buffer : animation.joints)
            {
                if (!ctx.resizeBuffer(buffer, uint64_t{jointCapacity} * JOINT_BYTES))
                    return false;
            }
            animation.jointCapacity = jointCapacity;
            animation.jointRanges.grow(jointCapacity);
        }
        if (poseCount > animation.poseCapacity)
        {
            if (!ctx.resizeBuffer(animation.pose, uint64_t{poseCapacity} * POSE_BYTES_PER_NODE))
                return false;
            animation.poseCapacity = poseCapacity;
            animation.poseRanges.grow(poseCapacity);
        }
        return true;
    }

    // The draw list's cull mode follows the rest pose only, so such a sub-mesh is drawn without face culling: depth
    // still shows a closed mesh's outside, whichever way its matrix turns the winding.
    bool Renderer::animationCanMirror(MeshHandle mesh, uint32_t subMesh) const
    {
        const GpuAnimation &animation = *gpuAnimation;
        return mesh.index < animation.meshes.size() && subMesh < animation.meshes[mesh.index].subMeshMirrors.size() &&
               animation.meshes[mesh.index].subMeshMirrors[subMesh] != 0;
    }

    int32_t Renderer::getAnimationRow(EntityHandle entity, uint32_t subMesh) const
    {
        const GpuAnimation &animation = *gpuAnimation;
        if (animation.entityStates.size() <= entity.index || (!animation.entityStates[entity.index].playing && !animation.entityStates[entity.index].rest))
            return -1;
        const EntityAnimation &state = animation.entityStates[entity.index];
        const int32_t transform = animation.meshes[registry.entities[entity.index].mesh.index].subMeshTransforms[subMesh];
        return transform < 0 ? -1 : static_cast<int32_t>(state.firstTransform) + transform;
    }

    void Renderer::writeAnimationRow(EntityHandle entity)
    {
        const GpuAnimation &animation = *gpuAnimation;
        if (animation.entityStates.size() <= entity.index || animation.entityStates[entity.index].row == UINT32_MAX)
            return;
        const EntityAnimation &state = animation.entityStates[entity.index];
        const Entity &entityRef = registry.entities[entity.index];
        const std::vector<AnimationClip> &clips = registry.meshes[entityRef.mesh.index].animations;
        float duration = 0.0f;
        for (size_t i = 0; state.playing && i < clips.size(); ++i)
        {
            if (state.play.clip == ALL_ANIMATION_CLIPS || state.play.clip == i)
                duration = std::max(duration, clips[i].duration);
        }
        // The rest pose is the channel row after ALL_ANIMATION_CLIPS's, with no channel: every node at its rest TRS.
        int32_t clip = -1;
        if (state.playing)
            clip = static_cast<int32_t>(state.play.clip == ALL_ANIMATION_CLIPS ? clips.size() : state.play.clip);
        else if (state.rest)
            clip = static_cast<int32_t>(clips.size() + 1);
        gfx::AnimatedEntity row{.rowPadding = 0,
                                .mesh = animation.meshes[entityRef.mesh.index].address,
                                .phase = getAnimationPhase(state.play, duration, animation.base),
                                .speed = state.play.speed,
                                .duration = duration,
                                .clip = clip,
                                .loop = state.play.loop ? 1u : 0u,
                                .firstTransform = state.firstTransform,
                                .firstNode = state.firstNode,
                                .resetSync = state.resetSync,
                                .firstJoint = state.firstJoint,
                                .firstSkinnedVertex = state.firstSkinnedVertex,
                                .firstWeight = state.firstWeight,
                                .padding = 0};
        writeWorldRows(entityRef.transform.toMatrix(), row.entity, row.translationLow);
        gpuAnimation->entities.setRow(state.row, &row);
    }

    // The clock's base moves when the clock leaves its window, and the playing rows get their phase at the new base.
    void Renderer::syncGpuAnimation()
    {
        GpuAnimation &animation = *gpuAnimation;
        const double base = getAnimationBase(animation.base, animation.clock);
        if (base != animation.base)
        {
            animation.base = base;
            for (size_t i = 0; i < animation.entityStates.size(); ++i)
            {
                if (animation.entityStates[i].playing)
                    writeAnimationRow(registry.entities[i].handle);
            }
        }
        animation.entities.upload(ctx, static_cast<uint32_t>(frameCount));
        animation.weightOverrides.upload(ctx, static_cast<uint32_t>(frameCount));
    }

    void Renderer::addAnimationPass(RenderGraph &graph)
    {
        if (graph.animationPass)
            return;
        graph.animationPass = true;
        graph.addAsyncComputePass(
            "Animate",
            [this](PassBuilder &builder)
            {
                builder.write(gpuAnimation->transforms);
                builder.readPrevious(gpuAnimation->transforms);
                builder.write(gpuAnimation->pose);
                builder.write(gpuAnimation->joints);
                builder.write(gpuAnimation->weights);
            },
            [this](RenderContext &)
            { recordAnimation(); });
        // Once per frame, before every view: they all draw the vertices it writes.
        graph.addAsyncComputePass(
            "Skin",
            [this](PassBuilder &builder)
            {
                builder.read(gpuAnimation->joints);
                builder.read(gpuAnimation->weights);
                builder.write(gpuAnimation->skinnedVertices);
            },
            [this](RenderContext &)
            { recordSkinning(); });
    }

    // A shader error logs and retries the next frame; until then the animated parts are not drawn right. Once an
    // entity with skinned vertices plays, the Skin pipeline is made here too, so the Animate pass runs only when the
    // Skin pass can follow it.
    void Renderer::recordAnimation()
    {
        ZoneScoped;
        GpuAnimation &animation = *gpuAnimation;
        if (animation.entityRows == 0)
            return;
        if (!animation.pipeline.isValid())
            animation.pipeline = createComputePipeline("animate", readFile(pathFromUtf8(config.shaderDir) / "passes/animate.slang"), "animateMain");
        if (animation.skinnedRanges.used() > 0 && !animation.skinPipeline.isValid())
            animation.skinPipeline = createComputePipeline("skin", readFile(pathFromUtf8(config.shaderDir) / "passes/skin.slang"), "skinMain");
        if (!animation.pipeline.isValid() || (animation.skinnedRanges.used() > 0 && !animation.skinPipeline.isValid()))
        {
            spdlog::error("Failed to create the animation pipelines; animated meshes are not posed until they compile.");
            return;
        }

        const auto frame = static_cast<uint32_t>(frameCount);
        const uint32_t previous = (frame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
        const gfx::AnimationFrame data{.entities = ctx.getBufferDeviceAddress(animation.entities.buffer(frame)),
                                       .transforms = ctx.getBufferDeviceAddress(animation.transforms[frame]),
                                       .previousTransforms = ctx.getBufferDeviceAddress(animation.transforms[previous]),
                                       .pose = ctx.getBufferDeviceAddress(animation.pose),
                                       .clock = static_cast<float>(animation.clock - animation.base),
                                       .sync = gpuScene->syncCount,
                                       .entityCount = animation.entityRows,
                                       .previousSync = animation.recordedSync,
                                       .joints = ctx.getBufferDeviceAddress(animation.joints[frame]),
                                       .vertices = ctx.getBufferDeviceAddress(geometryPool->vertexBuffer()),
                                       .skins = ctx.getBufferDeviceAddress(geometryPool->skinBuffer()),
                                       .geometry = ctx.getBufferDeviceAddress(geometryPool->tableBuffer()),
                                       .skinnedVertices = ctx.getBufferDeviceAddress(animation.skinnedVertices[frame]),
                                       .weights = ctx.getBufferDeviceAddress(animation.weights[frame]),
                                       .weightOverrides = ctx.getBufferDeviceAddress(animation.weightOverrides.buffer(frame)),
                                       .morphs = ctx.getBufferDeviceAddress(geometryPool->morphBuffer())};
        if (!ctx.fillBuffer(animation.frames[frame], &data, sizeof(data)))
            return;
        animation.recordedSync = gpuScene->syncCount;

        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::AnimatePushConstants pushConstants{.frame = ctx.getBufferDeviceAddress(animation.frames[frame]), .zero = 0, .padding = 0};
        ctx.bindPipeline(commandBuffer, animation.pipeline);
        ctx.pushConstants(commandBuffer, animation.pipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, animation.entityRows);
    }

    // Runs after this frame's Animate pass, with its frame data: x is the entity's row, y its next 64 skinned vertices,
    // which morph targets move first and joints then.
    void Renderer::recordSkinning()
    {
        ZoneScoped;
        const GpuAnimation &animation = *gpuAnimation;
        if (animation.skinnedRanges.used() == 0 || animation.recordedSync != gpuScene->syncCount)
            return;
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::AnimatePushConstants pushConstants{.frame = ctx.getBufferDeviceAddress(animation.frames[frame]), .zero = 0, .padding = 0};
        ctx.bindPipeline(commandBuffer, animation.skinPipeline);
        ctx.pushConstants(commandBuffer, animation.skinPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, animation.entityRows, (animation.maxSkinnedVertices + SKIN_GROUP_SIZE - 1) / SKIN_GROUP_SIZE);
    }

    std::vector<float> getMorphRadii(const ObjRaw &raw)
    {
        std::vector<float> radii(raw.morphTargetCount, 0.0f);
        const size_t vertexCount = raw.vertices.size();
        for (size_t t = 0; t < raw.morphTargetCount; ++t)
        {
            for (size_t v = 0; v < vertexCount && t * vertexCount + v < raw.morphs.size(); ++v)
                radii[t] = std::max(radii[t], glm::length(raw.morphs[t * vertexCount + v].position));
        }
        return radii;
    }
}
