#include "c/cApi.h"

#include "rasm/render/renderGraph.h"
#include "rasm/renderer.h"
#include "rasm/scene.h"
#include "rasm/view.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace rasm;

// What a pass callback records with: one per graph, made with it, so a frame allocates nothing. It is active only while
// a callback of the graph runs; every rasm_cmd_ call checks that first.
struct rasm_commands
{
    Renderer *renderer = nullptr;
    CommandBufferHandle commandBuffer;
    PipelineHandle pipeline;
    uint8_t pipelineType = 0; // rasm_pipeline_type + 1 of the bound pipeline, 0 for none
    rasm_pass_type passType = RASM_PASS_TYPE_GRAPHICS;
    bool indexBound = false;
    bool active = false;
    const rasm_renderer *owner = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string timerName; // keeps its capacity, so a timer's name allocates once
};

struct rasm_render_graph
{
    rasm_renderer *owner = nullptr; // null once its renderer is gone
    std::unique_ptr<RenderGraph> graph;
    rasm_commands commands;
    std::vector<SceneShadow> shadows; // a rasm_shadow's index + 1, in its low 32 bits
    uint64_t serial = 0;              // in a rasm_shadow's high 32 bits
};

namespace
{
    // The graph's commands are active only during a pass callback.
    struct ActiveCommands
    {
        rasm_commands &commands;
        explicit ActiveCommands(rasm_commands &cmd) : commands(cmd) { commands.active = true; }
        ~ActiveCommands() { commands.active = false; }
        ActiveCommands(const ActiveCommands &) = delete;
        ActiveCommands &operator=(const ActiveCommands &) = delete;
    };

    struct Recording
    {
        rasm_renderer &renderer;
        explicit Recording(rasm_renderer &owner) : renderer(owner) { ++renderer.recording; }
        ~Recording() { --renderer.recording; }
        Recording(const Recording &) = delete;
        Recording &operator=(const Recording &) = delete;
    };
}

