#include "renderer/inspector.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/animation.h"
#include "renderer/frameStats.h"
#include "renderer/gpuScene.h"
#include "renderer/lightClusters.h"
#include "renderer/shaderCompiler.h"
#include "renderer/uberMaterialTypes.h"
#include "renderer/ui.h"

#include "imgui.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

namespace rasm
{
    namespace
    {
        // A GPU table as the GPU tables section lists it: its rows' struct in the shaders, or none for a float.
        struct TableView
        {
            const GpuTable *table;
            uint32_t used;
            const char *layout;
        };
    }

    namespace
    {
        // numthreads of shaders/passes/inspect.slang.
        constexpr uint32_t INSPECT_GROUP_SIZE = 8;
        static_assert(INSPECT_TILES == CLUSTER_SLICES, "Inspection::level counts the tiles after the slices");

        constexpr const char *CHANNEL_NAMES[] = {"RGB", "R", "G", "B", "A"};
        constexpr const char *FACE_NAMES[CUBE_FACES] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

        bool matches(const InspectEntry &entry, const Inspection &inspection)
        {
            if (entry.target != inspection.target || entry.kind != inspection.kind)
                return false;
            if (entry.kind == InspectKind::COLOR)
                return entry.attachment == inspection.attachment;
            if (entry.kind == InspectKind::HIZ || entry.kind == InspectKind::LIGHT_CLUSTERS)
                return entry.face == inspection.face;
            return true;
        }

        // A target without a name is listed, and picked, by its handle.
        std::string getTargetName(RenderContext &ctx, RenderTargetHandle target)
        {
            const std::string name = ctx.getRenderTargetDesc(target).name;
            return name.empty() ? "#" + std::to_string(target.index) + ":" + std::to_string(target.generation) : name;
        }

        void addTarget(std::vector<RenderTargetHandle> &targets, RenderTargetHandle target)
        {
            if (target.isValid() && std::find(targets.begin(), targets.end(), target) == targets.end())
                targets.push_back(target);
        }

        std::string formatBytes(uint64_t bytes)
        {
            if (bytes >= (10ull << 20))
                return std::to_string(bytes >> 20) + " MiB";
            if (bytes >= (10ull << 10))
                return std::to_string(bytes >> 10) + " KiB";
            return std::to_string(bytes) + " B";
        }

        // One field's value from a row's bytes, its vectors on one line and a matrix's rows on lines of their own.
        std::string formatField(const StructField &field, const uint8_t *row)
        {
            std::string text;
            // A matrix is stored column by column (the session's column-major layout), and printed row by row.
            const uint32_t rows = field.count / field.columns;
            for (uint32_t i = 0; i < field.count; ++i)
            {
                if (i > 0)
                    text += i % field.columns == 0 ? "\n" : "  ";
                char value[32] = {};
                const uint32_t stored = rows > 1 ? (i % field.columns) * rows + i / field.columns : i;
                const uint8_t *bytes = row + field.offset + stored * 4;
                if (field.type == FieldType::FLOAT)
                {
                    float f = 0.0f;
                    std::memcpy(&f, bytes, sizeof(f));
                    std::snprintf(value, sizeof(value), "%.4g", static_cast<double>(f));
                }
                else if (field.type == FieldType::INT)
                {
                    int32_t v = 0;
                    std::memcpy(&v, bytes, sizeof(v));
                    std::snprintf(value, sizeof(value), "%d", v);
                }
                else if (field.type == FieldType::UINT)
                {
                    uint32_t v = 0;
                    std::memcpy(&v, bytes, sizeof(v));
                    std::snprintf(value, sizeof(value), v == UINT32_MAX ? "0x%08X" : "%u", v);
                }
                else if (field.type == FieldType::UINT64 || field.type == FieldType::POINTER)
                {
                    uint64_t v = 0;
                    std::memcpy(&v, row + field.offset, sizeof(v));
                    std::snprintf(value, sizeof(value), "0x%016" PRIX64, v);
                    return value;
                }
                else
                    return "(" + std::to_string(field.size) + " B)";
                text += value;
            }
            return text;
        }

