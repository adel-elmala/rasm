#include "renderer/frameStats.h"
#include "renderer/gpuScene.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <string>

namespace rasm
{
    namespace
    {
        // Returns UINT32_MAX when the slot isn't measured, the GPU has no timestamps or the frame has used limit timers.
        uint32_t beginTimer(RenderContext &ctx, FrameStatsCollector &collector, uint32_t frame, const CommandBufferHandle &commandBuffer, const std::string &name, uint32_t limit)
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
            ctx.writeTimestamp(commandBuffer, slot.timestamps, 2 * timer);
            return timer;
        }

        // A 64-bit count that cull.slang adds up in two words, low first.
        uint64_t readCount64(const uint32_t *words)
        {
            return static_cast<uint64_t>(words[1]) << 32 | words[0];
        }
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
            // Timer 0 is the frame's command buffer. Culling's command buffer runs before it, so the frame's GPU
            // time goes from the earliest timestamp (0) to the latest, and the timers are listed by start time.
            uint64_t nanoseconds[2 * MAX_GPU_TIMERS];
            if (slot.timerCount > 0 && ctx.getTimestamps(slot.timestamps, 2 * slot.timerCount, nanoseconds))
            {
                uint64_t end = 0;
                for (uint32_t i = 0; i < slot.timerCount; ++i)
                    end = std::max(end, nanoseconds[2 * i + 1]);
                stats.gpuFrameMs = static_cast<float>(static_cast<double>(end) * 1e-6);

                uint32_t order[MAX_GPU_TIMERS];
                for (uint32_t i = 1; i < slot.timerCount; ++i)
                    order[i - 1] = i;
                stats.gpuTimerCount = slot.timerCount - 1;
                std::sort(order, order + stats.gpuTimerCount, [&](uint32_t a, uint32_t b)
                          { return nanoseconds[2 * a] < nanoseconds[2 * b]; });
                for (uint32_t i = 0; i < stats.gpuTimerCount; ++i)
                {
                    const uint32_t timer = order[i];
                    stats.gpuTimers[i] = {.name = slot.timerNames[timer],
                                          .milliseconds = static_cast<float>(static_cast<double>(nanoseconds[2 * timer + 1] - nanoseconds[2 * timer]) * 1e-6)};
                }
            }

            // Culling counted the visible draws and triangles of the culled calls; unculled calls drew all of theirs.
            const DrawTotals &totals = gpuScene->drawTotals[frame];
            // Unculled calls draw LOD 0.
            uint32_t culled[CULL_STATS_WORDS] = {};
            ctx.readBuffer(gpuScene->cullStats[frame], culled, sizeof(culled));
            stats.draws = totals.draws;
            stats.drawsVisible = culled[CULL_STATS_DRAWS] + totals.unculledDraws;
            stats.triangles = totals.triangles;
            stats.trianglesVisible = readCount64(culled + CULL_STATS_TRIANGLES) + totals.unculledTriangles;
            stats.trianglesVisibleLod0 = readCount64(culled + CULL_STATS_LOD0_TRIANGLES) + totals.unculledTriangles;
            for (uint32_t i = 0; i < MAX_MESH_LODS; ++i)
                stats.drawsPerLod[i] = culled[CULL_STATS_LOD_DRAWS + i];
            stats.drawsPerLod[0] += totals.unculledDraws;
        }
        slot.readFenceValue = fenceValue;
        stats.cpuFrameMs = deltaTime * 1000.0f;

        slot.timerCount = 0;
        gpuScene->drawTotals[frame] = {};
        slot.measuring = config.showStats || frameStats->requested;
        if (!slot.measuring)
            return;

        stats.memory = ctx.queryVRAMStats();
        if (slot.timestamps.isValid())
            ctx.resetQueries(slot.timestamps);
        // Culling adds to the counts even in frames that aren't measured.
        const uint32_t zeros[CULL_STATS_WORDS] = {};
        ctx.fillBuffer(gpuScene->cullStats[frame], zeros, sizeof(zeros));
    }

    // The last timer is kept for culling, which endFrame records after every pass.
    uint32_t Engine::beginGpuTimer(const CommandBufferHandle &commandBuffer, const std::string &name)
    {
        return beginTimer(ctx, *frameStats, static_cast<uint32_t>(frameCount), commandBuffer, name, MAX_GPU_TIMERS - 1);
    }

    uint32_t Engine::beginCullingTimer(const CommandBufferHandle &commandBuffer)
    {
        return beginTimer(ctx, *frameStats, static_cast<uint32_t>(frameCount), commandBuffer, "Culling", MAX_GPU_TIMERS);
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
