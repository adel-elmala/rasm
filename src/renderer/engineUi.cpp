#include "rasm/engine.h"
#include "renderer/frameStats.h"
#include "renderer/gpuScene.h"
#include "renderer/inspector.h"
#include "renderer/ui.h"

#include "glm/glm.hpp"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

namespace rasm
{
    namespace
    {
        // Slow enough for the numbers to be read, like the stats overlay's.
        constexpr float STATS_REFRESH = 0.25f;

        constexpr const char *DEBUG_VIEW_NAMES[] = {"None", "Normals", "UVs", "Overdraw", "LODs", "Culling", "Light count", "Meshlets"};
        // The display colors of the views' keys: DEBUG_LOD_COLORS and the culling colors in debugViews.slang and
        // debugGhosts.slang (the ghosts' a few layers deep), HEAT_COLORS in lib/heat.slang.
        constexpr float LOD_COLORS[MAX_MESH_LODS][3] = {{0.20f, 0.80f, 0.25f}, {0.65f, 0.85f, 0.15f}, {0.95f, 0.85f, 0.15f}, {1.00f, 0.55f, 0.10f}, {0.95f, 0.20f, 0.15f}, {0.90f, 0.25f, 0.70f}, {0.55f, 0.30f, 0.95f}, {0.20f, 0.45f, 1.00f}};
        constexpr const char *LOD_LABELS[MAX_MESH_LODS] = {"0", "1", "2", "3", "4", "5", "6", "7"};
        constexpr float CULLING_COLORS[4][3] = {{0.20f, 0.80f, 0.25f}, {0.95f, 0.85f, 0.15f}, {0.60f, 0.04f, 0.04f}, {0.04f, 0.12f, 0.60f}};
        constexpr const char *CULLING_LABELS[4] = {"early", "late", "occluded", "outside"};
        constexpr float HEAT_COLORS[7][3] = {{0.05f, 0.05f, 0.35f}, {0.10f, 0.30f, 0.95f}, {0.05f, 0.80f, 0.90f}, {0.15f, 0.85f, 0.20f}, {0.98f, 0.88f, 0.10f}, {0.95f, 0.15f, 0.10f}, {1.00f, 1.00f, 1.00f}};
        constexpr const char *HEAT_LABELS[7] = {"1", "2", "4", "8", "16", "32", "64"};
        // The frozen frustum's far side is drawn at this NDC depth, 10000 times the near plane's distance: the
        // reversed-Z far plane is at infinity.
        constexpr float FROZEN_FAR_DEPTH = 1e-4f;
    }

    namespace
    {
        void drawStatsSection(const FrameStats &stats)
        {
            const float fps = stats.cpuFrameMs > 0.0f ? 1000.0f / stats.cpuFrameMs : 0.0f;
            ImGui::Text("Frame   %6.2f ms  %4.0f fps", stats.cpuFrameMs, fps);
            if (stats.gpuTimerCount == 0)
                return;
            ImGui::Text("GPU     %6.2f ms  uploads %5.2f ms", stats.gpuFrameMs, stats.uploadGpuMs);
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
            ImGui::Text("Meshlets   %s drawn, %s culled", formatCount(stats.meshletsVisible).c_str(), formatCount(stats.meshletsCulled).c_str());
            if (stats.meshletDrawsWhole > 0)
                ImGui::Text("Whole      %s draws (maxViewMeshlets)", formatCount(stats.meshletDrawsWhole).c_str());
            // What LODs saved, and then what meshlet culling saved of the rest: two numbers, so neither hides the other.
            const uint64_t saved = stats.trianglesVisibleLod0 > stats.trianglesVisibleLod ? stats.trianglesVisibleLod0 - stats.trianglesVisibleLod : 0;
            const double savedPercent = stats.trianglesVisibleLod0 > 0 ? 100.0 * static_cast<double>(saved) / static_cast<double>(stats.trianglesVisibleLod0) : 0.0;
            ImGui::Text("LOD saved  %s (%.0f%%)", formatCount(saved).c_str(), savedPercent);
            const uint64_t meshletSaved = stats.trianglesVisibleLod > stats.trianglesVisible ? stats.trianglesVisibleLod - stats.trianglesVisible : 0;
            const double meshletPercent = stats.trianglesVisibleLod > 0 ? 100.0 * static_cast<double>(meshletSaved) / static_cast<double>(stats.trianglesVisibleLod) : 0.0;
            ImGui::Text("Meshlets saved %s (%.0f%%)", formatCount(meshletSaved).c_str(), meshletPercent);
            // In a buffer on the stack: a std::string this long would allocate in every frame the window is open.
            char lodDraws[160] = "LOD draws ";
            size_t used = std::strlen(lodDraws);
            uint32_t lodsUsed = 1;
            for (uint32_t i = 0; i < MAX_MESH_LODS; ++i)
                lodsUsed = stats.drawsPerLod[i] > 0 ? i + 1 : lodsUsed;
            for (uint32_t i = 0; i < lodsUsed && used < sizeof(lodDraws); ++i)
                used += static_cast<size_t>(std::max(0, std::snprintf(lodDraws + used, sizeof(lodDraws) - used, " %s", formatCount(stats.drawsPerLod[i]).c_str())));
            ImGui::TextUnformatted(lodDraws);
            ImGui::Text("Lights     %s visible, %s global", formatCount(stats.lightsVisible).c_str(), formatCount(stats.lightsGlobal).c_str());
            if (stats.lightsDropped > 0)
                ImGui::Text("Dropped    %s (full clusters)", formatCount(stats.lightsDropped).c_str());
            ImGui::Text("VRAM       %llu / %llu MiB, %llu allocated", static_cast<unsigned long long>(stats.memory.vramUsage >> 20), static_cast<unsigned long long>(stats.memory.vramBudget >> 20),
                        static_cast<unsigned long long>(stats.memory.vramAllocated >> 20));
        }

