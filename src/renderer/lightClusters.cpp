#include "renderer/lightClusters.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>

namespace rasm
{
    namespace
    {
        constexpr uint32_t CLUSTER_GROUP_SIZE = 64; // numthreads of clustersMain in lightClusters.slang
        constexpr uint32_t FIRST_LIGHT_CAPACITY = 32;

        // The cone's falloff of a spot light, and none for another light.
        void setSpotCone(const Light &light, gfx::Light &gpuLight)
        {
            gpuLight.spotScale = 0.0f;
            gpuLight.spotOffset = 1.0f;
            if (light.type != LightType::SPOT)
                return;
            const float cosOuter = std::cos(light.outerConeAngle);
            gpuLight.spotScale = 1.0f / std::max(std::cos(light.innerConeAngle) - cosOuter, 1e-3f);
            gpuLight.spotOffset = -cosOuter * gpuLight.spotScale;
        }

        uint64_t getAddress(RenderContext &ctx, const BufferHandle &buffer)
        {
            return buffer.isValid() ? ctx.getBufferDeviceAddress(buffer) : 0;
        }
    }

    // A light without a range reaches every point, so no cluster can leave it out.
    bool isGlobalLight(const Light &light)
    {
        return light.type == LightType::DIRECTIONAL || !(light.range > 0.0f);
    }

