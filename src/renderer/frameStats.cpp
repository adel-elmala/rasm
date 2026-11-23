#include "renderer/frameStats.h"
#include "renderer/gpuScene.h"
#include "renderer/lightClusters.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace rasm
{
    namespace
    {
        // Returns UINT32_MAX when the slot isn't measured, the GPU has no timestamps or the frame has used limit timers.
        uint32_t beginTimer(RenderContext &ctx, FrameStatsCollector &collector, uint32_t frame, const CommandBufferHandle &commandBuffer, const std::string &name, bool onCompute, uint32_t limit)
        {
            FrameStatsSlot &slot = collector.slots[frame];
            if (!slot.measuring || !slot.timestamps.isValid())
                return UINT32_MAX;
            if (slot.timerCount >= limit)
            {
                if (!collector.timersFullLogged)
                    spdlog::warn("A frame used all {} GPU timers: '{}' and the timers after it are not measured.", limit, name);
                collector.timersFullLogged = true;
                return UINT32_MAX;
            }

            const uint32_t timer = slot.timerCount++;
            slot.timerNames[timer] = name;
            slot.timerOnCompute[timer] = onCompute;
            ctx.writeTimestamp(commandBuffer, slot.timestamps, 2 * timer);
            return timer;
        }

        // A 64-bit count that cull.slang adds up in two words, low first.
        uint64_t readCount64(const uint32_t *words)
        {
            return static_cast<uint64_t>(words[1]) << 32 | words[0];
        }

        constexpr std::chrono::seconds LIGHTS_DROPPED_WARNING(10);
    }

    std::string formatCount(uint64_t count)
    {
        char text[32];
        if (count >= 1000000)
            std::snprintf(text, sizeof(text), "%.2fM", static_cast<double>(count) / 1e6);
        else if (count >= 10000)
            std::snprintf(text, sizeof(text), "%.1fK", static_cast<double>(count) / 1e3);
        else
            std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(count));
        return text;
    }

    // Without timestamps the stats only lack GPU times.
    void Engine::createFrameStats()
    {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
            frameStats->slots[i].timestamps = ctx.createTimestampQueryPool("GPU Timers " + std::to_string(i), 2 * MAX_GPU_TIMERS);
    }

    // Runs right after the fence wait for the frame's slot: it reads what the slot's last frame measured, then
    // resets the slot's timers and counts for this frame. Frames are measured only while the overlay is shown
    // or once getFrameStats was called.
    void Engine::updateFrameStats()
    {
        ZoneScoped;

        const auto frame = static_cast<uint32_t>(frameCount);
        FrameStatsSlot &slot = frameStats->slots[frame];
        FrameStats &stats = frameStats->stats;

        // The slot's fence value moves only when a frame in it was submitted, so a frame that was recorded but not
        // submitted is skipped: its queries were never written.
        const uint64_t fenceValue = frameResources[frame].fenceValue;
        if (slot.measuring && fenceValue != slot.readFenceValue)
        {
            // Timer 0 spans the frame on the graphics queue and its begin is the first query, so the frame's GPU time
            // runs from 0 to the latest end there. A timestamp of another queue may count from another origin, so an
            // async compute timer is left out of it, and its time is read from its own two queries. They are listed
            // in the order they were recorded, which is the order each queue runs them.
            uint64_t nanoseconds[2 * MAX_GPU_TIMERS];
            if (slot.timerCount > 0 && ctx.getTimestamps(slot.timestamps, 0, 2 * slot.timerCount, nanoseconds))
            {
                uint64_t end = 0;
                for (uint32_t i = 0; i < slot.timerCount; ++i)
                {
                    if (!slot.timerOnCompute[i])
                        end = std::max(end, nanoseconds[2 * i + 1]);
                }
                stats.gpuFrameMs = static_cast<float>(static_cast<double>(end) * 1e-6);

                stats.gpuTimerCount = slot.timerCount - 1;
                for (uint32_t i = 0; i < stats.gpuTimerCount; ++i)
                {
                    const uint32_t timer = i + 1;
                    uint64_t pair[2] = {nanoseconds[2 * timer], nanoseconds[2 * timer + 1]};
                    if (slot.timerOnCompute[timer] && !ctx.getTimestamps(slot.timestamps, 2 * timer, 2, pair))
                        pair[1] = pair[0];
                    stats.gpuTimers[i] = {.name = slot.timerNames[timer],
                                          .milliseconds = static_cast<float>(static_cast<double>(pair[1] - std::min(pair[0], pair[1])) * 1e-6)};
                }
            }

            // Culling counted the visible draws and triangles of the culled views; unculled views drew all of theirs.
            const DrawTotals &totals = gpuScene->drawTotals[frame];
            // Unculled views draw LOD 0.
            uint32_t culled[CULL_STATS_WORDS] = {};
            ctx.readBuffer(gpuScene->cullStats[frame], culled, sizeof(culled));
            const LightTotals &lightTotals = gpuLights->totals[frame];
            stats.lightsGlobal = lightTotals.global;
            stats.lightsVisible = culled[CULL_STATS_LIGHTS_VISIBLE] + lightTotals.unclustered;
            stats.lightsDropped = culled[CULL_STATS_LIGHTS_DROPPED];
            stats.draws = totals.draws;
            stats.drawsVisible = culled[CULL_STATS_DRAWS] + totals.unculledDraws;
            stats.triangles = totals.triangles;
            stats.trianglesVisible = readCount64(culled + CULL_STATS_TRIANGLES) + totals.unculledTriangles;
            stats.trianglesVisibleLod0 = readCount64(culled + CULL_STATS_LOD0_TRIANGLES) + totals.unculledTriangles;
            for (uint32_t i = 0; i < MAX_MESH_LODS; ++i)
                stats.drawsPerLod[i] = culled[CULL_STATS_LOD_DRAWS + i];
            stats.drawsPerLod[0] += totals.unculledDraws;
            stats.meshletsVisible = culled[CULL_STATS_MESHLETS];
            stats.meshletsCulled = culled[CULL_STATS_MESHLETS_CULLED];
            stats.meshletDrawsWhole = culled[CULL_STATS_MESHLETS_WHOLE];
        }
        // Full clusters are reported in every frame, measured or not, since they drop light from the image. The
        // warning comes once, and again only while it goes on, every LIGHTS_DROPPED_WARNING.
        if (fenceValue != slot.readFenceValue)
        {
            uint32_t dropped = 0;
            ctx.readBuffer(gpuScene->cullStats[frame], &dropped, sizeof(dropped), CULL_STATS_LIGHTS_DROPPED * sizeof(uint32_t));
            const auto now = std::chrono::steady_clock::now();
            if (dropped > 0 && (!frameStats->lightsDroppedLogged || now - frameStats->lightsDroppedTime > LIGHTS_DROPPED_WARNING))
            {
                spdlog::warn("{} lights were left out of full light clusters (MAX_CLUSTER_LIGHTS, {}) or full screen tiles ({} lights), so some pixels "
                             "miss them. Use fewer lights there, or shorter ranges.",
                             dropped, MAX_CLUSTER_LIGHTS, CLUSTER_SLICES * MAX_CLUSTER_LIGHTS);
                frameStats->lightsDroppedLogged = true;
                frameStats->lightsDroppedTime = now;
            }
        }
        slot.readFenceValue = fenceValue;
        stats.cpuFrameMs = frameTime * 1000.0f;
        // Every frame, measured or not: the copies' time is summed as their batches finish, on whatever queue.
        stats.uploadGpuMs = ctx.takeUploadGpuMs();

        slot.timerCount = 0;
        gpuScene->drawTotals[frame] = {};
        gpuLights->totals[frame] = {};
        // Culling and the light clusters add to the counts in every frame.
        const uint32_t zeros[CULL_STATS_WORDS] = {};
        ctx.fillBuffer(gpuScene->cullStats[frame], zeros, sizeof(zeros));
        slot.measuring = config.showStats || frameStats->requested;
        if (!slot.measuring)
            return;

        stats.memory = ctx.queryVRAMStats();
        if (slot.timestamps.isValid())
            ctx.resetQueries(slot.timestamps);
    }

    uint32_t Engine::beginGpuTimer(const CommandBufferHandle &commandBuffer, const std::string &name)
    {
        const bool onCompute = asyncCompute && commandBuffer == frameResources[frameCount].commandBuffer && recordingQueue == QueueType::COMPUTE;
        return beginTimer(ctx, *frameStats, static_cast<uint32_t>(frameCount), commandBuffer, name, onCompute, MAX_GPU_TIMERS);
    }

    void Engine::endGpuTimer(const CommandBufferHandle &commandBuffer, uint32_t timer)
    {
        if (timer != UINT32_MAX)
            ctx.writeTimestamp(commandBuffer, frameStats->slots[frameCount].timestamps, 2 * timer + 1);
    }

    const FrameStats &Engine::getFrameStats() const
    {
        frameStats->requested = true;
        return frameStats->stats;
    }
}