        void drawKey(const float (*colors)[3], const char *const *labels, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                if (i > 0)
                    ImGui::SameLine();
                ImGui::ColorButton(labels[i], ImVec4(colors[i][0], colors[i][1], colors[i][2], 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop);
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextUnformatted(labels[i]);
            }
        }

        void drawDebugViewKey(DebugView view)
        {
            if (view == DebugView::NORMALS)
                ImGui::TextDisabled("world x, y, z as red, green, blue");
            if (view == DebugView::UVS)
                ImGui::TextDisabled("UV 0 as red, green; 8 squares a unit");
            if (view == DebugView::OVERDRAW || view == DebugView::LIGHT_COUNT)
                drawKey(HEAT_COLORS, HEAT_LABELS, 7);
            if (view == DebugView::LODS)
                drawKey(LOD_COLORS, LOD_LABELS, MAX_MESH_LODS);
            if (view == DebugView::CULLING)
                drawKey(CULLING_COLORS, CULLING_LABELS, 4);
            if (view == DebugView::MESHLETS)
                ImGui::TextDisabled("a color per meshlet; a mesh drawn whole has one");
        }

        // The part of a line in front of the camera, in clip space; false when none of it is.
        bool clipToFront(glm::vec4 &a, glm::vec4 &b)
        {
            constexpr float MIN_W = 1e-4f;
            if (a.w < MIN_W && b.w < MIN_W)
                return false;
            if (a.w < MIN_W)
                a = glm::mix(a, b, (MIN_W - a.w) / (b.w - a.w));
            if (b.w < MIN_W)
                b = glm::mix(b, a, (MIN_W - b.w) / (a.w - b.w));
            return true;
        }

        ImVec2 toScreen(const glm::vec4 &clip, const ImVec2 &size)
        {
            // Clip space is Y-up, the screen Y-down.
            return ImVec2((clip.x / clip.w * 0.5f + 0.5f) * size.x, (0.5f - clip.y / clip.w * 0.5f) * size.y);
        }
    }

    void drawHeatKey()
    {
        drawKey(HEAT_COLORS, HEAT_LABELS, 7);
    }