    // The global lights first, then the local ones, each in the scene's order. Returns the global count.
    uint32_t buildSceneLightList(const Light *lights, const LightHandle *sceneLights, uint32_t count, std::vector<uint32_t> &rows)
    {
        rows.clear();
        for (uint32_t i = 0; i < count; ++i)
        {
            if (isGlobalLight(lights[sceneLights[i].index]))
                rows.push_back(static_cast<uint32_t>(sceneLights[i].index));
        }
        const auto globalCount = static_cast<uint32_t>(rows.size());
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!isGlobalLight(lights[sceneLights[i].index]))
                rows.push_back(static_cast<uint32_t>(sceneLights[i].index));
        }
        return globalCount;
    }

    // The header, a light count per cluster, MAX_CLUSTER_LIGHTS rows per cluster, a light count per tile and
    // CLUSTER_SLICES * MAX_CLUSTER_LIGHTS lights per tile, then the visible local lights' spheres in view space and
    // their visible bits. Must match lightClusters.slang.
    uint64_t getClusterBufferSize(uint32_t lightCapacity)
    {
        const uint64_t tiles = uint64_t{CLUSTER_TILES_X} * CLUSTER_TILES_Y;
        const uint64_t words = CLUSTER_HEADER_WORDS + CLUSTER_COUNT + static_cast<uint64_t>(CLUSTER_COUNT) * MAX_CLUSTER_LIGHTS + tiles + tiles * CLUSTER_SLICES * MAX_CLUSTER_LIGHTS;
        return words * sizeof(uint32_t) + static_cast<uint64_t>(lightCapacity) * 4 * sizeof(float) + (lightCapacity + 31) / 32 * sizeof(uint32_t);
    }

    // Slice k starts at this view depth. The slices cut sliceNear..sliceFar evenly for an orthographic camera, and
    // for a perspective one each slice is the same factor longer than the one before. Must match lightClusters.slang.
    float getClusterSliceDepth(uint32_t slice, float sliceNear, float sliceFar, bool even)
    {
        const float t = static_cast<float>(slice) / static_cast<float>(CLUSTER_SLICES);
        if (even)
            return sliceNear + (sliceFar - sliceNear) * t;
        return sliceNear * std::pow(sliceFar / sliceNear, t);
    }

    // The slice that holds a view depth, or -1 outside sliceNear..sliceFar, where no local light reaches.
    int32_t getClusterSlice(float depth, float sliceNear, float sliceFar, bool even)
    {
        if (!(sliceFar > sliceNear) || !(depth >= sliceNear) || depth > sliceFar || (!even && !(sliceNear > 0.0f)))
            return -1;
        const float t = even ? (depth - sliceNear) / (sliceFar - sliceNear) : std::log(depth / sliceNear) / std::log(sliceFar / sliceNear);
        return std::min(static_cast<int32_t>(t * static_cast<float>(CLUSTER_SLICES)), static_cast<int32_t>(CLUSTER_SLICES) - 1);
    }

    bool Engine::createGpuLights()
    {
        // A row per light handle, and handles start at 1.
        if (config.maxLights == 0 || config.maxLights == UINT32_MAX)
        {
            spdlog::error("EngineConfig::maxLights is {}; it must be at least 1 and less than {}.", config.maxLights, UINT32_MAX);
            return false;
        }
        if (!gpuLights->lights.create(ctx, "Light Table", sizeof(gfx::Light), config.maxLights + 1))
        {
            spdlog::error("Failed to create the light table of EngineConfig::maxLights = {} lights ({} B each, one copy per frame slot). Lower maxLights.",
                          config.maxLights, sizeof(gfx::Light));
            return false;
        }
        return true;
    }

    void Engine::writeLightRow(LightHandle light)
    {
        // Only an engine that failed to start has no table.
        if (light.index >= gpuLights->lights.capacity())
            return;
        const Light &lightRef = registry.lights[light.index];
        gfx::Light row{.position = lightRef.position,
                       .intensity = lightRef.intensity,
                       .color = lightRef.color,
                       .range = lightRef.range,
                       .direction = lightRef.direction,
                       .type = static_cast<gfx::LightType>(lightRef.type)};
        setSpotCone(lightRef, row);
        gpuLights->lights.setRow(static_cast<uint32_t>(light.index), &row);
    }

    void Engine::markLightListsDirty(LightHandle light)
    {
        const uint16_t scenes = light.index < gpuLights->lightScenes.size() ? gpuLights->lightScenes[light.index] : 0;
        for (uint32_t i = 0; i < MAX_SCENES; ++i)
        {
            if ((scenes & (1u << i)) != 0)
                gpuLights->lists[i].rebuild = true;
        }
    }

    // Runs in syncGpuScene, after the slot's fence wait. Only the scenes whose lights changed rebuild their lists, and
    // only changed light rows are uploaded: nothing here walks every light every frame.
    void Engine::syncGpuLights()
    {
        ZoneScoped;

        const auto frame = static_cast<uint32_t>(frameCount);
        const auto bit = static_cast<uint8_t>(1u << frame);
        for (uint32_t i = 0; i < MAX_SCENES && i < registry.scenes.size(); ++i)
        {
            SceneLightList &list = gpuLights->lists[i];
            if (list.rebuild)
            {
                // Removing a light clears its scene bit only, so the scene's list is compacted here: an entry stays while
                // its light lives and is in the scene, once.
                Scene &scene = registry.scenes[i];
                const uint32_t stamp = ++gpuLights->rebuildStamp;
                gpuLights->listStamps.resize(registry.lights.size(), 0);
                std::erase_if(scene.lights, [this, i, stamp](const LightHandle &light)
                              {
                                  if (!isLive(light) || light.index >= gpuLights->lightScenes.size() || (gpuLights->lightScenes[light.index] & (1u << i)) == 0 ||
                                      gpuLights->listStamps[light.index] == stamp)
                                      return true;
                                  gpuLights->listStamps[light.index] = stamp;
                                  return false; });
                list.globalCount = buildSceneLightList(registry.lights.data(), scene.lights.data(), static_cast<uint32_t>(scene.lights.size()), list.rows);
                list.rebuild = false;
                list.staleSlots = static_cast<uint8_t>((1u << MAX_FRAMES_IN_FLIGHT) - 1);
            }
            if ((list.staleSlots & bit) == 0 || list.rows.empty())
                continue;

            // Only this slot's last frame used the old buffer, and the fence wait covered it.
            const auto rowCount = static_cast<uint32_t>(list.rows.size());
            BufferHandle &buffer = list.buffers[frame];
            uint32_t &capacity = list.bufferCapacity[frame];
            if (capacity < rowCount)
            {
                if (buffer.isValid())
                    ctx.destroyBuffer(buffer);
                capacity = std::max(rowCount, capacity * 2);
                buffer = ctx.createBuffer({.name = "Scene Light Buffer", .usage = BufferUsage::STORAGE, .size = static_cast<uint64_t>(capacity) * sizeof(uint32_t)});
                if (!buffer.isValid())
                {
                    spdlog::error("Failed to create a scene light buffer of {} lights.", capacity);
                    capacity = 0;
                    continue;
                }
            }
            if (ctx.fillBuffer(buffer, list.rows.data(), list.rows.size() * sizeof(uint32_t), 0))
                list.staleSlots &= static_cast<uint8_t>(~bit);
        }
        gpuLights->lights.upload(ctx, frame);
    }

    // A shaded view's clusters, one buffer per frame slot. Without them its pixels shade every light.
    void Engine::createViewClusters(uint32_t viewIndex)
    {
        ViewClusters &clusters = gpuLights->views[viewIndex];
        clusters = {};
        if (!config.clusteredLighting)
            return;
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            clusters.buffers[i] = ctx.createBuffer({.name = "Light Cluster Buffer", .usage = BufferUsage::STORAGE, .size = getClusterBufferSize(FIRST_LIGHT_CAPACITY)});
            clusters.lightCapacity[i] = FIRST_LIGHT_CAPACITY;
            if (!clusters.buffers[i].isValid())
            {
                spdlog::error("Failed to create the light clusters of a scene view; its pixels shade every light.");
                releaseViewClusters(viewIndex);
                return;
            }
        }
    }

    void Engine::releaseViewClusters(uint32_t viewIndex)
    {
        ViewClusters &clusters = gpuLights->views[viewIndex];
        for (auto &buffer : clusters.buffers)
        {
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
        }
        clusters = {};
    }

    // The scene's lights in the view's SceneData, and whether its clusters are built this frame. Shadowed lights are
    // already in it: the clusters leave them out, since the view shades them with their shadows.
    void Engine::writeSceneLights(uint32_t viewIndex, SceneHandle scene, gfx::SceneData &sceneData)
    {
        const auto frame = static_cast<uint32_t>(frameCount);
        ViewClusters &clusters = gpuLights->views[viewIndex];
        clusters.active = false;

        // A list that this slot could not upload leaves the view without its lights for a frame.
        const SceneLightList &list = gpuLights->lists[scene.index];
        sceneData.lightsPtr = ctx.getBufferDeviceAddress(gpuLights->lights.buffer(frame));
        if ((list.staleSlots & (1u << frame)) != 0 || list.rows.empty())
            return;
        sceneData.sceneLightsPtr = getAddress(ctx, list.buffers[frame]);
        sceneData.globalLightCount = list.globalCount;
        sceneData.localLightCount = static_cast<uint32_t>(list.rows.size()) - list.globalCount;
        LightTotals &totals = gpuLights->totals[frame];
        totals.global += sceneData.globalLightCount;

        const uint32_t localCount = sceneData.localLightCount;
        if (localCount == 0)
            return;
        // A shader error retries every frame; meanwhile the pixels shade every light.
        if (clusters.buffers[frame].isValid() && (!gpuLights->lightsPipeline.isValid() || !gpuLights->tilesPipeline.isValid() || !gpuLights->clustersPipeline.isValid()))
        {
            const std::string source = readFile(pathFromUtf8(config.shaderDir) / "passes/lightClusters.slang");
            gpuLights->lightsPipeline = createComputePipeline("lightCull", source, "lightsMain");
            gpuLights->tilesPipeline = createComputePipeline("lightTiles", source, "tilesMain");
            gpuLights->clustersPipeline = createComputePipeline("lightClusters", source, "clustersMain");
            if (!gpuLights->lightsPipeline.isValid() || !gpuLights->tilesPipeline.isValid() || !gpuLights->clustersPipeline.isValid())
                spdlog::error("Failed to create the light cluster pipelines; every pixel shades every light.");
        }
        uint32_t &capacity = clusters.lightCapacity[frame];
        if (clusters.buffers[frame].isValid() && capacity < localCount)
        {
            const uint32_t grown = std::max(localCount, capacity * 2);
            if (ctx.resizeBuffer(clusters.buffers[frame], getClusterBufferSize(grown)))
                capacity = grown;
            else
                spdlog::error("Failed to grow a light cluster buffer to {} lights; every pixel shades every light.", grown);
        }
        clusters.active = clusters.buffers[frame].isValid() && capacity >= localCount && gpuLights->lightsPipeline.isValid() && gpuLights->tilesPipeline.isValid() &&
                          gpuLights->clustersPipeline.isValid();
        if (clusters.active)
            sceneData.clustersPtr = ctx.getBufferDeviceAddress(clusters.buffers[frame]);
        else
            totals.unclustered += localCount;
    }

    // lightsMain puts each local light's sphere in view space and keeps those in the frustum, tilesMain lists the
    // spheres that touch each screen tile's column, and clustersMain the ones of its tile that touch each cluster's box.
    void Engine::recordLightClusters(uint32_t viewIndex, uint64_t sceneDataAddress)
    {
        if (!gpuLights->views[viewIndex].active)
            return;
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const uint32_t timer = beginGpuTimer(commandBuffer, "Light Clusters");
        const gfx::PushConstants pushConstants{.scene = sceneDataAddress};
        ctx.bindPipeline(commandBuffer, gpuLights->lightsPipeline);
        ctx.pushConstants(commandBuffer, gpuLights->lightsPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, 1);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuLights->tilesPipeline);
        ctx.pushConstants(commandBuffer, gpuLights->tilesPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, CLUSTER_TILES_X * CLUSTER_TILES_Y);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuLights->clustersPipeline);
        ctx.pushConstants(commandBuffer, gpuLights->clustersPipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, CLUSTER_COUNT / CLUSTER_GROUP_SIZE);
        endGpuTimer(commandBuffer, timer);
    }

    static_assert(CLUSTER_COUNT % CLUSTER_GROUP_SIZE == 0, "clustersMain has a thread per cluster in whole groups");
}