namespace
{
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): numbers the graphs, so a rasm_shadow names its own
    std::atomic<uint64_t> nextGraphSerial{1};

    // The graph's renderer is still alive and nothing records: the graph may change.
    rasm_render_graph *getGraph(rasm_render_graph *graph, const char *function, bool changes = true)
    {
        if (graph == nullptr)
        {
            c::reportError(function, "graph is NULL.");
            return nullptr;
        }
        if (graph->owner == nullptr || !graph->graph)
        {
            c::reportError(function, "the graph's renderer was destroyed: only rasm_render_graph_destroy may take it now.");
            return nullptr;
        }
        if (changes && !c::checkNotRecording(graph->owner, function))
            return nullptr;
        return graph;
    }

    rasm_commands *getCommands(rasm_commands *commands, const char *function)
    {
        if (commands == nullptr)
        {
            c::reportError(function, "commands is NULL.");
            return nullptr;
        }
        if (!commands->active)
        {
            c::reportError(function, "commands is valid only inside the pass callback it was given to.");
            return nullptr;
        }
        return commands;
    }

    // A draw or a dispatch: the pass's type, and a bound pipeline of that type.
    rasm_commands *getRecording(rasm_commands *commands, bool compute, const char *function)
    {
        rasm_commands *cmd = getCommands(commands, function);
        if (cmd == nullptr)
            return nullptr;
        const bool computePass = cmd->passType != RASM_PASS_TYPE_GRAPHICS;
        if (computePass != compute)
        {
            c::reportError(function, compute ? "a graphics pass cannot dispatch." : "a compute pass cannot draw.");
            return nullptr;
        }
        if (cmd->pipelineType == 0)
        {
            c::reportError(function, "no pipeline is bound: call rasm_cmd_bind_pipeline first.");
            return nullptr;
        }
        return cmd;
    }

    rasm_commands *getGraphicsCommands(rasm_commands *commands, const char *function)
    {
        rasm_commands *cmd = getCommands(commands, function);
        if (cmd != nullptr && cmd->passType != RASM_PASS_TYPE_GRAPHICS)
        {
            c::reportError(function, "only a graphics pass has a viewport, a scissor, a cull mode and an index buffer.");
            return nullptr;
        }
        return cmd;
    }

    rasm_commands *getIndexedRecording(rasm_commands *commands, const char *function)
    {
        rasm_commands *cmd = getRecording(commands, false, function);
        if (cmd != nullptr && !cmd->indexBound)
        {
            c::reportError(function, "no index buffer is bound: call rasm_cmd_bind_index_buffer first.");
            return nullptr;
        }
        return cmd;
    }

    // The access is one checkResources let through.
    void addResource(PassBuilder &builder, const rasm_pass_resource &resource)
    {
        const uint64_t index = c::unpackIndex(resource.id);
        const uint64_t generation = c::unpackGeneration(resource.id);
        const TextureHandle texture = {index, generation};
        const BufferHandle buffer = {index, generation};
        const RenderTargetHandle target = {index, generation};
        switch (resource.access)
        {
        case RASM_PASS_ACCESS_READ_TEXTURE:
            builder.read(texture);
            break;
        case RASM_PASS_ACCESS_READ_BUFFER:
            builder.read(buffer);
            break;
        case RASM_PASS_ACCESS_READ_RENDER_TARGET:
            builder.read(target);
            break;
        case RASM_PASS_ACCESS_READ_PREVIOUS_RENDER_TARGET:
            builder.readPrevious(target);
            break;
        case RASM_PASS_ACCESS_READ_PREVIOUS_BUFFER:
            builder.readPrevious(buffer);
            break;
        case RASM_PASS_ACCESS_READ_DEPTH:
            builder.readDepth(target);
            break;
        case RASM_PASS_ACCESS_WRITE_TEXTURE:
            builder.write(texture);
            break;
        case RASM_PASS_ACCESS_WRITE_BUFFER:
            builder.write(buffer);
            break;
        case RASM_PASS_ACCESS_WRITE_RENDER_TARGET:
            builder.write(target, resource.face);
            break;
        case RASM_PASS_ACCESS_WRITE_SWAPCHAIN:
            builder.writeSwapchain(WindowHandle{index, generation});
            break;
        default:
            break;
        }
    }

    // The resources' enums and handles: a swapchain write may name the main window with 0, every other one a handle.
    bool checkResources(const rasm_pass_resource *resources, uint32_t count, rasm_pass_type type, RenderTargetHandle &output, const char *function)
    {
        if (count > 0 && !c::checkPointer(resources, function, "desc->resources"))
            return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const rasm_pass_resource &resource = resources[i];
            const std::string name = "desc->resources[" + std::to_string(i) + "]";
            if (!c::checkEnum(resource.access, RASM_PASS_ACCESS_WRITE_SWAPCHAIN + 1, function, (name + ".access").c_str()))
                return false;
            if (resource.access != RASM_PASS_ACCESS_WRITE_SWAPCHAIN && !c::checkHandle(resource.id, function, (name + ".id").c_str()))
                return false;
            if (resource.access == RASM_PASS_ACCESS_WRITE_SWAPCHAIN && type != RASM_PASS_TYPE_GRAPHICS)
            {
                c::reportError(function, (name + ": only a graphics pass can write the swapchain.").c_str());
                return false;
            }
            if (resource.access == RASM_PASS_ACCESS_WRITE_RENDER_TARGET)
                output = {c::unpackIndex(resource.id), c::unpackGeneration(resource.id)};
        }
        return true;
    }

    bool checkTarget(rasm_render_target target, const char *function, const char *name)
    {
        return c::checkHandle(target.id, function, name);
    }
}

rasm_renderer::~rasm_renderer()
{
    // Each graph's C++ graph goes while the renderer it uses lives; the C graph stays for rasm_render_graph_destroy.
    for (rasm_render_graph *graph : graphs)
    {
        graph->graph.reset();
        graph->owner = nullptr;
    }
}