    // The RASM window, F1: the frame stats, the forced LOD (F2), the debug views, the frame's resources and GPU tables,
    // and the render graph's dump (F3).
    void Engine::drawEngineWindow()
    {
        // Counted in frame time (buildUi adds it), so with EngineConfig::fixedDeltaTime the same frames renew the numbers
        // in every run (C15).
        if (ui->statsAge >= STATS_REFRESH)
        {
            ui->stats = frameStats->stats;
            ui->statsAge = 0.0f;
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
        // setInspection opens the sections it fills, and closes the others, once.
        const bool openBuffers = std::exchange(inspector->openBuffers, false);
        const bool openTables = std::exchange(inspector->openTables, false);
        if (openBuffers || openTables)
            ImGui::SetNextItemOpen(false);
        if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen))
            drawStatsSection(ui->stats);
        if (openBuffers || openTables)
            ImGui::SetNextItemOpen(false);
        if (ImGui::CollapsingHeader("Culling and LODs", ImGuiTreeNodeFlags_DefaultOpen))
        {
            drawCullingSection(ui->stats);
            const LoadStats loads = getLoadStats();
            ImGui::Text("Loads      %u queued, %u running, %llu MiB, %u workers", loads.queued, loads.running, static_cast<unsigned long long>(loads.decodedBytes >> 20), loads.workers);
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
            const char *meshletPaths[] = {"off", "vertex pulling", ctx.hasMeshShaders() ? "mesh shaders" : "mesh shaders (vertex pulling here)", ctx.hasMeshShaders() ? "auto (mesh shaders)" : "auto (off here)"};
            const uint32_t meshletPath = std::min(static_cast<uint32_t>(config.meshletPath), static_cast<uint32_t>(std::size(meshletPaths) - 1));
            if (ImGui::BeginCombo("Meshlets", meshletPaths[meshletPath]))
            {
                for (uint32_t i = 0; i < std::size(meshletPaths); ++i)
                {
                    if (ImGui::Selectable(meshletPaths[i], static_cast<uint32_t>(config.meshletPath) == i))
                        setMeshletPath(static_cast<MeshletPath>(i));
                }
                ImGui::EndCombo();
            }
        }
        if (openBuffers || openTables)
            ImGui::SetNextItemOpen(false);
        if (ImGui::CollapsingHeader("Debug views", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DebugView view = debugView;
            if (ImGui::BeginCombo("View", DEBUG_VIEW_NAMES[static_cast<uint32_t>(view)]))
            {
                for (uint32_t i = 0; i < std::size(DEBUG_VIEW_NAMES); ++i)
                {
                    if (ImGui::Selectable(DEBUG_VIEW_NAMES[i], static_cast<uint32_t>(view) == i))
                        view = static_cast<DebugView>(i);
                }
                ImGui::EndCombo();
            }
            setDebugView(view);
            drawDebugViewKey(view);
            bool frozen = cullingFrozen;
            if (ImGui::Checkbox("Freeze culling", &frozen))
                setCullingFrozen(frozen);
        }
        if (openBuffers)
            ImGui::SetNextItemOpen(true);
        if (ImGui::CollapsingHeader("Buffers"))
            drawBuffersSection();
        if (openTables)
            ImGui::SetNextItemOpen(true);
        if (ImGui::CollapsingHeader("GPU tables"))
            drawTablesSection();
        if (ImGui::CollapsingHeader("Render graph"))
        {
            if (ImGui::Button("Dump (F3)"))
                graphDumpRequested = true;
            ImGui::SameLine();
            ImGui::TextDisabled("of the next graph that runs");
            if (!graphDump.empty())
            {
                ImGui::BeginChild("dump", ImVec2(0.0f, 240.0f * scale), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::TextUnformatted(graphDump.c_str());
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }

    // A frozen view's frustum as lines on the UI's background, seen from the frame's camera: the near rectangle, the
    // side edges and a far rectangle at FROZEN_FAR_DEPTH. Views of other cameras draw elsewhere, so they are left out.
    void Engine::drawFrozenFrustums(CameraHandle camera)
    {
        if (!cullingFrozen)
            return;
        ImDrawList *drawList = ImGui::GetBackgroundDrawList();
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float thickness = 2.0f * ImGui::GetStyle().FontScaleDpi;
        for (const SceneView &view : gpuScene->views)
        {
            if (view.graphs == 0 || view.depthOnly || !view.frozen || view.camera != camera)
                continue;
            const RenderTarget &target = ctx.getRenderTarget(view.renderTarget);
            const glm::mat4 live = getProjectionMatrix(view.camera, static_cast<float>(target.width) / static_cast<float>(target.height)) * getViewMatrix(view.camera);
            const glm::mat4 toWorld = glm::inverse(view.frozenViewProjection);
            glm::vec4 corners[8];
            for (uint32_t i = 0; i < 8; ++i)
            {
                const glm::vec4 world = toWorld * glm::vec4((i & 1) != 0 ? 1.0f : -1.0f, (i & 2) != 0 ? 1.0f : -1.0f, i < 4 ? 1.0f : FROZEN_FAR_DEPTH, 1.0f);
                corners[i] = live * glm::vec4(glm::vec3(world) / world.w, 1.0f);
            }
            const uint32_t edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto &edge : edges)
            {
                glm::vec4 a = corners[edge[0]];
                glm::vec4 b = corners[edge[1]];
                if (clipToFront(a, b))
                    drawList->AddLine(toScreen(a, size), toScreen(b, size), IM_COL32(255, 140, 40, 255), thickness);
            }
        }
    }
}
