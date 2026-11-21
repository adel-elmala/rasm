#include "renderer/animation.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/animationTime.h"
#include "renderer/geometryPool.h"
#include "renderer/gpuScene.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
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
    }

    namespace
    {
        uint64_t alignTo16(uint64_t value)
        {
            return (value + 15) & ~uint64_t{15};
        }

        PoolLayout getPoolLayout(const AnimationRaw &animation, uint32_t subMeshCount, uint32_t sphereCount)
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
            layout.subMeshes = alignTo16(layout.channels + (animation.clips.size() + 1) * nodeCount * 3 * sizeof(int32_t));
            layout.keys = alignTo16(layout.subMeshes + subMeshCount * sizeof(gfx::AnimationSubMeshData));
            layout.joints = alignTo16(layout.keys + keyFloats * sizeof(float));
            layout.spheres = alignTo16(layout.joints + animation.joints.size() * sizeof(gfx::AnimationJointData));
            layout.size = alignTo16(layout.spheres + sphereCount * sizeof(glm::vec4));
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
    }

    uint64_t getAnimationPoolBytes(const AnimationRaw &animation, uint32_t subMeshCount, uint32_t sphereCount)
    {
        if (animation.nodes.empty() || subMeshCount == 0)
            return 0;
        return getPoolLayout(animation, subMeshCount, sphereCount).size;
    }

    std::vector<uint8_t> buildAnimationPoolData(const AnimationRaw &animation, const gfx::AnimationSubMeshData *subMeshes, uint32_t subMeshCount, const glm::vec4 *spheres,
                                                uint32_t sphereCount, uint64_t address)
    {
        const PoolLayout layout = getPoolLayout(animation, subMeshCount, sphereCount);
        const auto nodeCount = static_cast<uint32_t>(animation.nodes.size());
        std::vector<uint8_t> bytes(layout.size, 0);

        uint32_t skinnedVertexCount = 0;
        for (uint32_t k = 0; k < subMeshCount; ++k)
        {
            if (subMeshes[k].jointCount > 0)
                skinnedVertexCount = std::max(skinnedVertexCount, subMeshes[k].firstSkinnedVertex + subMeshes[k].vertexCount);
        }
        const gfx::AnimationMeshData header{.nodes = address + layout.nodes,
                                            .samplers = address + layout.samplers,
                                            .channels = address + layout.channels,
                                            .subMeshes = address + layout.subMeshes,
                                            .keys = address + layout.keys,
                                            .joints = address + layout.joints,
                                            .jointSpheres = address + layout.spheres,
                                            .nodeCount = nodeCount,
                                            .subMeshCount = subMeshCount,
                                            .jointCount = static_cast<uint32_t>(animation.joints.size()),
                                            .skinnedVertexCount = skinnedVertexCount};
        std::memcpy(bytes.data(), &header, sizeof(header));

        for (uint32_t n = 0; n < nodeCount; ++n)
        {
            const AnimationNode &node = animation.nodes[n];
            gfx::AnimationNodeData data{.translation = node.translation, .parent = node.parent, .rotation = node.rotation, .scale = node.scale, .padding = 0};
            getRows(node.rest, data.rest);
            std::memcpy(bytes.data() + layout.nodes + n * sizeof(data), &data, sizeof(data));
        }

        // A row of channels per clip, then one with every clip's channels for ALL_ANIMATION_CLIPS.
        const size_t clipCount = animation.clips.size();
        std::vector<int32_t> channels((clipCount + 1) * nodeCount * 3, -1);
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
                const size_t slot = channel.node * 3 + static_cast<uint32_t>(channel.path);
                channels[c * nodeCount * 3 + slot] = static_cast<int32_t>(firstSampler + channel.sampler);
                channels[clipCount * nodeCount * 3 + slot] = static_cast<int32_t>(firstSampler + channel.sampler);
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
        return bytes;
    }

    bool Engine::createGpuAnimation()
    {
        if (config.maxAnimatedEntities == 0 || config.maxAnimatedEntities > MAX_ANIMATE_GROUPS)
        {
            spdlog::error("EngineConfig::maxAnimatedEntities is {}; it must be 1 to {}.", config.maxAnimatedEntities, MAX_ANIMATE_GROUPS);
            return false;
        }
        GpuAnimation &animation = *gpuAnimation;
        if (!animation.entities.create(ctx, "Animated Entity Table", sizeof(gfx::AnimatedEntity), config.maxAnimatedEntities))
        {
            spdlog::error("Failed to create the animated entity table of EngineConfig::maxAnimatedEntities = {} rows ({} B each, one copy per frame slot).",
                          config.maxAnimatedEntities, sizeof(gfx::AnimatedEntity));
            return false;
        }
        animation.pool = ctx.createBuffer({.name = "Animation Pool", .usage = BufferUsage::STORAGE, .size = std::max<uint64_t>(config.animationPoolBytes, 16)});
        animation.pose = ctx.createBuffer({.name = "Animation Pose Buffer", .usage = BufferUsage::STORAGE, .size = FIRST_POSE_CAPACITY * POSE_BYTES_PER_NODE});
        animation.poseCapacity = FIRST_POSE_CAPACITY;
        animation.transformCapacity = FIRST_TRANSFORM_CAPACITY;
        animation.jointCapacity = FIRST_JOINT_CAPACITY;
        animation.skinnedCapacity = FIRST_SKINNED_CAPACITY;
        bool created = animation.pool.isValid() && animation.pose.isValid();
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            animation.transforms[i] = ctx.createBuffer({.name = "Animated Transform Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = FIRST_TRANSFORM_CAPACITY * sizeof(gfx::InstanceTransform)});
            animation.joints[i] = ctx.createBuffer({.name = "Joint Matrix Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = FIRST_JOINT_CAPACITY * JOINT_BYTES});
            animation.skinnedVertices[i] = ctx.createBuffer({.name = "Skinned Vertex Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = FIRST_SKINNED_CAPACITY * sizeof(Vertex)});
            animation.frames[i] = ctx.createBuffer({.name = "Animation Frame Buffer", .usage = BufferUsage::STORAGE, .size = sizeof(gfx::AnimationFrame), .hostVisible = true});
            created = created && animation.transforms[i].isValid() && animation.joints[i].isValid() && animation.skinnedVertices[i].isValid() && animation.frames[i].isValid() &&
                      clearBuffer(ctx, animation.transforms[i], FIRST_TRANSFORM_CAPACITY * sizeof(gfx::InstanceTransform));
        }
        if (!created)
        {
            spdlog::error("Failed to create the animation buffers (EngineConfig::animationPoolBytes = {}).", config.animationPoolBytes);
            return false;
        }
        return true;
    }

    // A mesh's animations take one range of the pool, which is never freed (D4), like its geometry.
    uint64_t Engine::getMeshAnimationBytes(const GLTFRaw &raw) const
    {
        return getAnimationPoolBytes(raw.animation, countAnimatedSubMeshes(raw), countJointSpheres(raw));
    }

    uint64_t Engine::getAnimationPoolUsed() const
    {
        return gpuAnimation->poolUsed;
    }

    bool Engine::uploadMeshAnimation(MeshHandle mesh, const GLTFRaw &raw)
    {
        const uint64_t bytes = getMeshAnimationBytes(raw);
        if (bytes == 0)
            return true;
        GpuAnimation &animation = *gpuAnimation;
        const std::vector<SubMesh> &subMeshes = registry.meshes[mesh.index].subMeshes;
        MeshAnimation info{.nodeCount = static_cast<uint32_t>(raw.animation.nodes.size()), .jointCount = static_cast<uint32_t>(raw.animation.joints.size())};
        std::vector<gfx::AnimationSubMeshData> animated;
        std::vector<glm::vec4> spheres;
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
            animated.push_back({.node = sub.animatedNode >= 0 ? static_cast<uint32_t>(sub.animatedNode) : 0,
                                .jointCount = sub.jointCount,
                                .firstJoint = sub.firstJoint,
                                .firstSphere = static_cast<uint32_t>(spheres.size()),
                                .geometryIndex = subMeshes[i].geometry.geometryIndex,
                                .vertexCount = sub.jointCount > 0 ? subMeshes[i].geometry.vertexCount : 0,
                                .firstSkinnedVertex = sub.jointCount > 0 ? info.skinnedVertexCount : 0});
            if (sub.jointCount > 0)
            {
                spheres.insert(spheres.end(), sub.jointSpheres.begin(), sub.jointSpheres.end());
                info.skinnedVertexCount += subMeshes[i].geometry.vertexCount;
            }
        }
        info.transformCount = static_cast<uint32_t>(animated.size());
        info.address = ctx.getBufferDeviceAddress(animation.pool) + animation.poolUsed;
        const std::vector<uint8_t> data =
            buildAnimationPoolData(raw.animation, animated.data(), info.transformCount, spheres.data(), static_cast<uint32_t>(spheres.size()), info.address);
        if (!ctx.fillBuffer(animation.pool, data.data(), data.size(), animation.poolUsed))
            return false;
        animation.poolUsed += bytes;
        if (animation.meshes.size() <= mesh.index)
            animation.meshes.resize(mesh.index + 1);
        animation.meshes[mesh.index] = std::move(info);
        return true;
    }

    void Engine::setAnimationTime(double seconds)
    {
        if (!std::isfinite(seconds))
        {
            spdlog::error("setAnimationTime: {} is not a time; the clock stays at {}.", seconds, gpuAnimation->clock);
            return;
        }
        gpuAnimation->clock = seconds;
    }

    void Engine::playAnimation(EntityHandle entity, const AnimationPlay &play)
    {
        if (!entity.isValid() || entity.index >= registry.entities.size())
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
        if (!mesh.isValid() || mesh.index >= registry.meshes.size())
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

    void Engine::stopAnimation(EntityHandle entity)
    {
        if (!entity.isValid() || entity.index >= registry.entities.size())
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
        writeInstanceRows(entity, false);
    }

    std::vector<AnimationClip> Engine::getAnimations(MeshHandle mesh) const
    {
        if (!mesh.isValid() || mesh.index >= registry.meshes.size())
            return {};
        return registry.meshes[mesh.index].animations;
    }

    int32_t Engine::findAnimation(MeshHandle mesh, const std::string &name) const
    {
        if (!mesh.isValid() || mesh.index >= registry.meshes.size())
            return -1;
        const std::vector<AnimationClip> &clips = registry.meshes[mesh.index].animations;
        for (size_t i = 0; i < clips.size(); ++i)
        {
            if (clips[i].name == name)
                return static_cast<int32_t>(i);
        }
        return -1;
    }

    // From syncGpuScene, once the entity has its instance rows: the frame of this sync draws the pose first.
    void Engine::startPendingAnimation(EntityHandle entity)
    {
        if (gpuAnimation->entityStates.size() <= entity.index || !gpuAnimation->entityStates[entity.index].pending)
            return;
        EntityAnimation &state = gpuAnimation->entityStates[entity.index];
        state.pending = false;
        startAnimation(entity, state.play, gpuScene->syncCount);
    }

    // The entity's first play takes a table row and its ranges of transforms and pose nodes, which it keeps (D4).
    void Engine::startAnimation(EntityHandle entity, const AnimationPlay &play, uint32_t firstSync)
    {
        GpuAnimation &animation = *gpuAnimation;
        const MeshHandle mesh = registry.entities[entity.index].mesh;
        const std::vector<AnimationClip> &clips = registry.meshes[mesh.index].animations;
        if (play.clip >= clips.size() && (play.clip != ALL_ANIMATION_CLIPS || clips.empty()))
        {
            spdlog::error("playAnimation: entity {} cannot play clip {}: its mesh has {} clips.", entity.index, play.clip, clips.size());
            return;
        }
        EntityAnimation &state = animation.entityStates[entity.index];
        const MeshAnimation &info = animation.meshes[mesh.index];
        if (state.row == UINT32_MAX)
        {
            if (info.skinnedVertexCount > uint64_t{MAX_SKIN_GROUPS} * SKIN_GROUP_SIZE)
            {
                spdlog::error("Entity {} cannot play: its mesh has {} skinned vertices, and the Skin pass takes at most {}.", entity.index, info.skinnedVertexCount,
                              MAX_SKIN_GROUPS * SKIN_GROUP_SIZE);
                return;
            }
            if (animation.entityRows >= animation.entities.capacity())
            {
                spdlog::error("The animated entity table is full: entity {} cannot play ({}/{} rows in use). EngineConfig::maxAnimatedEntities sets its size.",
                              entity.index, animation.entityRows, animation.entities.capacity());
                return;
            }
            if (!growAnimationBuffers(animation.transformCount + info.transformCount, animation.poseCount + info.nodeCount, animation.jointCount + info.jointCount,
                                      animation.skinnedCount + info.skinnedVertexCount, firstSync))
            {
                spdlog::error("Entity {} cannot play: the animation buffers cannot grow.", entity.index);
                return;
            }
            state.row = animation.entityRows++;
            state.firstTransform = animation.transformCount;
            state.firstNode = animation.poseCount;
            state.firstJoint = animation.jointCount;
            state.firstSkinnedVertex = animation.skinnedCount;
            animation.transformCount += info.transformCount;
            animation.poseCount += info.nodeCount;
            animation.jointCount += info.jointCount;
            animation.skinnedCount += info.skinnedVertexCount;
            animation.maxSkinnedVertices = std::max(animation.maxSkinnedVertices, info.skinnedVertexCount);
        }
        state.play = play;
        state.playing = true;
        state.resetSync = firstSync;
        writeInstanceRows(entity, false);
    }

    // Both slots' transforms and skinned vertices grow at once, after the GPU is done with them: a frame reads the other
    // slot's as the frame before. Their old rows are gone, so every playing entity starts its motion again in
    // firstSync. A new skinned vertex is not cleared: the Skin pass writes it in the frame its entity starts, before any
    // view reads it, and while the pipelines do not compile its transform holds zeros, which draws it as a point.
    bool Engine::growAnimationBuffers(uint32_t transformCount, uint32_t poseCount, uint32_t jointCount, uint32_t skinnedCount, uint32_t firstSync)
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
                if (!state.playing)
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
        }
        if (skinnedCount > animation.skinnedCapacity)
        {
            for (auto &buffer : animation.skinnedVertices)
            {
                if (!ctx.resizeBuffer(buffer, uint64_t{skinnedCapacity} * sizeof(Vertex)))
                    return false;
            }
            animation.skinnedCapacity = skinnedCapacity;
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
        }
        if (poseCount > animation.poseCapacity)
        {
            if (!ctx.resizeBuffer(animation.pose, uint64_t{poseCapacity} * POSE_BYTES_PER_NODE))
                return false;
            animation.poseCapacity = poseCapacity;
        }
        return true;
    }

    // The draw list's cull mode follows the rest pose only, so such a sub-mesh is drawn without face culling: depth
    // still shows a closed mesh's outside, whichever way its matrix turns the winding.
    bool Engine::animationCanMirror(MeshHandle mesh, uint32_t subMesh) const
    {
        const GpuAnimation &animation = *gpuAnimation;
        return mesh.index < animation.meshes.size() && subMesh < animation.meshes[mesh.index].subMeshMirrors.size() &&
               animation.meshes[mesh.index].subMeshMirrors[subMesh] != 0;
    }

    int32_t Engine::getAnimationRow(EntityHandle entity, uint32_t subMesh) const
    {
        const GpuAnimation &animation = *gpuAnimation;
        if (animation.entityStates.size() <= entity.index || !animation.entityStates[entity.index].playing)
            return -1;
        const EntityAnimation &state = animation.entityStates[entity.index];
        const int32_t transform = animation.meshes[registry.entities[entity.index].mesh.index].subMeshTransforms[subMesh];
        return transform < 0 ? -1 : static_cast<int32_t>(state.firstTransform) + transform;
    }

    void Engine::writeAnimationRow(EntityHandle entity)
    {
        const GpuAnimation &animation = *gpuAnimation;
        if (animation.entityStates.size() <= entity.index || animation.entityStates[entity.index].row == UINT32_MAX)
            return;
        const EntityAnimation &state = animation.entityStates[entity.index];
        const Entity &entityRef = registry.entities[entity.index];
        const std::vector<AnimationClip> &clips = registry.meshes[entityRef.mesh.index].animations;
        float duration = 0.0f;
        for (size_t i = 0; i < clips.size(); ++i)
        {
            if (state.play.clip == ALL_ANIMATION_CLIPS || state.play.clip == i)
                duration = std::max(duration, clips[i].duration);
        }
        const uint32_t clip = state.play.clip == ALL_ANIMATION_CLIPS ? static_cast<uint32_t>(clips.size()) : state.play.clip;
        const gfx::AnimatedEntity row{.entityMatrix = entityRef.transform.toMatrix(),
                                      .mesh = animation.meshes[entityRef.mesh.index].address,
                                      .phase = getAnimationPhase(state.play, duration, animation.base),
                                      .speed = state.play.speed,
                                      .duration = duration,
                                      .clip = state.playing ? static_cast<int32_t>(clip) : -1,
                                      .loop = state.play.loop ? 1u : 0u,
                                      .firstTransform = state.firstTransform,
                                      .firstNode = state.firstNode,
                                      .resetSync = state.resetSync,
                                      .firstJoint = state.firstJoint,
                                      .firstSkinnedVertex = state.firstSkinnedVertex};
        gpuAnimation->entities.setRow(state.row, &row);
    }

    // The clock's base moves when the clock leaves its window, and the playing rows get their phase at the new base.
    void Engine::syncGpuAnimation()
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
    }

    void Engine::addAnimationPass(RenderGraph &graph)
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
            },
            [this](RenderContext &)
            { recordAnimation(); });
        // Once per frame, before every view: they all draw the vertices it writes.
        graph.addAsyncComputePass(
            "Skin",
            [this](PassBuilder &builder)
            {
                builder.read(gpuAnimation->joints);
                builder.write(gpuAnimation->skinnedVertices);
            },
            [this](RenderContext &)
            { recordSkinning(); });
    }

    // A shader error logs and retries the next frame; until then the animated parts are not drawn right. The Skin
    // pipeline is made here too, so the Animate pass runs only when the Skin pass can follow it.
    void Engine::recordAnimation()
    {
        ZoneScoped;
        GpuAnimation &animation = *gpuAnimation;
        if (animation.entityRows == 0)
            return;
        if (!animation.pipeline.isValid())
            animation.pipeline = createComputePipeline("animate", readFile(pathFromUtf8(config.shaderDir) / "passes/animate.slang"), "animateMain");
        if (!animation.skinPipeline.isValid())
            animation.skinPipeline = createComputePipeline("skin", readFile(pathFromUtf8(config.shaderDir) / "passes/skin.slang"), "skinMain");
        if (!animation.pipeline.isValid() || !animation.skinPipeline.isValid())
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
                                       .skinnedVertices = ctx.getBufferDeviceAddress(animation.skinnedVertices[frame])};
        if (!ctx.fillBuffer(animation.frames[frame], &data, sizeof(data)))
            return;
        animation.recordedSync = gpuScene->syncCount;

        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::AnimatePushConstants pushConstants{.frame = ctx.getBufferDeviceAddress(animation.frames[frame]), .padding = {0, 0}};
        ctx.bindPipeline(commandBuffer, animation.pipeline);
        ctx.pushConstants(commandBuffer, animation.pipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, animation.entityRows);
    }

    // Runs after this frame's Animate pass, with its frame data: x is the entity's row, y its next 64 skinned vertices.
    void Engine::recordSkinning()
    {
        ZoneScoped;
        const GpuAnimation &animation = *gpuAnimation;
        if (animation.skinnedCount == 0 || animation.recordedSync != gpuScene->syncCount)
            return;
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::AnimatePushConstants pushConstants{.frame = ctx.getBufferDeviceAddress(animation.frames[frame]), .padding = {0, 0}};
        ctx.bindPipeline(commandBuffer, animation.skinPipeline);
        ctx.pushConstants(commandBuffer, animation.skinPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, animation.entityRows, (animation.maxSkinnedVertices + SKIN_GROUP_SIZE - 1) / SKIN_GROUP_SIZE);
    }
}
