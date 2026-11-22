#include "rasm/engine.h"
#include "renderer/frameStats.h"
#include "renderer/ui.h"

#include "imgui.h"

#include <cfloat>
#include <chrono>
#include <string>

namespace rasm
{
    namespace
    {
        // Slow enough for the numbers to be read, like the stats overlay's.
        constexpr std::chrono::milliseconds STATS_REFRESH(250);
    }

    namespace
    {
        void drawStatsSection(const FrameStats &stats)
        {
            const float fps = stats.cpuFrameMs > 0.0f ? 1000.0f / stats.cpuFrameMs : 0.0f;
            ImGui::Text("Frame   %6.2f ms  %4.0f fps", stats.cpuFrameMs, fps);
            if (stats.gpuTimerCount == 0)
                return;
            ImGui::Text("GPU     %6.2f ms", stats.gpuFrameMs);
            if (ImGui::BeginTable("timers", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                for (uint32_t i = 0; i < stats.gpuTimerCount; ++i)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(stats.gpuTimers[i].name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%6.2f ms", stats.gpuTimers[i].milliseconds);
                }
                ImGui::EndTable();
            }
        }

        void drawCullingSection(const FrameStats &stats)
        {
            ImGui::Text("Draws      %s / %s", formatCount(stats.drawsVisible).c_str(), formatCount(stats.draws).c_str());
            ImGui::Text("Culled     %s", formatCount(stats.draws - stats.drawsVisible).c_str());
            ImGui::Text("Triangles  %s / %s", formatCount(stats.trianglesVisible).c_str(), formatCount(stats.triangles).c_str());
            const uint64_t saved = stats.trianglesVisibleLod0 > stats.trianglesVisible ? stats.trianglesVisibleLod0 - stats.trianglesVisible : 0;
            const double savedPercent = stats.trianglesVisibleLod0 > 0 ? 100.0 * static_cast<double>(saved) / static_cast<double>(stats.trianglesVisibleLod0) : 0.0;
            ImGui::Text("LOD saved  %s (%.0f%%)", formatCount(saved).c_str(), savedPercent);
            std::string lodDraws = "LOD draws ";
            uint32_t lodsUsed = 1;
            for (uint32_t i = 0; i < MAX_MESH_LODS; ++i)
                lodsUsed = stats.drawsPerLod[i] > 0 ? i + 1 : lodsUsed;
            for (uint32_t i = 0; i < lodsUsed; ++i)
                lodDraws += " " + formatCount(stats.drawsPerLod[i]);
            ImGui::TextUnformatted(lodDraws.c_str());
            ImGui::Text("Lights     %s visible, %s global", formatCount(stats.lightsVisible).c_str(), formatCount(stats.lightsGlobal).c_str());
            if (stats.lightsDropped > 0)
                ImGui::Text("Dropped    %s (full clusters)", formatCount(stats.lightsDropped).c_str());
            ImGui::Text("VRAM       %llu / %llu MiB", static_cast<unsigned long long>(stats.memory.vramUsage >> 20), static_cast<unsigned long long>(stats.memory.vramBudget >> 20));
        }
    }

    // The RASM window, F1: the frame stats, the forced LOD (F2) and the render graph's dump (F3).
    void Engine::drawEngineWindow()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - ui->statsTime >= STATS_REFRESH)
        {
            ui->stats = frameStats->stats;
            ui->statsTime = now;
        }

        const float scale = ImGui::GetStyle().FontScaleDpi;
        ImGui::SetNextWindowPos(ImVec2(10.0f * scale, 10.0f * scale), ImGuiCond_FirstUseEver);
        // As tall as its contents, up to the display's height.
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * scale, 0.0f), ImVec2(FLT_MAX, ImGui::GetIO().DisplaySize.y - 20.0f * scale));
        if (!ImGui::Begin("RASM", &config.showStats, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return;
        }
        if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen))
            drawStatsSection(ui->stats);
        if (ImGui::CollapsingHeader("Culling and LODs", ImGuiTreeNodeFlags_DefaultOpen))
        {
            drawCullingSection(ui->stats);
            const std::string current = config.forcedLod < 0 ? "by distance" : "LOD " + std::to_string(config.forcedLod);
            if (ImGui::BeginCombo("LOD (F2)", current.c_str()))
            {
                if (ImGui::Selectable("by distance", config.forcedLod < 0))
                    config.forcedLod = -1;
                for (int32_t lod = 0; lod < static_cast<int32_t>(MAX_MESH_LODS); ++lod)
                {
                    if (ImGui::Selectable(("LOD " + std::to_string(lod)).c_str(), config.forcedLod == lod))
                        config.forcedLod = lod;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::CollapsingHeader("Render graph"))
        {
            if (ImGui::Button("Dump (F3)"))
                graphDumpRequested = true;
            ImGui::SameLine();
            ImGui::TextDisabled("of Engine::render's graph");
            if (!ui->graphDump.empty())
            {
                ImGui::BeginChild("dump", ImVec2(0.0f, 240.0f * scale), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::TextUnformatted(ui->graphDump.c_str());
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }
}