        // A row as a table of its fields: the names, types and offsets come from the shaders' reflection.
        void drawRow(const GpuTable &table, uint32_t row, const char *layoutName, const std::vector<StructField> *layout, bool open = false)
        {
            if (row >= table.capacity())
                return;
            const std::string title = table.name() + " row " + std::to_string(row);
            if (open)
                ImGui::SetNextItemOpen(true);
            if (!ImGui::TreeNode(title.c_str()))
                return;
            const uint8_t *bytes = table.row(row);
            if (!layout && layoutName)
                ImGui::TextDisabled("%s: no compiled shader has reached it yet", layoutName);
            else if (ImGui::BeginTable("fields", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Field");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Offset");
                ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();
                const StructField single = {.name = "weight", .typeName = "float", .size = 4, .type = FieldType::FLOAT};
                const StructField *fields = layout ? layout->data() : &single;
                const size_t count = layout ? layout->size() : 1;
                for (size_t i = 0; i < count; ++i)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(fields[i].name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", fields[i].typeName.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", fields[i].offset);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(formatField(fields[i], bytes).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        // What the picture's colors mean.
        void drawInspectKey(const Inspection &inspection, bool motion, bool orthographic)
        {
            if (inspection.kind == InspectKind::COLOR && motion)
                ImGui::TextWrapped("Direction as hue, length as color: white is still, full color at %.0f px.", 16.0 / static_cast<double>(inspection.exposure));
            else if (inspection.kind == InspectKind::COLOR)
                ImGui::TextWrapped("Linear values times the exposure, clamped to 0..1.");
            else if (inspection.kind == InspectKind::LIGHT_CLUSTERS)
            {
                ImGui::TextWrapped("Lights per %s, 16 x 9 tiles:", inspection.level >= INSPECT_TILES ? "screen tile (a column of 24 slices)" : "cluster of the slice");
                drawHeatKey();
            }
            else if (orthographic)
                ImGui::TextWrapped("Orthographic: the depth is linear already, white at the near plane, black at the far one.");
            else if (inspection.kind == InspectKind::DEPTH || inspection.kind == InspectKind::HIZ)
                ImGui::TextWrapped("Reversed-Z made linear: view depth = near / depth. White at the camera, black at %.0f m and at the sky.", static_cast<double>(inspection.depthRange));
        }
    }

    glm::uvec2 getInspectRegion(glm::uvec2 source, uint32_t size)
    {
        const glm::uvec2 clamped = glm::max(source, glm::uvec2(1));
        const double scale = static_cast<double>(size) / static_cast<double>(std::max(clamped.x, clamped.y));
        return glm::clamp(glm::uvec2(glm::round(glm::dvec2(clamped) * scale)), glm::uvec2(1), glm::uvec2(size));
    }

    glm::uvec2 getHizLevelSize(glm::uvec2 viewport, uint32_t level)
    {
        glm::uvec2 size = (viewport + 1u) / 2u;
        for (uint32_t i = 0; i < level; ++i)
            size = (size + 1u) / 2u;
        return size;
    }

    // Runs at the start of RenderGraph::execute. While the Buffers section is open it lists the graph's resources, and
    // the graph holds an Inspect pass for the picked one, named after it, which the UI pass reads. A new pick replaces
    // the pass; no pick, or a closed section, takes it out, so the graph is again the one without the UI.
    void Engine::updateInspectPass(RenderGraph &graph)
    {
        if (!ui || !inspector)
            return;
        Inspector &state = *inspector;
        state.entries.clear();
        state.active = UINT32_MAX;
        state.noUiPass = false;

        uint32_t wanted = UINT32_MAX;
        if (state.shown)
        {
            // The targets in the order the passes use them, then each scene view's buffers.
            std::vector<RenderTargetHandle> targets;
            for (uint32_t i = 0; i < graph.passes.size(); ++i)
            {
                if (i == graph.inspectPass)
                    continue;
                const Pass &pass = graph.passes[i];
                addTarget(targets, pass.outputRenderTarget);
                for (auto target : pass.inputRenderTargets)
                    addTarget(targets, target);
                for (auto target : pass.depthRenderTargets)
                    addTarget(targets, target);
            }
            auto viewOf = [&](RenderTargetHandle target)
            {
                for (uint32_t view : graph.sceneViews)
                {
                    if (gpuScene->views[view].renderTarget == target)
                        return view;
                }
                return UINT32_MAX;
            };
            for (auto target : targets)
            {
                if (target == state.capture)
                    continue;
                const RenderTarget &rt = ctx.getRenderTarget(target);
                const std::string name = getTargetName(ctx, target);
                const uint32_t view = viewOf(target);
                for (uint32_t c = 0; c < rt.colorCount; ++c)
                {
                    std::string label = name;
                    if (rt.colorFormats[c] == MOTION_FORMAT)
                        label += " motion";
                    else if (c > 0)
                        label += " color " + std::to_string(c);
                    state.entries.push_back({.label = label, .target = name, .kind = InspectKind::COLOR, .attachment = c, .renderTarget = target, .view = view});
                }
                // The depth the graph lets a pass sample (canReadDepth in renderGraph.cpp), of a target a view draws, whose
                // projection makes it a view depth.
                const bool motion = rt.colorCount == 2 && rt.colorFormats[1] == MOTION_FORMAT;
                if (view != UINT32_MAX && rt.depthFormat == Format::D32_SFLOAT && (config.occlusionCulling || rt.colorCount == 0 || motion))
                    state.entries.push_back({.label = name + " depth", .target = name, .kind = InspectKind::DEPTH, .renderTarget = target, .view = view});
            }
            for (uint32_t view : graph.sceneViews)
            {
                const SceneView &sceneView = gpuScene->views[view];
                const std::string target = getTargetName(ctx, sceneView.renderTarget);
                const std::string name = ctx.getRenderTargetDesc(sceneView.renderTarget).cube ? target + " " + FACE_NAMES[sceneView.face] : target;
                if (sceneView.visibility.isValid())
                    state.entries.push_back({.label = name + " Hi-Z", .target = target, .kind = InspectKind::HIZ, .face = sceneView.face, .view = view});
                if (gpuLights->views[view].buffers[0].isValid())
                    state.entries.push_back({.label = name + " light clusters", .target = target, .kind = InspectKind::LIGHT_CLUSTERS, .face = sceneView.face, .view = view});
            }
            for (uint32_t i = 0; i < state.entries.size() && wanted == UINT32_MAX && !inspection.target.empty(); ++i)
                wanted = matches(state.entries[i], inspection) ? i : UINT32_MAX;
        }

        // The UI pass is the one that draws the swapchain through its UNORM view.
        uint32_t uiPass = UINT32_MAX;
        for (uint32_t i = 0; i < graph.passes.size() && uiPass == UINT32_MAX; ++i)
            uiPass = graph.passes[i].encodedSwapchain ? i : UINT32_MAX;
        state.noUiPass = wanted != UINT32_MAX && uiPass == UINT32_MAX;
        if (state.noUiPass)
            wanted = UINT32_MAX;

        const std::string name = wanted != UINT32_MAX ? "Inspect " + state.entries[wanted].label : "";
        if (graph.inspectPass != UINT32_MAX && graph.passes[graph.inspectPass].name == name)
        {
            state.active = wanted;
            return;
        }
        if (graph.inspectPass != UINT32_MAX)
        {
            if (uiPass != UINT32_MAX)
            {
                auto &reads = graph.passes[uiPass].inputRenderTargets;
                reads.erase(std::remove(reads.begin(), reads.end(), state.capture), reads.end());
            }
            uiPass -= uiPass != UINT32_MAX && uiPass > graph.inspectPass ? 1 : 0;
            graph.removePass(graph.inspectPass);
        }
        if (wanted == UINT32_MAX)
            return;

        // Made on the first copy, and kept: a pick after that needs nothing new. A failure is not tried again.
        if (state.captureFailed)
            return;
        if (!state.capture.isValid())
            state.capture = ctx.createRenderTarget({.name = "inspect", .width = INSPECT_SIZE, .height = INSPECT_SIZE, .colorFormats = {SCENE_COLOR_FORMAT}, .storage = true});
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            if (!state.params[i].isValid())
                state.params[i] = ctx.createBuffer({.name = "Inspect Params " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = sizeof(gfx::InspectParams), .hostVisible = true});
        }
        if (!state.capture.isValid() || !state.params[0].isValid() || !state.params[1].isValid())
        {
            if (!std::exchange(state.captureFailed, true))
                spdlog::error("The Buffers section could not create its capture target; nothing is shown.");
            return;
        }

        const InspectEntry entry = state.entries[wanted];
        const RenderTargetHandle capture = state.capture;
        const SceneView &sceneView = gpuScene->views[entry.view < MAX_SCENE_VIEWS ? entry.view : 0];
        const ViewClusters &clusters = gpuLights->views[entry.view < MAX_SCENE_VIEWS ? entry.view : 0];
        graph.addComputePass(
            name,
            [&](PassBuilder &builder)
            {
                if (entry.kind == InspectKind::COLOR)
                    builder.read(entry.renderTarget);
                if (entry.kind == InspectKind::DEPTH)
                    builder.readDepth(entry.renderTarget);
                if (entry.kind == InspectKind::HIZ)
                    builder.read(sceneView.hiz);
                if (entry.kind == InspectKind::LIGHT_CLUSTERS)
                    builder.read(clusters.buffers);
                builder.write(capture);
            },
            [this, entry](RenderContext &)
            { recordInspect(entry); });
        graph.inspectPass = static_cast<uint32_t>(graph.passes.size() - 1);
        graph.passes[uiPass].inputRenderTargets.push_back(capture);
        state.pipelineTried = false;
        state.active = wanted;
    }

    void Engine::recordInspect(const InspectEntry &entry)
    {
        Inspector &state = *inspector;
        if (!state.pipeline.isValid() && !state.pipelineTried)
        {
            state.pipelineTried = true;
            state.pipeline = createComputePipeline("inspect", readFile(pathFromUtf8(config.shaderDir) / "passes/inspect.slang"), "inspectMain");
            if (!state.pipeline.isValid())
                spdlog::error("Failed to create the Inspect pipeline; the Buffers section shows nothing until the pick changes.");
        }
        if (!state.pipeline.isValid())
            return;

        // The view's SceneData of this frame: one that did not get ready has none.
        const uint32_t frame = static_cast<uint32_t>(frameCount);
        uint32_t view = entry.view;
        if (entry.kind == InspectKind::DEPTH && view != UINT32_MAX)
        {
            for (uint32_t v = 0; v < MAX_SCENE_VIEWS; ++v)
            {
                if (gpuScene->views[v].graphs > 0 && gpuScene->views[v].renderTarget == entry.renderTarget && gpuScene->views[v].face == inspection.face)
                    view = v;
            }
        }
        if (entry.kind != InspectKind::COLOR && (view == UINT32_MAX || !gpuScene->views[view].ready))
            return;

        gfx::InspectParams params = {.scene = view != UINT32_MAX ? getSceneDataAddress(ctx, *gpuScene, frame, view) : 0,
                                     .source = INVALID_TEXTURE_INDEX,
                                     .target = getRenderTargetStorageIndex(state.capture),
                                     .channel = std::min(inspection.channel, 4u),
                                     .face = std::min(inspection.face, CUBE_FACES - 1),
                                     .level = inspection.level,
                                     .sample = inspection.sample,
                                     .exposure = inspection.exposure,
                                     .depthRange = std::max(inspection.depthRange, 1e-3f)};
        glm::uvec2 sourceSize(CLUSTER_TILES_X, CLUSTER_TILES_Y);
        if (entry.kind == InspectKind::COLOR || entry.kind == InspectKind::DEPTH)
        {
            const RenderTarget &rt = ctx.getRenderTarget(entry.renderTarget);
            const TextureHandle texture = entry.kind == InspectKind::COLOR ? getRenderTargetTexture(entry.renderTarget, entry.attachment) : getRenderTargetDepthTexture(entry.renderTarget);
            const bool motion = entry.kind == InspectKind::COLOR && rt.colorFormats[entry.attachment] == MOTION_FORMAT;
            params.mode = entry.kind == InspectKind::DEPTH ? gfx::INSPECT_MODE_DEPTH : (motion ? gfx::INSPECT_MODE_MOTION : gfx::INSPECT_MODE_COLOR);
            params.source = getBindlessTextureIndex(texture);
            params.flags = (rt.samples > 1 ? gfx::INSPECT_FLAG_MULTISAMPLED : 0) | (ctx.getTextureDesc(texture).cube ? gfx::INSPECT_FLAG_CUBE : 0);
            sourceSize = glm::uvec2(rt.width, rt.height);
            if (params.source == UINT32_MAX)
                return;
        }
        if (entry.kind == InspectKind::HIZ)
        {
            params.mode = gfx::INSPECT_MODE_HIZ;
            sourceSize = getHizLevelSize({gpuScene->views[view].width, gpuScene->views[view].height}, inspection.level);
        }
        if (entry.kind == InspectKind::LIGHT_CLUSTERS)
            params.mode = gfx::INSPECT_MODE_CLUSTERS;
        const glm::uvec2 region = getInspectRegion(sourceSize, INSPECT_SIZE);
        params.sourceWidth = sourceSize.x;
        params.sourceHeight = sourceSize.y;
        params.width = region.x;
        params.height = region.y;
        if (!ctx.fillBuffer(state.params[frame], &params, sizeof(params), 0))
            return;

        const auto commandBuffer = getCurrentFrameResources().commandBuffer;
        const gfx::InspectPushConstants pushConstants{.params = ctx.getBufferDeviceAddress(state.params[frame])};
        ctx.bindPipeline(commandBuffer, state.pipeline);
        ctx.bindDescriptorSet(commandBuffer, state.pipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, state.pipeline, ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, (region.x + INSPECT_GROUP_SIZE - 1) / INSPECT_GROUP_SIZE, (region.y + INSPECT_GROUP_SIZE - 1) / INSPECT_GROUP_SIZE);
    }

    // The RASM window's Buffers section: pick a resource of the frame's graph, then a face, sample, channel, Hi-Z level
    // or cluster slice where it has them. The Inspect pass copied it into the capture target this frame.
    void Engine::drawBuffersSection()
    {
        Inspector &state = *inspector;
        state.shown = true;
        const float scale = ImGui::GetStyle().FontScaleDpi;

        uint32_t picked = UINT32_MAX;
        for (uint32_t i = 0; i < state.entries.size() && picked == UINT32_MAX; ++i)
            picked = matches(state.entries[i], inspection) ? i : UINT32_MAX;
        const std::string preview = inspection.target.empty() ? "None" : (picked != UINT32_MAX ? state.entries[picked].label : inspection.target + " (not in this frame)");
        if (ImGui::BeginCombo("Resource", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("None", inspection.target.empty()))
                inspection.target.clear();
            for (uint32_t i = 0; i < state.entries.size(); ++i)
            {
                const InspectEntry &entry = state.entries[i];
                if (ImGui::Selectable(entry.label.c_str(), i == picked))
                {
                    inspection.target = entry.target;
                    inspection.kind = entry.kind;
                    inspection.attachment = entry.attachment;
                    if (entry.kind == InspectKind::HIZ || entry.kind == InspectKind::LIGHT_CLUSTERS)
                        inspection.face = entry.face;
                    picked = i;
                }
            }
            ImGui::EndCombo();
        }
        if (inspection.target.empty() || picked == UINT32_MAX)
        {
            ImGui::TextDisabled("Nothing is copied: the graph has no Inspect pass.");
            return;
        }

        // The controls of what the resource has, and its size.
        const InspectEntry &entry = state.entries[picked];
        glm::uvec2 sourceSize(CLUSTER_TILES_X, CLUSTER_TILES_Y);
        bool motion = false;
        bool orthographic = false;
        // A shadow view draws from its light, a directional one's orthographic.
        if (entry.view != UINT32_MAX)
        {
            const SceneView &view = gpuScene->views[entry.view];
            if (view.depthOnly && view.shadowCount > 0)
                orthographic = registry.lights[view.shadows[0].light.index].type == LightType::DIRECTIONAL;
            else
                orthographic = registry.cameras[view.camera.index].projection.type == CameraProjection::Type::ORTHOGRAPHIC;
        }
        if (entry.kind == InspectKind::COLOR || entry.kind == InspectKind::DEPTH)
        {
            const RenderTarget &rt = ctx.getRenderTarget(entry.renderTarget);
            const RenderTargetDesc desc = ctx.getRenderTargetDesc(entry.renderTarget);
            sourceSize = glm::uvec2(rt.width, rt.height);
            motion = entry.kind == InspectKind::COLOR && rt.colorFormats[entry.attachment] == MOTION_FORMAT;
            ImGui::Text("%u x %u%s%s", rt.width, rt.height, rt.samples > 1 ? (", " + std::to_string(rt.samples) + " samples").c_str() : "", desc.cube ? ", a cube" : "");
            if (desc.cube)
            {
                int face = static_cast<int>(std::min(inspection.face, CUBE_FACES - 1));
                ImGui::SliderInt("Face", &face, 0, CUBE_FACES - 1, FACE_NAMES[face]);
                inspection.face = static_cast<uint32_t>(face);
            }
            if (rt.samples > 1)
            {
                const std::string current = inspection.sample == INSPECT_ALL_SAMPLES ? "average (a box resolve)" : "sample " + std::to_string(std::min(inspection.sample, rt.samples - 1));
                if (ImGui::BeginCombo("Sample", current.c_str()))
                {
                    for (uint32_t s = 0; s < rt.samples; ++s)
                    {
                        if (ImGui::Selectable(("sample " + std::to_string(s)).c_str(), inspection.sample == s))
                            inspection.sample = s;
                    }
                    if (ImGui::Selectable("average (a box resolve)", inspection.sample == INSPECT_ALL_SAMPLES))
                        inspection.sample = INSPECT_ALL_SAMPLES;
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("One sample of the multisampled target, not the MSAA resolve pass's result.");
            }
        }
        if (entry.kind == InspectKind::COLOR && !motion)
        {
            int channel = static_cast<int>(std::min(inspection.channel, 4u));
            ImGui::Combo("Channel", &channel, CHANNEL_NAMES, static_cast<int>(std::size(CHANNEL_NAMES)));
            inspection.channel = static_cast<uint32_t>(channel);
        }
        if (entry.kind == InspectKind::COLOR)
            ImGui::SliderFloat(motion ? "Gain" : "Exposure", &inspection.exposure, 1.0f / 64.0f, 64.0f, "%.3g", ImGuiSliderFlags_Logarithmic);
        if ((entry.kind == InspectKind::DEPTH || entry.kind == InspectKind::HIZ) && !orthographic)
            ImGui::SliderFloat("Range", &inspection.depthRange, 1.0f, 1000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
        if (entry.kind == InspectKind::HIZ)
        {
            const SceneView &view = gpuScene->views[entry.view];
            const int levels = static_cast<int>(std::max(view.hizLevelCount, 1u));
            int level = std::min(static_cast<int>(inspection.level), levels - 1);
            ImGui::SliderInt("Level", &level, 0, levels - 1);
            inspection.level = static_cast<uint32_t>(level);
            sourceSize = getHizLevelSize({view.width, view.height}, inspection.level);
            ImGui::Text("%u x %u, the farthest depth of each %u x %u pixels", sourceSize.x, sourceSize.y, 2u << inspection.level, 2u << inspection.level);
        }
        if (entry.kind == InspectKind::LIGHT_CLUSTERS)
        {
            int level = static_cast<int>(std::min(inspection.level, INSPECT_TILES));
            ImGui::SliderInt("Slice", &level, 0, static_cast<int>(INSPECT_TILES), level == static_cast<int>(INSPECT_TILES) ? "tiles" : "%d");
            inspection.level = static_cast<uint32_t>(level);
        }
        drawInspectKey(inspection, motion, orthographic && entry.kind != InspectKind::COLOR);

        if (state.active != picked || !state.capture.isValid())
        {
            ImGui::TextDisabled(state.noUiPass ? "The graph has no UI pass (Engine::addUiPass), so nothing is copied." : "Copied from the next frame on.");
            return;
        }
        const glm::uvec2 region = getInspectRegion(sourceSize, INSPECT_SIZE);
        // The window fits its contents, so the picture sets its width.
        const float width = 400.0f * scale;
        const float height = width * static_cast<float>(region.y) / static_cast<float>(region.x);
        const ImVec2 uv1(static_cast<float>(region.x) / static_cast<float>(INSPECT_SIZE), static_cast<float>(region.y) / static_cast<float>(INSPECT_SIZE));
        ImGui::Image(ImTextureRef(getUiTexture(getRenderTargetTexture(state.capture))), ImVec2(std::max(width, 1.0f), std::max(height, 1.0f)), ImVec2(0.0f, 0.0f), uv1);
        ImGui::TextDisabled("Copied right after the last pass that writes it.");
    }

    // The RASM window's GPU tables section: each table's rows and bytes, and one entity's rows field by field, from
    // the table's CPU copy (GpuTable::row), which is what each frame slot receives.
    void Engine::drawTablesSection()
    {
        ImGui::TextWrapped("What the CPU uploaded, from each table's CPU copy. Not read back from the GPU.");
        const TableView tables[] = {{&gpuScene->instances, gpuScene->instanceCount, "InstanceData"},
                                    {&gpuScene->materials, static_cast<uint32_t>(registry.materials.size()), "MaterialData"},
                                    {&gpuLights->lights, static_cast<uint32_t>(registry.lights.size()), "Light"},
                                    {&gpuAnimation->entities, gpuAnimation->entityRows, "AnimatedEntity"},
                                    {&gpuAnimation->weightOverrides, gpuAnimation->weightCount, nullptr}};
        if (ImGui::BeginTable("tables", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Table");
            ImGui::TableSetupColumn("Rows used");
            ImGui::TableSetupColumn("Bytes per slot");
            ImGui::TableSetupColumn("Sent this frame");
            ImGui::TableHeadersRow();
            for (const TableView &view : tables)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(view.table->name().c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s / %s", formatCount(view.used).c_str(), formatCount(view.table->capacity()).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s / %s", formatBytes(static_cast<uint64_t>(view.used) * view.table->rowBytes()).c_str(),
                            formatBytes(static_cast<uint64_t>(view.table->capacity()) * view.table->rowBytes()).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatCount(view.table->uploadedRows()).c_str());
            }
            ImGui::EndTable();
        }

        const bool valid = inspection.entity.isValid() && inspection.entity.index < registry.entities.size();
        const std::string current = valid ? registry.entities[inspection.entity.index].name : "None";
        if (ImGui::BeginCombo("Entity", current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("None", !valid))
                inspection.entity = {};
            for (const Entity &entity : registry.entities)
            {
                if (ImGui::Selectable((entity.name + "##" + std::to_string(entity.handle.index)).c_str(), entity.handle == inspection.entity))
                    inspection.entity = entity.handle;
            }
            ImGui::EndCombo();
        }
        if (!valid || inspection.entity.index >= gpuScene->entities.size())
            return;

        // Its instance rows, the materials they name, and its animated entity row.
        // setInspection opens the rows once they exist: a mesh that still loads has none.
        const EntityDrawState &draws = gpuScene->entities[inspection.entity.index];
        const bool openRows = draws.instanceCount > 0 && std::exchange(inspector->openRows, false);
        const std::vector<StructField> *instanceLayout = shaderCompiler->findStructLayout("InstanceData");
        uint32_t materialOffset = UINT32_MAX;
        for (size_t i = 0; instanceLayout && i < instanceLayout->size(); ++i)
            materialOffset = (*instanceLayout)[i].name == "materialIndex" ? (*instanceLayout)[i].offset : materialOffset;
        for (uint32_t i = 0; i < draws.instanceCount; ++i)
        {
            const uint32_t row = draws.firstInstance + i;
            // Two instances can name one material, so each instance's nodes get IDs of their own.
            ImGui::PushID(static_cast<int>(row));
            drawRow(gpuScene->instances, row, "InstanceData", instanceLayout, openRows);
            if (materialOffset != UINT32_MAX && row < gpuScene->instances.capacity())
            {
                uint32_t material = 0;
                std::memcpy(&material, gpuScene->instances.row(row) + materialOffset, sizeof(material));
                ImGui::Indent();
                drawRow(gpuScene->materials, material, "MaterialData", shaderCompiler->findStructLayout("MaterialData"));
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (inspection.entity.index < gpuAnimation->entityStates.size() && gpuAnimation->entityStates[inspection.entity.index].row != UINT32_MAX)
            drawRow(gpuAnimation->entities, gpuAnimation->entityStates[inspection.entity.index].row, "AnimatedEntity", shaderCompiler->findStructLayout("AnimatedEntity"));
    }
}