namespace rasm::c
{
    bool checkNotRecording(const rasm_renderer *renderer, const char *function)
    {
        if (renderer != nullptr && renderer->recording > 0)
        {
            reportError(function, "a render graph of this renderer is recording: call it outside the pass callbacks.");
            return false;
        }
        return true;
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_pass_desc_init(rasm_pass_desc *desc, uint32_t size)
try
{
    const rasm_pass_desc defaults = {.size = sizeof(rasm_pass_desc)};
    return c::initSized(desc, size, c::PASS_DESC_SIZE_1_17, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_shadow_desc_init(rasm_shadow_desc *desc, uint32_t size)
try
{
    const ShadowDesc shadow = {};
    const rasm_shadow_desc defaults = {.size = sizeof(rasm_shadow_desc), .cascade_count = shadow.cascadeCount, .map_size = shadow.size, .distance = shadow.distance, .split_lambda = shadow.splitLambda};
    return c::initSized(desc, size, c::SHADOW_DESC_SIZE_1_17, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_view_output_init(rasm_view_output *output, uint32_t size)
try
{
    const rasm_view_output defaults = {.size = sizeof(rasm_view_output), .rect = {0.0f, 0.0f, 1.0f, 1.0f}};
    return c::initSized(output, size, c::VIEW_OUTPUT_SIZE_1_17, &defaults, sizeof(defaults), __func__, "output");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_render_graph_create(rasm_renderer *renderer, rasm_render_graph **graph)
try
{
    if (!c::checkPointer(static_cast<const void *>(graph), __func__, "graph"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *graph = nullptr;
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    if (!c::checkNotRecording(renderer, __func__))
        return RASM_ERROR_FAILED;
    auto created = std::make_unique<rasm_render_graph>();
    created->owner = renderer;
    created->graph = std::make_unique<RenderGraph>(r->createRenderGraph());
    created->commands.renderer = r;
    created->commands.owner = renderer;
    created->serial = nextGraphSerial.fetch_add(1) & UINT32_MAX;
    renderer->graphs.push_back(created.get());
    *graph = created.release();
    return RASM_OK;
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_render_graph_destroy(rasm_render_graph *graph)
try
{
    if (graph == nullptr)
        return;
    if (graph->owner != nullptr)
    {
        if (!c::checkNotRecording(graph->owner, __func__))
            return;
        std::erase(graph->owner->graphs, graph);
    }
    delete graph;
}
RASM_C_CATCH_VOID

rasm_render_target RASM_CALL rasm_render_graph_create_render_target(rasm_render_graph *graph, const rasm_render_target_desc *desc)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    RenderTargetDesc target;
    if (g == nullptr || !c::readRenderTargetDesc(desc, target, __func__))
        return {};
    return c::toC(g->graph->createRenderTarget(target));
}
RASM_C_CATCH({})

// The callback goes into the C++ pass through a lambda of a few words, which the graph keeps: a frame allocates nothing.
rasm_result RASM_CALL rasm_render_graph_add_pass(rasm_render_graph *graph, const rasm_pass_desc *desc)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    rasm_pass_desc values = {.size = sizeof(rasm_pass_desc)};
    if (g == nullptr || !c::readSized(desc, c::PASS_DESC_SIZE_1_17, &values, sizeof(values), __func__, "desc") ||
        !c::checkEnum(values.type, RASM_PASS_TYPE_ASYNC_COMPUTE + 1, __func__, "desc->type"))
        return RASM_ERROR_INVALID_ARGUMENT;
    RenderTargetHandle output;
    if (!checkResources(values.resources, values.resource_count, values.type, output, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;

    const rasm_pass_desc *pass = &values;
    const auto setup = [pass](PassBuilder &builder)
    {
        for (uint32_t i = 0; i < pass->resource_count; ++i)
            addResource(builder, pass->resources[i]);
    };
    const rasm_pass_type type = values.type;
    const rasm_pass_callback callback = values.execute;
    void *userData = values.user_data;
    const auto execute = [g, type, callback, userData, output](RenderContext &ctx)
    {
        if (callback == nullptr)
            return;
        rasm_commands &cmd = g->commands;
        cmd.commandBuffer = cmd.renderer->getCurrentFrameResources().commandBuffer;
        cmd.pipeline = {};
        cmd.pipelineType = 0;
        cmd.indexBound = false;
        cmd.passType = type;
        cmd.width = 0;
        cmd.height = 0;
        if (type == RASM_PASS_TYPE_GRAPHICS)
        {
            const Viewport &viewport = ctx.getViewport();
            cmd.width = static_cast<uint32_t>(viewport.width);
            cmd.height = static_cast<uint32_t>(viewport.height);
        }
        else if (output.isValid() && ctx.isAlive(output))
        {
            const RenderTarget &target = ctx.getRenderTarget(output);
            cmd.width = target.width;
            cmd.height = target.height;
        }
        const ActiveCommands active(cmd);
        callback(userData, &cmd);
    };
    const c::CallErrors errors;
    std::string name = values.name != nullptr ? values.name : "";
    if (type == RASM_PASS_TYPE_GRAPHICS)
        g->graph->addPass(std::move(name), setup, execute);
    else if (type == RASM_PASS_TYPE_COMPUTE)
        g->graph->addComputePass(std::move(name), setup, execute);
    else
        g->graph->addAsyncComputePass(std::move(name), setup, execute);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_shadow RASM_CALL rasm_render_graph_add_shadow_passes(rasm_render_graph *graph, rasm_scene scene, rasm_camera camera, const rasm_shadow_desc *desc)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    const ShadowDesc shadow = {};
    rasm_shadow_desc values = {.size = sizeof(rasm_shadow_desc), .cascade_count = shadow.cascadeCount, .map_size = shadow.size, .distance = shadow.distance, .split_lambda = shadow.splitLambda};
    if (g == nullptr || !c::checkHandle(scene.id, __func__, "scene") || !c::checkHandle(camera.id, __func__, "camera") ||
        !c::readSized(desc, c::SHADOW_DESC_SIZE_1_17, &values, sizeof(values), __func__, "desc") || !c::checkHandle(values.light.id, __func__, "desc->light"))
        return {};
    const ShadowDesc maps = {.light = c::toCpp(values.light), .cascadeCount = values.cascade_count, .size = values.map_size, .distance = values.distance, .splitLambda = values.split_lambda};
    const SceneShadow added = g->owner->renderer->addShadowPasses(*g->graph, c::toCpp(scene), c::toCpp(camera), maps);
    if (added.mapCount == 0)
        return {};
    g->shadows.push_back(added);
    return {g->serial << 32 | static_cast<uint64_t>(g->shadows.size())};
}
RASM_C_CATCH({})

// The shadows and the sampled targets are copied once, when the graph is built, not each frame.
rasm_result RASM_CALL rasm_render_graph_add_scene_passes(rasm_render_graph *graph, rasm_scene scene, rasm_camera camera, rasm_render_target target, const rasm_shadow *shadows, uint32_t shadow_count,
                                                         const rasm_render_target *sampled, uint32_t sampled_count)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    if (g == nullptr || !c::checkHandle(scene.id, __func__, "scene") || !c::checkHandle(camera.id, __func__, "camera") || !checkTarget(target, __func__, "target") ||
        (shadow_count > 0 && !c::checkPointer(shadows, __func__, "shadows")) || (sampled_count > 0 && !c::checkPointer(sampled, __func__, "sampled")))
        return RASM_ERROR_INVALID_ARGUMENT;
    std::vector<SceneShadow> maps;
    maps.reserve(shadow_count);
    for (uint32_t i = 0; i < shadow_count; ++i)
    {
        const uint64_t index = c::unpackIndex(shadows[i].id);
        if (c::unpackGeneration(shadows[i].id) != g->serial || index == 0 || index > g->shadows.size())
        {
            c::reportError(__func__, ("shadows[" + std::to_string(i) + "] is not a shadow of this graph.").c_str());
            return RASM_ERROR_INVALID_ARGUMENT;
        }
        maps.push_back(g->shadows[index - 1]);
    }
    std::vector<RenderTargetHandle> targets(sampled_count);
    for (uint32_t i = 0; i < sampled_count; ++i)
        targets[i] = c::toCpp(sampled[i]);
    const c::CallErrors errors;
    const bool added = g->owner->renderer->addScenePasses(*g->graph, c::toCpp(scene), c::toCpp(camera), c::toCpp(target), maps.data(), shadow_count, targets.data(), sampled_count);
    return added ? errors.result() : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_render_target RASM_CALL rasm_render_graph_add_msaa_resolve_pass(rasm_render_graph *graph, rasm_render_target source, rasm_camera camera)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    if (g == nullptr || !checkTarget(source, __func__, "source") || !c::checkHandle(camera.id, __func__, "camera"))
        return {};
    return c::toC(g->owner->renderer->addMsaaResolvePass(*g->graph, c::toCpp(source), c::toCpp(camera)));
}
RASM_C_CATCH({})

rasm_render_target RASM_CALL rasm_render_graph_add_taa_pass(rasm_render_graph *graph, rasm_render_target source, rasm_camera camera)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    if (g == nullptr || !checkTarget(source, __func__, "source") || !c::checkHandle(camera.id, __func__, "camera"))
        return {};
    return c::toC(g->owner->renderer->addTaaPass(*g->graph, c::toCpp(source), c::toCpp(camera)));
}
RASM_C_CATCH({})

rasm_render_target RASM_CALL rasm_render_graph_add_bloom_passes(rasm_render_graph *graph, rasm_render_target source, rasm_camera camera)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    if (g == nullptr || !checkTarget(source, __func__, "source") || !c::checkHandle(camera.id, __func__, "camera"))
        return {};
    return c::toC(g->owner->renderer->addBloomPasses(*g->graph, c::toCpp(source), c::toCpp(camera)));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_render_graph_add_tone_map_pass(rasm_render_graph *graph, rasm_render_target source, rasm_camera camera, rasm_render_target bloom, const rasm_view_output *output)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    rasm_view_output values = {.size = sizeof(rasm_view_output), .rect = {0.0f, 0.0f, 1.0f, 1.0f}};
    if (g == nullptr || !checkTarget(source, __func__, "source") || !c::checkHandle(camera.id, __func__, "camera") ||
        (output != nullptr && !c::readSized(output, c::VIEW_OUTPUT_SIZE_1_17, &values, sizeof(values), __func__, "output")))
        return RASM_ERROR_INVALID_ARGUMENT;
    const ViewOutput viewOutput = {.rect = c::toCpp(values.rect), .target = c::toCpp(values.target), .window = c::toCpp(values.window)};
    const c::CallErrors errors;
    const bool added = g->owner->renderer->addToneMapPass(*g->graph, c::toCpp(source), c::toCpp(camera), c::toCpp(bloom), viewOutput);
    return added ? errors.result() : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_render_graph_add_ui_pass(rasm_render_graph *graph, const rasm_render_target *shown, uint32_t shown_count)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    if (g == nullptr || (shown_count > 0 && !c::checkPointer(shown, __func__, "shown")))
        return RASM_ERROR_INVALID_ARGUMENT;
    std::vector<RenderTargetHandle> targets(shown_count);
    for (uint32_t i = 0; i < shown_count; ++i)
        targets[i] = c::toCpp(shown[i]);
    const c::CallErrors errors;
    const bool added = g->owner->renderer->addUiPass(*g->graph, targets.data(), shown_count);
    return added ? errors.result() : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_render_graph_compile(rasm_render_graph *graph)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    if (g == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    g->graph->compile();
    return errors.result();
}
RASM_C_CATCH_RESULT

// The renderer records while the graph runs, so the calls that would start a frame, change a graph or free what the
// frame uses are refused from the pass callbacks.
rasm_result RASM_CALL rasm_render_graph_execute(rasm_render_graph *graph, rasm_camera camera)
try
{
    rasm_render_graph *g = getGraph(graph, __func__);
    if (g == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    {
        const Recording recording(*g->owner);
        g->graph->execute(c::toCpp(camera));
    }
    return errors.result();
}
RASM_C_CATCH_RESULT

uint32_t RASM_CALL rasm_render_graph_dump(rasm_render_graph *graph, char *buffer, uint32_t capacity)
try
{
    rasm_render_graph *g = getGraph(graph, __func__, false);
    return g != nullptr ? c::copyString(g->graph->dump(), buffer, capacity) : 0;
}
RASM_C_CATCH(0)

rasm_result RASM_CALL rasm_cmd_get_info(rasm_commands *commands, rasm_pass_info *info)
try
{
    rasm_commands *cmd = getCommands(commands, __func__);
    if (cmd == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const rasm_pass_info values = {.size = sizeof(rasm_pass_info), .type = cmd->passType, .width = cmd->width, .height = cmd->height};
    return c::writeSized(info, c::PASS_INFO_SIZE_1_17, &values, sizeof(values), __func__, "info") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_bind_pipeline(rasm_commands *commands, rasm_pipeline pipeline)
try
{
    rasm_commands *cmd = getCommands(commands, __func__);
    if (cmd == nullptr || !c::checkHandle(pipeline.id, __func__, "pipeline"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const PipelineHandle handle = c::toCpp(pipeline);
    const auto &types = cmd->owner->pipelineTypes;
    const uint8_t type = handle.index < types.size() ? types[handle.index] : 0;
    if (type == 0)
    {
        c::reportError(__func__, "pipeline is not one rasm_create_pipeline made.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    const bool compute = type == RASM_PIPELINE_TYPE_COMPUTE + 1;
    if (compute != (cmd->passType != RASM_PASS_TYPE_GRAPHICS))
    {
        c::reportError(__func__, compute ? "a graphics pass cannot bind a compute pipeline." : "a compute pass cannot bind a graphics pipeline.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    RenderContext &ctx = cmd->renderer->getRenderContext();
    ctx.bindPipeline(cmd->commandBuffer, handle);
    ctx.bindDescriptorSet(cmd->commandBuffer, handle, cmd->renderer->getBindlessDescriptorSet(), 0);
    cmd->pipeline = handle;
    cmd->pipelineType = type;
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_push_constants(rasm_commands *commands, const void *data, uint32_t size, uint32_t offset)
try
{
    rasm_commands *cmd = getCommands(commands, __func__);
    if (cmd == nullptr || !c::checkPointer(data, __func__, "data"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (cmd->pipelineType == 0)
    {
        c::reportError(__func__, "no pipeline is bound: call rasm_cmd_bind_pipeline first.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0 || size % 4 != 0 || offset % 4 != 0 || offset > PUSH_CONSTANTS_SIZE || size > PUSH_CONSTANTS_SIZE - offset)
    {
        c::reportError(__func__, "size and offset must be multiples of 4, size above 0, and offset + size at most RASM_PUSH_CONSTANTS_SIZE.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    cmd->renderer->getRenderContext().pushConstants(cmd->commandBuffer, cmd->pipeline, data, size, offset);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_set_viewport(rasm_commands *commands, float x, float y, float width, float height, float min_depth, float max_depth)
try
{
    rasm_commands *cmd = getGraphicsCommands(commands, __func__);
    if (cmd == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    return cmd->renderer->getRenderContext().setViewport(cmd->commandBuffer, x, y, width, height, min_depth, max_depth) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_set_scissor(rasm_commands *commands, int32_t x, int32_t y, uint32_t width, uint32_t height)
try
{
    rasm_commands *cmd = getGraphicsCommands(commands, __func__);
    if (cmd == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    return cmd->renderer->getRenderContext().setScissor(cmd->commandBuffer, x, y, width, height) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_set_cull_mode(rasm_commands *commands, rasm_cull_mode cull_mode)
try
{
    rasm_commands *cmd = getGraphicsCommands(commands, __func__);
    if (cmd == nullptr || !c::checkEnum(cull_mode, RASM_CULL_MODE_BACK + 1, __func__, "cull_mode"))
        return RASM_ERROR_INVALID_ARGUMENT;
    return cmd->renderer->getRenderContext().setCullMode(cmd->commandBuffer, static_cast<CullMode>(cull_mode)) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_draw(rasm_commands *commands, uint32_t vertex_count, uint32_t instance_count)
try
{
    rasm_commands *cmd = getRecording(commands, false, __func__);
    if (cmd == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    cmd->renderer->getRenderContext().draw(cmd->commandBuffer, vertex_count, instance_count);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_bind_index_buffer(rasm_commands *commands, rasm_buffer buffer, uint64_t offset, rasm_format index_format)
try
{
    rasm_commands *cmd = getGraphicsCommands(commands, __func__);
    if (cmd == nullptr || !c::checkHandle(buffer.id, __func__, "buffer"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (index_format != RASM_FORMAT_U16_UINT && index_format != RASM_FORMAT_U32_UINT)
    {
        c::reportError(__func__, "index_format must be RASM_FORMAT_U16_UINT or RASM_FORMAT_U32_UINT.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    cmd->renderer->getRenderContext().bindIndexBuffer(cmd->commandBuffer, c::toCpp(buffer), offset, static_cast<Format>(index_format));
    cmd->indexBound = true;
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_draw_indexed(rasm_commands *commands, uint32_t index_count, uint32_t instance_count, uint32_t vertex_offset, uint32_t first_index, uint32_t first_instance)
try
{
    rasm_commands *cmd = getIndexedRecording(commands, __func__);
    if (cmd == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    cmd->renderer->getRenderContext().drawIndexed(cmd->commandBuffer, index_count, instance_count, vertex_offset, first_index, first_instance);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_draw_indexed_indirect(rasm_commands *commands, rasm_buffer buffer, uint64_t offset, uint32_t draw_count, uint32_t stride)
try
{
    rasm_commands *cmd = getIndexedRecording(commands, __func__);
    if (cmd == nullptr || !c::checkHandle(buffer.id, __func__, "buffer"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (offset > UINT32_MAX)
    {
        c::reportError(__func__, "offset must be below 4 GiB.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    cmd->renderer->getRenderContext().drawIndexedIndirect(cmd->commandBuffer, c::toCpp(buffer), static_cast<uint32_t>(offset), draw_count, stride);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_draw_indexed_indirect_count(rasm_commands *commands, rasm_buffer buffer, uint64_t offset, rasm_buffer count_buffer, uint64_t count_offset, uint32_t max_draw_count,
                                                           uint32_t stride)
try
{
    rasm_commands *cmd = getIndexedRecording(commands, __func__);
    if (cmd == nullptr || !c::checkHandle(buffer.id, __func__, "buffer") || !c::checkHandle(count_buffer.id, __func__, "count_buffer"))
        return RASM_ERROR_INVALID_ARGUMENT;
    cmd->renderer->getRenderContext().drawIndexedIndirectCount(cmd->commandBuffer, c::toCpp(buffer), offset, c::toCpp(count_buffer), count_offset, max_draw_count, stride);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_dispatch(rasm_commands *commands, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
try
{
    rasm_commands *cmd = getRecording(commands, true, __func__);
    if (cmd == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    cmd->renderer->getRenderContext().dispatch(cmd->commandBuffer, group_count_x, group_count_y, group_count_z);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cmd_dispatch_indirect(rasm_commands *commands, rasm_buffer buffer, uint64_t offset)
try
{
    rasm_commands *cmd = getRecording(commands, true, __func__);
    if (cmd == nullptr || !c::checkHandle(buffer.id, __func__, "buffer"))
        return RASM_ERROR_INVALID_ARGUMENT;
    cmd->renderer->getRenderContext().dispatchIndirect(cmd->commandBuffer, c::toCpp(buffer), offset);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

// Between the dispatches of one compute pass; the graph places the barriers between passes itself.
rasm_result RASM_CALL rasm_cmd_memory_barrier(rasm_commands *commands, rasm_pipeline_stage before, rasm_pipeline_stage after)
try
{
    rasm_commands *cmd = getCommands(commands, __func__);
    if (cmd == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    constexpr uint32_t stages = RASM_PIPELINE_STAGE_COMPUTE | RASM_PIPELINE_STAGE_GRAPHICS | RASM_PIPELINE_STAGE_HOST;
    if (cmd->passType == RASM_PASS_TYPE_GRAPHICS || before == 0 || after == 0 || (before & ~stages) != 0 || (after & ~stages) != 0)
    {
        c::reportError(__func__, "only a compute pass places barriers, between RASM_PIPELINE_STAGE_ flags.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    cmd->renderer->getRenderContext().memoryBarrier(cmd->commandBuffer, static_cast<PipelineStage>(before), static_cast<PipelineStage>(after));
    return RASM_OK;
}
RASM_C_CATCH_RESULT

uint32_t RASM_CALL rasm_cmd_begin_gpu_timer(rasm_commands *commands, const char *name)
try
{
    rasm_commands *cmd = getCommands(commands, __func__);
    if (cmd == nullptr || !c::checkPointer(name, __func__, "name"))
        return RASM_INVALID_INDEX;
    cmd->timerName = name;
    return cmd->renderer->beginGpuTimer(cmd->commandBuffer, cmd->timerName);
}
RASM_C_CATCH(RASM_INVALID_INDEX)

rasm_result RASM_CALL rasm_cmd_end_gpu_timer(rasm_commands *commands, uint32_t timer)
try
{
    rasm_commands *cmd = getCommands(commands, __func__);
    if (cmd == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    if (timer >= MAX_GPU_TIMERS && timer != RASM_INVALID_INDEX)
    {
        c::reportError(__func__, "timer is not one rasm_cmd_begin_gpu_timer gave.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    cmd->renderer->endGpuTimer(cmd->commandBuffer, timer);
    return RASM_OK;
}
RASM_C_CATCH_RESULT
// NOLINTEND(readability-identifier-naming)
