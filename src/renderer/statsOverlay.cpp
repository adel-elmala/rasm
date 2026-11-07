#include "renderer/statsOverlay.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/frameStats.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace rasm
{
    namespace
    {
        constexpr uint32_t MAX_TEXT_COLUMNS = 40;
        // Frame, GPU and its timers (one is kept for culling), Draws, Culled, Triangles, two LOD lines, LOD draws
        // (8 counts of up to 8 characters wrap at most twice) and VRAM.
        constexpr uint32_t MAX_TEXT_ROWS = 2 + (MAX_GPU_TIMERS - 1) + 3 + 2 + 3 + 1;
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

        std::string buildText(const FrameStats &stats, int32_t forcedLod)
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

            if (forcedLod >= 0)
                std::snprintf(line, sizeof(line), "LOD       forced %d (F2)\n", forcedLod);
            else
                std::snprintf(line, sizeof(line), "LOD       by distance (F2)\n");
            text += line;
            const uint64_t saved = stats.trianglesVisibleLod0 > stats.trianglesVisible ? stats.trianglesVisibleLod0 - stats.trianglesVisible : 0;
            const double savedPercent = stats.trianglesVisibleLod0 > 0 ? 100.0 * static_cast<double>(saved) / static_cast<double>(stats.trianglesVisibleLod0) : 0.0;
            std::snprintf(line, sizeof(line), "LOD saved %s (%.0f%%)\n", formatCount(saved).c_str(), savedPercent);
            text += line;

            // The visible draws at LOD 0, 1, 2, ... up to the coarsest one in use, wrapped to the overlay's width.
            uint32_t lodsUsed = 1;
            for (uint32_t i = 0; i < MAX_MESH_LODS; ++i)
                lodsUsed = stats.drawsPerLod[i] > 0 ? i + 1 : lodsUsed;
            std::string lodLine = "LOD draws";
            for (uint32_t i = 0; i < lodsUsed; ++i)
            {
                const std::string count = formatCount(stats.drawsPerLod[i]);
                if (lodLine.size() + 1 + count.size() > MAX_TEXT_COLUMNS)
                {
                    text += lodLine + "\n";
                    lodLine = "         ";
                }
                lodLine += " " + count;
            }
            text += lodLine + "\n";
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

    // A shader that fails to compile only leaves the overlay off.
    bool Engine::createStatsOverlay()
    {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            StatsOverlaySlot &slot = statsOverlay->slots[i];
            slot.text = ctx.createBuffer({.name = "Stats Overlay Text " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = MAX_TEXT_COLUMNS * MAX_TEXT_ROWS, .hostVisible = true});
            if (!slot.text.isValid())
            {
                spdlog::error("Failed to create the stats overlay's text buffer.");
                return false;
            }
        }

        statsOverlay->pipeline = createPipeline("statsOverlay", readFile(pathFromUtf8(config.shaderDir) / "passes/statsOverlay.slang"));
        if (!statsOverlay->pipeline.isValid())
            spdlog::error("Failed to create the stats overlay pipeline; the overlay stays off.");
        return true;
    }

    // Runs after updateFrameStats. Each slot draws from its own buffer, so both get the current text every frame.
    void Engine::updateStatsOverlay()
    {
        StatsOverlaySlot &slot = statsOverlay->slots[frameCount];
        slot.rows = 0;
        if (!config.showStats || !statsOverlay->pipeline.isValid())
            return;

        const auto now = std::chrono::steady_clock::now();
        if (statsOverlay->grid.empty() || now - statsOverlay->gridTime >= TEXT_REFRESH)
        {
            statsOverlay->gridTime = now;
            buildGrid(buildText(frameStats->stats, config.forcedLod), statsOverlay->grid, statsOverlay->columns, statsOverlay->rows);
        }
        if (ctx.fillBuffer(slot.text, statsOverlay->grid.data(), statsOverlay->grid.size()))
        {
            slot.columns = statsOverlay->columns;
            slot.rows = statsOverlay->rows;
        }
    }

    // Records into the current swapchain pass, before it ends. Every swapchain pass clears, so drawing it in each
    // one keeps it on the presented image.
    void Engine::drawStatsOverlay()
    {
        const StatsOverlaySlot &slot = statsOverlay->slots[frameCount];
        if (!config.showStats || slot.rows == 0)
            return;

        const uint32_t scale = std::max(1u, swapchain.height / 1080);
        const OverlayPushConstants pushConstants{.text = ctx.getBufferDeviceAddress(slot.text),
                                                 .grid = slot.columns | slot.rows << 8 | scale << 16,
                                                 .screen = swapchain.width | swapchain.height << 16};
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        // The shader places the panel in window pixels, whatever viewport the pass left set, and its triangles
        // wind clockwise on screen.
        ctx.setViewport(commandBuffer, 0.0f, 0.0f, static_cast<float>(swapchain.width), static_cast<float>(swapchain.height));
        ctx.setScissor(commandBuffer, 0, 0, swapchain.width, swapchain.height);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        ctx.bindPipeline(commandBuffer, statsOverlay->pipeline);
        ctx.pushConstants(commandBuffer, statsOverlay->pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
        ctx.draw(commandBuffer, 6);
    }
}
