#include "renderer/animation.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/animationTime.h"
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
            uint64_t subMeshNodes = 0;
            uint64_t keys = 0;
            uint64_t size = 0;
            uint32_t samplerCount = 0;
        };

        constexpr uint32_t FIRST_TRANSFORM_CAPACITY = 64;
        constexpr uint32_t FIRST_POSE_CAPACITY = 256;
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

        PoolLayout getPoolLayout(const AnimationRaw &animation, uint32_t subMeshCount)
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
            layout.subMeshNodes = alignTo16(layout.channels + (animation.clips.size() + 1) * nodeCount * 3 * sizeof(int32_t));
            layout.keys = alignTo16(layout.subMeshNodes + subMeshCount * sizeof(uint32_t));
            layout.size = alignTo16(layout.keys + keyFloats * sizeof(float));
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

        uint32_t countAnimatedSubMeshes(const GLTFRaw &raw)
        {
            if (raw.animation.nodes.empty())
                return 0;
            return static_cast<uint32_t>(std::count_if(raw.subGltfRaws.begin(), raw.subGltfRaws.end(), [](const SubGLTFRaw &sub)
                                                       { return sub.animatedNode >= 0; }));
        }
    }

    uint64_t getAnimationPoolBytes(const AnimationRaw &animation, uint32_t subMeshCount)
    {
        if (animation.nodes.empty() || subMeshCount == 0)
            return 0;
        return getPoolLayout(animation, subMeshCount).size;
    }

    std::vector<uint8_t> buildAnimationPoolData(const AnimationRaw &animation, const int32_t *subMeshNodes, uint32_t subMeshCount, uint64_t address)
    {
        const PoolLayout layout = getPoolLayout(animation, subMeshCount);
        const auto nodeCount = static_cast<uint32_t>(animation.nodes.size());
        std::vector<uint8_t> bytes(layout.size, 0);

        const gfx::AnimationMeshData header{.nodes = address + layout.nodes,
                                            .samplers = address + layout.samplers,
                                            .channels = address + layout.channels,
                                            .subMeshNodes = address + layout.subMeshNodes,
                                            .keys = address + layout.keys,
                                            .nodeCount = nodeCount,
                                            .subMeshCount = subMeshCount};
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
        std::memcpy(bytes.data() + layout.subMeshNodes, subMeshNodes, subMeshCount * sizeof(int32_t));
        std::memcpy(bytes.data() + layout.keys, keys.data(), keys.size() * sizeof(float));
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
        bool created = animation.pool.isValid() && animation.pose.isValid();
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            animation.transforms[i] = ctx.createBuffer({.name = "Animated Transform Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = FIRST_TRANSFORM_CAPACITY * sizeof(gfx::InstanceTransform)});
            animation.frames[i] = ctx.createBuffer({.name = "Animation Frame Buffer", .usage = BufferUsage::STORAGE, .size = sizeof(gfx::AnimationFrame), .hostVisible = true});
            created = created && animation.transforms[i].isValid() && animation.frames[i].isValid() &&
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
        return getAnimationPoolBytes(raw.animation, countAnimatedSubMeshes(raw));
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
        MeshAnimation info{.nodeCount = static_cast<uint32_t>(raw.animation.nodes.size())};
        std::vector<int32_t> subMeshNodes;
        for (const SubGLTFRaw &sub : raw.subGltfRaws)
        {
            info.subMeshTransforms.push_back(sub.animatedNode >= 0 ? static_cast<int32_t>(subMeshNodes.size()) : -1);
            if (sub.animatedNode >= 0)
                subMeshNodes.push_back(sub.animatedNode);
        }
        info.transformCount = static_cast<uint32_t>(subMeshNodes.size());
        info.address = ctx.getBufferDeviceAddress(animation.pool) + animation.poolUsed;
        const std::vector<uint8_t> data = buildAnimationPoolData(raw.animation, subMeshNodes.data(), info.transformCount, info.address);
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
            if (animation.entityRows >= animation.entities.capacity())
            {
                spdlog::error("The animated entity table is full: entity {} cannot play ({}/{} rows in use). EngineConfig::maxAnimatedEntities sets its size.",
                              entity.index, animation.entityRows, animation.entities.capacity());
                return;
            }
            if (!growAnimationBuffers(animation.transformCount + info.transformCount, animation.poseCount + info.nodeCount, firstSync))
            {
                spdlog::error("Entity {} cannot play: the animation buffers cannot grow.", entity.index);
                return;
            }
            state.row = animation.entityRows++;
            state.firstTransform = animation.transformCount;
            state.firstNode = animation.poseCount;
            animation.transformCount += info.transformCount;
            animation.poseCount += info.nodeCount;
        }
        state.play = play;
        state.playing = true;
        state.resetSync = firstSync;
        writeInstanceRows(entity, false);
    }

    // Both slots' transforms grow at once, after the GPU is done with them: a frame reads the other slot's as the
    // frame before. Their old rows are gone, so every playing entity starts its motion again in firstSync.
    bool Engine::growAnimationBuffers(uint32_t transformCount, uint32_t poseCount, uint32_t firstSync)
    {
        GpuAnimation &animation = *gpuAnimation;
        if (transformCount <= animation.transformCapacity && poseCount <= animation.poseCapacity)
            return true;
        ctx.waitForFence(frameFence, frameFenceValue);
        if (asyncCompute)
            ctx.waitForFence(computeFence, computeFenceValue);
        const uint32_t transformCapacity = std::max(transformCount, animation.transformCapacity * 2);
        const uint32_t poseCapacity = std::max(poseCount, animation.poseCapacity * 2);
        if (transformCount > animation.transformCapacity)
        {
            for (auto &buffer : animation.transforms)
            {
                if (!ctx.resizeBuffer(buffer, uint64_t{transformCapacity} * sizeof(gfx::InstanceTransform)) ||
                    !clearBuffer(ctx, buffer, uint64_t{transformCapacity} * sizeof(gfx::InstanceTransform)))
                    return false;
            }
            animation.transformCapacity = transformCapacity;
            for (size_t i = 0; i < animation.entityStates.size(); ++i)
            {
                EntityAnimation &state = animation.entityStates[i];
                if (!state.playing)
                    continue;
                state.resetSync = firstSync;
                writeAnimationRow(registry.entities[i].handle);
            }
        }
        if (poseCount > animation.poseCapacity)
        {
            if (!ctx.resizeBuffer(animation.pose, uint64_t{poseCapacity} * POSE_BYTES_PER_NODE))
                return false;
            animation.poseCapacity = poseCapacity;
        }
        return true;
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
                                      .resetSync = state.resetSync};
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
            },
            [this](RenderContext &)
            { recordAnimation(); });
    }

    // A shader error logs and retries the next frame; until then the animated parts are not drawn right.
    void Engine::recordAnimation()
    {
        ZoneScoped;
        GpuAnimation &animation = *gpuAnimation;
        if (animation.entityRows == 0)
            return;
        if (!animation.pipeline.isValid())
        {
            animation.pipeline = createComputePipeline("animate", readFile(pathFromUtf8(config.shaderDir) / "passes/animate.slang"), "animateMain");
            if (!animation.pipeline.isValid())
            {
                spdlog::error("Failed to create the animation pipeline; animated meshes are not posed until it compiles.");
                return;
            }
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
                                       .padding = 0};
        if (!ctx.fillBuffer(animation.frames[frame], &data, sizeof(data)))
            return;

        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::AnimatePushConstants pushConstants{.frame = ctx.getBufferDeviceAddress(animation.frames[frame]), .padding = {0, 0}};
        ctx.bindPipeline(commandBuffer, animation.pipeline);
        ctx.pushConstants(commandBuffer, animation.pipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, animation.entityRows);
    }
}
