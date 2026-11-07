#include "renderer/statsOverlay.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/gpuScene.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace rasm
{
    namespace
    {
        constexpr uint32_t MAX_TEXT_COLUMNS = 40;
        constexpr uint32_t MAX_TEXT_ROWS = 24;
        // Slow enough for the numbers to be read.
        constexpr std::chrono::milliseconds TEXT_REFRESH(250);

        // Must match PushConstants in shaders/passes/statsOverlay.slang.
        struct OverlayPushConstants
        {
            uint64_t text;
            uint32_t grid;
            uint32_t screen;
        };
        static_assert(sizeof(OverlayPushConstants) == PUSH_CONSTANTS_SIZE, "OverlayPushConstants must fill the push-constant range");
    }

    namespace
    {
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

        std::string buildText(const FrameStats &stats)
        {
            char line[128];
            std::string text;
            const float fps = stats.cpuFrameMs > 0.0f ? 1000.0f / stats.cpuFrameMs : 0.0f;
            std::snprintf(line, sizeof(line), "Frame     %6.2f ms %5.0f fps\n", stats.cpuFrameMs, fps);
            text += line;
            if (stats.gpuTimerCount > 0)
            {
                std::snprintf(line, sizeof(line), "GPU       %6.2f ms\n", stats.gpuFrameMs);
                text += line;
                for (uint32_t i = 0; i < stats.gpuTimerCount; ++i)
                {
                    std::snprintf(line, sizeof(line), " %-8.8s %6.2f ms\n", stats.gpuTimers[i].name.c_str(), stats.gpuTimers[i].milliseconds);
                    text += line;
                }
            }
            std::snprintf(line, sizeof(line), "Draws     %s / %s\n", formatCount(stats.drawsVisible).c_str(), formatCount(stats.draws).c_str());
            text += line;
            std::snprintf(line, sizeof(line), "Culled    %s\n", formatCount(stats.draws - stats.drawsVisible).c_str());
            text += line;
            std::snprintf(line, sizeof(line), "Triangles %s / %s\n", formatCount(stats.trianglesVisible).c_str(), formatCount(stats.triangles).c_str());
            text += line;
            std::snprintf(line, sizeof(line), "VRAM      %llu / %llu MiB\n", static_cast<unsigned long long>(stats.memory.vramUsage >> 20),
                          static_cast<unsigned long long>(stats.memory.vramBudget >> 20));
            text += line;
            return text;
        }

        // Lays the lines out in a grid as wide as the longest one, padded with spaces, which the shader leaves blank.
        void buildGrid(const std::string &text, std::string &grid, uint32_t &columns, uint32_t &rows)
        {
            columns = 0;
            rows = 0;
            for (size_t start = 0; start < text.size() && rows < MAX_TEXT_ROWS; ++rows)
            {
                const size_t end = std::min(text.find('\n', start), text.size());
                columns = std::max(columns, std::min(static_cast<uint32_t>(end - start), MAX_TEXT_COLUMNS));
                start = end + 1;
            }

            grid.assign(static_cast<size_t>(columns) * rows, ' ');
            size_t start = 0;
            for (uint32_t row = 0; row < rows; ++row)
            {
                const size_t end = std::min(text.find('\n', start), text.size());
                const size_t length = std::min(end - start, static_cast<size_t>(columns));
                grid.replace(static_cast<size_t>(row) * columns, length, text, start, length);
                start = end + 1;
            }
        }
    }

    bool Engine::createStatsOverlay()
    {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            StatsFrameSlot &slot = statsOverlay->slots[i];
            // Without timestamps the stats only lack GPU times.
            slot.timestamps = ctx.createTimestampQueryPool("GPU Timers " + std::to_string(i), 2 * MAX_GPU_TIMERS);
            slot.text = ctx.createBuffer({.name = "Stats Overlay Text " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = MAX_TEXT_COLUMNS * MAX_TEXT_ROWS, .hostVisible = true});
            if (!slot.text.isValid())
            {
                spdlog::error("Failed to create the stats overlay's text buffer.");
                return false;
            }
        }
        return true;
    }

    // Runs right after the fence wait for the frame's slot: it reads what the slot's last frame measured, then
    // resets the slot's timers and counts for this frame, and writes the overlay's text into the slot's buffer.
    void Engine::updateFrameStats()
    {
        ZoneScoped;

        const auto frame = static_cast<uint32_t>(frameCount);
        StatsFrameSlot &slot = statsOverlay->slots[frame];
        FrameStats &stats = statsOverlay->stats;

        // The slot's fence value moves only when a frame in it was submitted, so a frame that was recorded but not
        // submitted is skipped: its queries were never written.
        const uint64_t fenceValue = frameResources[frame].fenceValue;
        if (fenceValue != slot.readFenceValue)
        {
            slot.readFenceValue = fenceValue;

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
            uint32_t culled[2] = {};
            ctx.readBuffer(gpuScene->cullStats[frame], culled, sizeof(culled));
            stats.draws = totals.draws;
            stats.drawsVisible = culled[0] + totals.unculledDraws;
            stats.triangles = totals.triangles;
            stats.trianglesVisible = culled[1] + totals.unculledTriangles;
        }
        stats.cpuFrameMs = deltaTime * 1000.0f;
        stats.memory = ctx.queryVRAMStats();

        slot.timerCount = 0;
        if (slot.timestamps.isValid())
            ctx.resetQueries(slot.timestamps);
        const uint32_t zeros[2] = {};
        ctx.fillBuffer(gpuScene->cullStats[frame], zeros, sizeof(zeros));
        gpuScene->drawTotals[frame] = {};

        if (!config.showStats)
            return;

        // Each slot draws from its own buffer, so both get the current text every frame.
        const auto now = std::chrono::steady_clock::now();
        if (statsOverlay->grid.empty() || now - statsOverlay->gridTime >= TEXT_REFRESH)
        {
            statsOverlay->gridTime = now;
            buildGrid(buildText(stats), statsOverlay->grid, statsOverlay->columns, statsOverlay->rows);
        }
        slot.rows = 0;
        if (ctx.fillBuffer(slot.text, statsOverlay->grid.data(), statsOverlay->grid.size()))
        {
            slot.columns = statsOverlay->columns;
            slot.rows = statsOverlay->rows;
        }
    }

    // Returns UINT32_MAX, which endGpuTimer ignores, when the GPU has no timestamps or the frame has used every timer.
    uint32_t Engine::beginGpuTimer(CommandBufferHandle commandBuffer, const std::string &name)
    {
        StatsFrameSlot &slot = statsOverlay->slots[frameCount];
        if (!slot.timestamps.isValid() || slot.timerCount >= MAX_GPU_TIMERS)
            return UINT32_MAX;

        const uint32_t timer = slot.timerCount++;
        slot.timerNames[timer] = name;
        ctx.writeTimestamp(commandBuffer, slot.timestamps, 2 * timer);
        return timer;
    }

    void Engine::endGpuTimer(CommandBufferHandle commandBuffer, uint32_t timer)
    {
        if (timer != UINT32_MAX)
            ctx.writeTimestamp(commandBuffer, statsOverlay->slots[frameCount].timestamps, 2 * timer + 1);
    }

    // Records into the current swapchain pass, before it ends. Every swapchain pass clears, so drawing it in each
    // one keeps it on the presented image.
    void Engine::drawStatsOverlay()
    {
        const StatsFrameSlot &slot = statsOverlay->slots[frameCount];
        if (!config.showStats || slot.rows == 0)
            return;

        // A shader error retries every frame, so a shader fixed on disk is picked up while the application runs.
        if (!statsOverlay->pipeline.isValid())
        {
            statsOverlay->pipeline = createPipeline("statsOverlay", readFile(pathFromUtf8(config.shaderDir) / "passes/statsOverlay.slang"));
            if (!statsOverlay->pipeline.isValid())
            {
                spdlog::error("Failed to create the stats overlay pipeline.");
                return;
            }
        }

        const uint32_t scale = std::max(1u, swapchain.height / 1080);
        const OverlayPushConstants pushConstants{.text = ctx.getBufferDeviceAddress(slot.text),
                                                 .grid = slot.columns | slot.rows << 8 | scale << 16,
                                                 .screen = swapchain.width | swapchain.height << 16};
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        ctx.bindPipeline(commandBuffer, statsOverlay->pipeline);
        ctx.pushConstants(commandBuffer, statsOverlay->pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
        ctx.draw(commandBuffer, 6);
    }

    const FrameStats &Engine::getFrameStats() const
    {
        return statsOverlay->stats;
    }
}
