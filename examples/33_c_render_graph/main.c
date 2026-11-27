#include "rasm/rasm_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A frame built from C with the render graph (C API 1.17), compiled as C99.
 *
 * RASM's own passes draw the DamagedHelmet on a ground under a sun: its shadow maps, the scene into an HDR target with
 * motion, TAA, bloom, and the tone map into an LDR target. Three passes written here follow, each a C function the
 * graph calls while it records the frame:
 *
 *   Histogram (compute)  64 bins of the LDR image's brightness, added up in each group's shared memory, then with
 *                        atomics into a buffer through its device address; a first dispatch clears the bins, a
 *                        barrier orders the two.
 *   Adapt (graphics)     a 1 x 1 history target: the mean brightness of the bins, eased from the frame before's, which
 *                        the pass reads as the target's previous color (ping-pong between the frame slots).
 *   Grade (graphics)     the LDR image into the window: an exposure toward the adapted brightness, a tint, a vignette,
 *                        and the histogram drawn as bars in the bottom-left corner. Its parameters are more than the
 *                        16 bytes of push constants, so they sit in a buffer whose address goes in the push constants.
 *
 * RASM's UI pass draws last. A headless run checks the frame: the bins add up to the image's pixels, the histogram's
 * panel has its color, every Adapt pass read the target its last frame wrote, and the C passes have GPU timers. It exits
 * with 1 when one fails or the frame was not written.
 *
 * Flags, as the C++ examples take them: --headless, --frames N (60), --out <file.png> (frame.png), --fixed-step,
 * --wait-for-loads, --no-cache, and --dump (print the graph's passes).
 */

#define WIDTH 1280
#define HEIGHT 720
#define CHECKER_SIZE 256
#define BINS 64
#define PANEL_X 16
#define PANEL_WIDTH (4 * BINS)
#define PANEL_HEIGHT 96
#define PANEL_GRAY 0.05f

typedef struct ExampleArgs
{
    int headless;
    int fixedStep;
    int waitForLoads;
    int noCache;
    int dump;
    unsigned frames;
    const char *out;
} ExampleArgs;

/* What the Grade pass reads through the address in its push constants: the bins and its settings. */
typedef struct GradeParams
{
    uint64_t bins;
    float vignette;
    float radius;
    float targetBrightness;
    float exposureLimit;
    float tint[3];
    float panelGray;
    uint32_t panelX;
    uint32_t panelWidth;
    uint32_t panelHeight;
    uint32_t padding;
} GradeParams;

/* What the pass callbacks use. */
typedef struct App
{
    rasm_renderer *renderer;
    rasm_render_target ldr;
    rasm_render_target adapt;
    rasm_buffer bins;
    rasm_buffer params;
    uint64_t binsAddress;
    uint64_t paramsAddress;
    rasm_pipeline histogram;
    rasm_pipeline adaptPipeline;
    rasm_pipeline grade;
    /* The Adapt pass's check of the history target: what it wrote the frame before, and how many frames read it. */
    uint64_t lastWritten;
    unsigned adaptFrames;
    unsigned historyErrors;
    unsigned commandErrors;
} App;

static ExampleArgs parseArgs(int argc, char **argv)
{
    ExampleArgs args = {0, 0, 0, 0, 0, 60, "frame.png"};
    int i;
    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--headless") == 0)
            args.headless = 1;
        else if (strcmp(argv[i], "--fixed-step") == 0)
            args.fixedStep = 1;
        else if (strcmp(argv[i], "--wait-for-loads") == 0)
            args.waitForLoads = 1;
        else if (strcmp(argv[i], "--no-cache") == 0)
            args.noCache = 1;
        else if (strcmp(argv[i], "--dump") == 0)
            args.dump = 1;
        else if (i + 1 < argc && strcmp(argv[i], "--frames") == 0)
        {
            const int frames = atoi(argv[++i]);
            args.frames = frames > 0 ? (unsigned)frames : 1u;
        }
        else if (i + 1 < argc && strcmp(argv[i], "--out") == 0)
            args.out = argv[++i];
    }
    return args;
}

static rasm_vec3 vec3(float x, float y, float z)
{
    rasm_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static rasm_vec3 normalize(rasm_vec3 v)
{
    const float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return vec3(v.x / length, v.y / length, v.z / length);
}

/* A camera at eye that looks at the origin from the +Z side, its yaw and pitch made from the direction. */
static rasm_transform lookAtOrigin(rasm_vec3 eye)
{
    const rasm_vec3 back = normalize(eye);
    const float yaw = atan2f(back.x, back.z);
    const float pitch = -asinf(back.y);
    const float cy = cosf(0.5f * yaw), sy = sinf(0.5f * yaw), cp = cosf(0.5f * pitch), sp = sinf(0.5f * pitch);
    rasm_transform transform;
    /* yaw around Y, then pitch around the turned X */
    transform.rotation.w = cy * cp;
    transform.rotation.x = cy * sp;
    transform.rotation.y = sy * cp;
    transform.rotation.z = -sy * sp;
    transform.position = eye;
    transform.scale = vec3(1.0f, 1.0f, 1.0f);
    return transform;
}

static rasm_transform placed(rasm_vec3 position, float yaw, float scale)
{
    rasm_transform transform;
    transform.position = position;
    transform.rotation.x = 0.0f;
    transform.rotation.y = sinf(0.5f * yaw);
    transform.rotation.z = 0.0f;
    transform.rotation.w = cosf(0.5f * yaw);
    transform.scale = vec3(scale, scale, scale);
    return transform;
}

/* A ground square of 2 x 2 on y = 0, facing up, with a checker of light and dark gray. */
static rasm_mesh createGround(rasm_renderer *renderer)
{
    static const float corners[4][2] = {{-1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, -1.0f}};
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    rasm_vertex vertices[4];
    int i, set;
    memset(vertices, 0, sizeof(vertices));
    for (i = 0; i < 4; ++i)
    {
        vertices[i].position = vec3(corners[i][0], 0.0f, corners[i][1]);
        vertices[i].normal = vec3(0.0f, 1.0f, 0.0f);
        for (set = 0; set < RASM_MAX_VERTEX_UV_SETS; ++set)
        {
            vertices[i].uv[set].x = (corners[i][0] * 0.5f + 0.5f) * 6.0f;
            vertices[i].uv[set].y = (corners[i][1] * 0.5f + 0.5f) * 6.0f;
        }
    }
    return rasm_create_mesh(renderer, vertices, 4, indices, 6, NULL);
}

static rasm_material createGroundMaterial(rasm_renderer *renderer)
{
    static uint8_t pixels[CHECKER_SIZE * CHECKER_SIZE * 4];
    rasm_texture_desc desc;
    rasm_texture checker;
    int x, y;
    for (y = 0; y < CHECKER_SIZE; ++y)
    {
        for (x = 0; x < CHECKER_SIZE; ++x)
        {
            const uint8_t value = ((x / 32) + (y / 32)) % 2 == 0 ? 200 : 90;
            uint8_t *texel = &pixels[(y * CHECKER_SIZE + x) * 4];
            texel[0] = value;
            texel[1] = value;
            texel[2] = value;
            texel[3] = 255;
        }
    }
    rasm_texture_desc_init(&desc, sizeof(desc));
    desc.name = "checker";
    desc.width = CHECKER_SIZE;
    desc.height = CHECKER_SIZE;
    desc.format = RASM_FORMAT_R8G8B8A8_SRGB;
    checker = rasm_create_texture(renderer, &desc, pixels);
    return rasm_create_material(renderer, RASM_MATERIAL_TYPE_PBR, &checker, 1, RASM_FALSE);
}

/* The shaders, in Slang. Each pipeline sees RASM's bindless textures at set 0 and 16 bytes of push constants. */
static const char *HISTOGRAM_SHADER =
    "[[vk::binding(0, 0)]] Sampler2D gTextures[];\n"
    "struct Push { uint* bins; uint source; uint clear; };\n"
    "[[vk::push_constant]] ConstantBuffer<Push> pc;\n"
    "groupshared uint gBins[64];\n"
    "[shader(\"compute\")] [numthreads(16, 16, 1)]\n"
    "void compMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)\n"
    "{\n"
    "    if (pc.clear != 0) { if (index < 64) pc.bins[index] = 0; return; }\n"
    "    if (index < 64) gBins[index] = 0;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    uint width, height;\n"
    "    gTextures[pc.source].GetDimensions(width, height);\n"
    "    if (id.x < width && id.y < height)\n"
    "    {\n"
    "        float3 color = gTextures[pc.source].SampleLevel((float2(id.xy) + 0.5) / float2(width, height), 0).rgb;\n"
    "        float brightness = sqrt(saturate(dot(color, float3(0.2126, 0.7152, 0.0722))));\n"
    "        InterlockedAdd(gBins[min(uint(brightness * 64.0), 63u)], 1u);\n"
    "    }\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (index < 64 && gBins[index] != 0) InterlockedAdd(pc.bins[index], gBins[index]);\n"
    "}\n";

/* One triangle over the whole output; uv (0, 0) is the top-left corner. */
#define FULL_SCREEN                                                                               \
    "[[vk::binding(0, 0)]] Sampler2D gTextures[];\n"                                              \
    "struct VSOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };\n"                \
    "[shader(\"vertex\")] VSOutput vertMain(uint vertexID : SV_VertexID)\n"                       \
    "{\n"                                                                                         \
    "    VSOutput output;\n"                                                                      \
    "    output.uv = float2((vertexID << 1) & 2, vertexID & 2);\n"                                \
    "    output.position = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);\n" \
    "    return output;\n"                                                                        \
    "}\n"

static const char *ADAPT_SHADER = FULL_SCREEN
    "struct Push { uint* bins; uint previous; uint padding; };\n"
    "[[vk::push_constant]] ConstantBuffer<Push> pc;\n"
    "[shader(\"fragment\")] float4 fragMain(VSOutput input) : SV_TARGET\n"
    "{\n"
    "    float sum = 0.0, weighted = 0.0;\n"
    "    for (uint i = 0; i < 64; ++i) { sum += float(pc.bins[i]); weighted += float(pc.bins[i]) * (float(i) + 0.5) / 64.0; }\n"
    "    float mean = sum > 0.0 ? weighted / sum : 0.5;\n"
    "    float previous = gTextures[pc.previous].SampleLevel(float2(0.5, 0.5), 0).r;\n"
    "    float adapted = previous > 0.0 ? lerp(previous, mean, 0.1) : mean;\n"
    "    return float4(adapted, adapted, adapted, 1.0);\n"
    "}\n";

static const char *GRADE_SHADER = FULL_SCREEN
    "struct Params { uint* bins; float vignette; float radius; float targetBrightness; float exposureLimit;\n"
    "                float tintR; float tintG; float tintB; float panelGray; uint panelX; uint panelWidth; uint panelHeight; uint padding; };\n"
    "struct Push { Params* params; uint source; uint adapt; };\n"
    "[[vk::push_constant]] ConstantBuffer<Push> pc;\n"
    "[shader(\"fragment\")] float4 fragMain(VSOutput input) : SV_TARGET\n"
    "{\n"
    "    Params p = *pc.params;\n"
    "    uint width, height;\n"
    "    gTextures[pc.source].GetDimensions(width, height);\n"
    "    float2 pixel = input.position.xy;\n"
    "    float panelTop = float(height) - 16.0 - float(p.panelHeight);\n"
    "    if (pixel.x >= float(p.panelX) && pixel.x < float(p.panelX + p.panelWidth) && pixel.y >= panelTop && pixel.y < panelTop + float(p.panelHeight))\n"
    "    {\n"
    "        uint most = 1;\n"
    "        for (uint i = 0; i < 64; ++i) most = max(most, p.bins[i]);\n"
    "        uint bin = uint(pixel.x - float(p.panelX)) / 4;\n"
    "        float bar = float(p.bins[bin]) / float(most) * 0.9 * float(p.panelHeight);\n"
    "        bool inBar = panelTop + float(p.panelHeight) - pixel.y < bar;\n"
    "        return inBar ? float4(0.9, 0.75, 0.35, 1.0) : float4(p.panelGray, p.panelGray, p.panelGray, 1.0);\n"
    "    }\n"
    "    float3 color = gTextures[pc.source].Sample(input.uv).rgb;\n"
    "    float adapted = gTextures[pc.adapt].SampleLevel(float2(0.5, 0.5), 0).r;\n"
    "    float exposure = clamp(p.targetBrightness / max(adapted, 0.001), 1.0 / p.exposureLimit, p.exposureLimit);\n"
    "    color *= exposure * exposure * float3(p.tintR, p.tintG, p.tintB);\n"
    "    float distance = length(input.uv - 0.5) / 0.7071;\n"
    "    color *= 1.0 - p.vignette * smoothstep(p.radius, 1.0, distance);\n"
    "    return float4(color, 1.0);\n"
    "}\n";

static rasm_pipeline createPipeline(rasm_renderer *renderer, const char *name, rasm_pipeline_type type, const char *source)
{
    rasm_pipeline_desc desc;
    rasm_pipeline_desc_init(&desc, sizeof(desc));
    desc.name = name;
    desc.type = type;
    desc.source = source;
    return rasm_create_pipeline(renderer, &desc);
}

/* The push constants: a device address and two 32-bit values, 16 bytes. */
static void pushAddress(App *app, rasm_commands *cmd, uint64_t address, uint32_t a, uint32_t b)
{
    uint32_t words[4];
    words[0] = (uint32_t)(address & 0xFFFFFFFFu);
    words[1] = (uint32_t)(address >> 32);
    words[2] = a;
    words[3] = b;
    if (rasm_cmd_push_constants(cmd, words, sizeof(words), 0) != RASM_OK)
        ++app->commandErrors;
}

static uint32_t textureIndex(App *app, rasm_texture texture)
{
    return rasm_get_bindless_texture_index(app->renderer, texture);
}

static void RASM_CALL recordHistogram(void *userData, rasm_commands *cmd)
{
    App *app = (App *)userData;
    const uint32_t source = textureIndex(app, rasm_get_render_target_color(app->renderer, app->ldr, 0));
    rasm_pass_info info;
    uint32_t timer;
    info.size = sizeof(info);
    rasm_cmd_get_info(cmd, &info);
    if (rasm_cmd_bind_pipeline(cmd, app->histogram) != RASM_OK)
    {
        ++app->commandErrors;
        return;
    }
    /* The frames before read and wrote the bins: wait for them, clear, wait for the clear, then count. */
    timer = rasm_cmd_begin_gpu_timer(cmd, "Histogram clear");
    rasm_cmd_memory_barrier(cmd, RASM_PIPELINE_STAGE_COMPUTE | RASM_PIPELINE_STAGE_GRAPHICS, RASM_PIPELINE_STAGE_COMPUTE);
    pushAddress(app, cmd, app->binsAddress, source, 1);
    rasm_cmd_dispatch(cmd, 1, 1, 1);
    rasm_cmd_end_gpu_timer(cmd, timer);
    rasm_cmd_memory_barrier(cmd, RASM_PIPELINE_STAGE_COMPUTE, RASM_PIPELINE_STAGE_COMPUTE);
    pushAddress(app, cmd, app->binsAddress, source, 0);
    /* The LDR target is the window's size: the graph's target at swapchain scale 1. The pass writes no render target,
     * so its info has no size; the window's comes from the renderer. */
    {
        rasm_window_info window;
        window.size = sizeof(window);
        rasm_get_window(app->renderer, (rasm_window){0}, &window);
        if (rasm_cmd_dispatch(cmd, (window.pixel_width + 15) / 16, (window.pixel_height + 15) / 16, 1) != RASM_OK)
            ++app->commandErrors;
    }
    /* The CPU reads the bins after the last frame. */
    rasm_cmd_memory_barrier(cmd, RASM_PIPELINE_STAGE_COMPUTE, RASM_PIPELINE_STAGE_HOST);
    if (info.width != 0)
        ++app->commandErrors;
}

/* The adapted brightness ping-pongs between the history target's two copies: this frame writes one and reads the
 * other, which the frame before wrote. */
static void RASM_CALL recordAdapt(void *userData, rasm_commands *cmd)
{
    App *app = (App *)userData;
    const rasm_texture written = rasm_get_render_target_color(app->renderer, app->adapt, 0);
    const rasm_texture previous = rasm_get_previous_render_target_color(app->renderer, app->adapt, 0);
    if (written.id == previous.id || (app->adaptFrames > 0 && previous.id != app->lastWritten))
        ++app->historyErrors;
    app->lastWritten = written.id;
    ++app->adaptFrames;
    if (rasm_cmd_bind_pipeline(cmd, app->adaptPipeline) != RASM_OK)
    {
        ++app->commandErrors;
        return;
    }
    pushAddress(app, cmd, app->binsAddress, textureIndex(app, previous), 0);
    if (rasm_cmd_draw(cmd, 3, 1) != RASM_OK)
        ++app->commandErrors;
}

static void RASM_CALL recordGrade(void *userData, rasm_commands *cmd)
{
    App *app = (App *)userData;
    const uint32_t source = textureIndex(app, rasm_get_render_target_color(app->renderer, app->ldr, 0));
    const uint32_t adapted = textureIndex(app, rasm_get_render_target_color(app->renderer, app->adapt, 0));
    if (rasm_cmd_bind_pipeline(cmd, app->grade) != RASM_OK)
    {
        ++app->commandErrors;
        return;
    }
    pushAddress(app, cmd, app->paramsAddress, source, adapted);
    if (rasm_cmd_draw(cmd, 3, 1) != RASM_OK)
        ++app->commandErrors;
}

static int fail(const char *what)
{
    fprintf(stderr, "error: %s: %s\n", what, rasm_get_last_error());
    return 1;
}

static rasm_render_target createTarget(rasm_render_graph *graph, const char *name, rasm_format color, rasm_format motion, rasm_format depth, int history)
{
    rasm_render_target_desc desc;
    rasm_render_target_desc_init(&desc, sizeof(desc));
    desc.name = name;
    desc.format = color;
    desc.more_color_formats[0] = motion;
    desc.depth_format = depth;
    desc.clear_color.x = desc.clear_color.y = desc.clear_color.z = desc.clear_color.w = 0.0f;
    if (history)
    {
        desc.width = 1;
        desc.height = 1;
        desc.history = RASM_TRUE;
    }
    else
        desc.swapchain_scale = 1.0f;
    return rasm_render_graph_create_render_target(graph, &desc);
}

static rasm_result addPass(rasm_render_graph *graph, const char *name, rasm_pass_type type, const rasm_pass_resource *resources, uint32_t count, rasm_pass_callback execute, App *app)
{
    rasm_pass_desc desc;
    rasm_pass_desc_init(&desc, sizeof(desc));
    desc.name = name;
    desc.type = type;
    desc.resources = resources;
    desc.resource_count = count;
    desc.execute = execute;
    desc.user_data = app;
    return rasm_render_graph_add_pass(graph, &desc);
}

static rasm_pass_resource use(rasm_pass_access access, uint64_t id)
{
    rasm_pass_resource resource;
    resource.access = access;
    resource.face = 0;
    resource.id = id;
    return resource;
}

/* RASM's passes, then the three of this file and the UI. */
static int buildGraph(rasm_render_graph *graph, App *app, rasm_scene scene, rasm_camera camera, rasm_light sun)
{
    rasm_shadow_desc shadowDesc;
    rasm_shadow shadow;
    rasm_render_target hdr, resolved, bloom;
    rasm_view_output output;
    rasm_pass_resource resources[4];

    rasm_shadow_desc_init(&shadowDesc, sizeof(shadowDesc));
    shadowDesc.light = sun;
    shadowDesc.distance = 12.0f;
    shadow = rasm_render_graph_add_shadow_passes(graph, scene, camera, &shadowDesc);
    hdr = createTarget(graph, "scene_hdr", RASM_SCENE_COLOR_FORMAT, RASM_MOTION_FORMAT, RASM_FORMAT_D32_SFLOAT, 0);
    app->ldr = createTarget(graph, "ldr", RASM_FORMAT_R8G8B8A8_SRGB, RASM_FORMAT_UNKNOWN, RASM_FORMAT_UNKNOWN, 0);
    app->adapt = createTarget(graph, "adapt", RASM_FORMAT_R16G16B16A16_SFLOAT, RASM_FORMAT_UNKNOWN, RASM_FORMAT_UNKNOWN, 1);
    if (shadow.id == 0 || hdr.id == 0 || app->ldr.id == 0 || app->adapt.id == 0 || rasm_render_graph_add_scene_passes(graph, scene, camera, hdr, &shadow, 1, NULL, 0) != RASM_OK)
        return 0;
    resolved = rasm_render_graph_add_taa_pass(graph, hdr, camera);
    bloom = rasm_render_graph_add_bloom_passes(graph, resolved, camera);
    rasm_view_output_init(&output, sizeof(output));
    output.target = app->ldr;
    if (resolved.id == 0 || bloom.id == 0 || rasm_render_graph_add_tone_map_pass(graph, resolved, camera, bloom, &output) != RASM_OK)
        return 0;

    resources[0] = use(RASM_PASS_ACCESS_READ_RENDER_TARGET, app->ldr.id);
    resources[1] = use(RASM_PASS_ACCESS_WRITE_BUFFER, app->bins.id);
    if (addPass(graph, "Histogram", RASM_PASS_TYPE_COMPUTE, resources, 2, recordHistogram, app) != RASM_OK)
        return 0;
    resources[0] = use(RASM_PASS_ACCESS_READ_BUFFER, app->bins.id);
    resources[1] = use(RASM_PASS_ACCESS_READ_PREVIOUS_RENDER_TARGET, app->adapt.id);
    resources[2] = use(RASM_PASS_ACCESS_WRITE_RENDER_TARGET, app->adapt.id);
    if (addPass(graph, "Adapt", RASM_PASS_TYPE_GRAPHICS, resources, 3, recordAdapt, app) != RASM_OK)
        return 0;
    resources[0] = use(RASM_PASS_ACCESS_READ_RENDER_TARGET, app->ldr.id);
    resources[1] = use(RASM_PASS_ACCESS_READ_RENDER_TARGET, app->adapt.id);
    resources[2] = use(RASM_PASS_ACCESS_READ_BUFFER, app->bins.id);
    resources[3] = use(RASM_PASS_ACCESS_WRITE_SWAPCHAIN, 0);
    if (addPass(graph, "Grade", RASM_PASS_TYPE_GRAPHICS, resources, 4, recordGrade, app) != RASM_OK)
        return 0;
    rasm_render_graph_add_ui_pass(graph, NULL, 0);
    return rasm_render_graph_compile(graph) == RASM_OK;
}

/* The bins, which the CPU reads at the end, and the Grade pass's settings. */
static int createBuffers(App *app)
{
    rasm_buffer_desc bufferDesc;
    GradeParams params;

    rasm_buffer_desc_init(&bufferDesc, sizeof(bufferDesc));
    bufferDesc.name = "histogram_bins";
    bufferDesc.byte_size = BINS * sizeof(uint32_t);
    bufferDesc.host_readable = RASM_TRUE;
    app->bins = rasm_create_buffer(app->renderer, &bufferDesc);
    bufferDesc.name = "grade_params";
    bufferDesc.byte_size = sizeof(GradeParams);
    bufferDesc.host_readable = RASM_FALSE;
    app->params = rasm_create_buffer(app->renderer, &bufferDesc);
    if (app->bins.id == 0 || app->params.id == 0)
        return 0;
    app->binsAddress = rasm_get_buffer_device_address(app->renderer, app->bins);
    app->paramsAddress = rasm_get_buffer_device_address(app->renderer, app->params);

    /* Written once, before any frame reads it: a device-local buffer gets it with the first frame's uploads. */
    memset(&params, 0, sizeof(params));
    params.bins = app->binsAddress;
    params.vignette = 0.55f;
    params.radius = 0.45f;
    params.targetBrightness = 0.45f;
    params.exposureLimit = 1.25f;
    params.tint[0] = 1.04f;
    params.tint[1] = 1.0f;
    params.tint[2] = 0.94f;
    params.panelGray = PANEL_GRAY;
    params.panelX = PANEL_X;
    params.panelWidth = PANEL_WIDTH;
    params.panelHeight = PANEL_HEIGHT;
    return rasm_write_buffer(app->renderer, app->params, &params, sizeof(params), 0) == RASM_OK;
}

/* The Grade pipeline draws into the window's format, the Adapt one into the history target's. */
static int createPipelines(App *app)
{
    rasm_pipeline_desc desc;
    app->histogram = createPipeline(app->renderer, "histogram", RASM_PIPELINE_TYPE_COMPUTE, HISTOGRAM_SHADER);
    app->grade = createPipeline(app->renderer, "grade", RASM_PIPELINE_TYPE_GRAPHICS, GRADE_SHADER);
    rasm_pipeline_desc_init(&desc, sizeof(desc));
    desc.name = "adapt";
    desc.source = ADAPT_SHADER;
    desc.target = app->adapt;
    app->adaptPipeline = rasm_create_pipeline(app->renderer, &desc);
    return app->histogram.id != 0 && app->grade.id != 0 && app->adaptPipeline.id != 0;
}

/* The sRGB byte of a linear value, as the window's sRGB format stores it. */
static int toSrgbByte(float linear)
{
    const float encoded = linear <= 0.0031308f ? 12.92f * linear : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
    return (int)(encoded * 255.0f + 0.5f);
}

/* The headless run's checks of the last frame. */
static int checkFrame(const App *app, const rasm_image *image)
{
    const rasm_image_data data = rasm_image_get(image);
    const uint8_t *pixel;
    uint32_t bins[BINS], i, timers, found = 0;
    uint64_t sum = 0;
    int ok = 1;
    if (rasm_read_buffer(app->renderer, app->bins, bins, sizeof(bins), 0) != RASM_OK)
        return fail("rasm_read_buffer");
    for (i = 0; i < BINS; ++i)
        sum += bins[i];
    printf("Histogram: %llu pixels counted, of %u x %u.\n", (unsigned long long)sum, data.width, data.height);
    if (sum != (uint64_t)data.width * data.height)
    {
        fprintf(stderr, "error: the histogram counted %llu pixels, not %u.\n", (unsigned long long)sum, data.width * data.height);
        ok = 0;
    }
    /* The panel's top-left corner is above every bar: the bars reach 90 % of its height. */
    pixel = (const uint8_t *)data.pixels + ((size_t)(data.height - 16 - PANEL_HEIGHT + 2) * data.width + PANEL_X + 2) * 4;
    if (abs(pixel[0] - toSrgbByte(PANEL_GRAY)) > 2 || abs(pixel[1] - toSrgbByte(PANEL_GRAY)) > 2)
    {
        fprintf(stderr, "error: the histogram panel is (%u, %u, %u), not gray %d.\n", pixel[0], pixel[1], pixel[2], toSrgbByte(PANEL_GRAY));
        ok = 0;
    }
    if (app->historyErrors > 0 || app->commandErrors > 0 || app->adaptFrames == 0)
    {
        fprintf(stderr, "error: %u Adapt frames read a wrong history copy, %u commands failed, %u Adapt frames ran.\n", app->historyErrors, app->commandErrors, app->adaptFrames);
        ok = 0;
    }
    timers = rasm_get_gpu_timer_count(app->renderer);
    for (i = 0; i < timers; ++i)
    {
        char name[64];
        rasm_get_gpu_timer_name(app->renderer, i, name, sizeof(name));
        printf("  GPU %-24s %7.3f ms\n", name, rasm_get_gpu_timer_ms(app->renderer, i));
        if (strcmp(name, "Histogram") == 0 || strcmp(name, "Adapt") == 0 || strcmp(name, "Grade") == 0 || strcmp(name, "Histogram clear") == 0)
            ++found;
    }
    if (found != 4)
    {
        fprintf(stderr, "error: %u of the 4 timers of the C passes were measured.\n", found);
        ok = 0;
    }
    return ok;
}

int main(int argc, char **argv)
{
    const ExampleArgs args = parseArgs(argc, argv);
    rasm_renderer_config rendererConfig;
    rasm_asset_loader_config loaderConfig;
    rasm_engine_config engineConfig;
    rasm_engine *engine = NULL;
    rasm_renderer *renderer;
    rasm_asset_loader *loader;
    rasm_render_graph *graph = NULL;
    rasm_scene scene;
    rasm_mesh helmet;
    rasm_entity helmetEntity;
    rasm_environment sunset;
    rasm_scene_environment environment;
    rasm_light_desc lightDesc;
    rasm_light sun;
    rasm_camera_projection projection;
    rasm_camera camera;
    rasm_readback readback = {0};
    rasm_image *image = NULL;
    App app;
    int written = 0, checked = 1;
    unsigned frame;

    memset(&app, 0, sizeof(app));
    rasm_renderer_config_init(&rendererConfig, sizeof(rendererConfig));
    rendererConfig.app_name = "example_33";
    rendererConfig.window_width = WIDTH;
    rendererConfig.window_height = HEIGHT;
    rendererConfig.enable_validation = RASM_TRUE;
    rendererConfig.shader_dir = RASM_EXAMPLE_SHADER_DIR;
    if (args.fixedStep)
        rendererConfig.fixed_delta_time = 1.0f / 60.0f;
    if (args.noCache)
        rendererConfig.cache_dir = NULL;
    rasm_asset_loader_config_init(&loaderConfig, sizeof(loaderConfig));
    loaderConfig.asset_root = RASM_EXAMPLE_ASSET_ROOT;
    rasm_engine_config_init(&engineConfig, sizeof(engineConfig));
    engineConfig.platform = args.headless ? RASM_PLATFORM_TYPE_HEADLESS : RASM_PLATFORM_TYPE_SDL;
    engineConfig.renderer = &rendererConfig;
    engineConfig.loader = &loaderConfig;
    if (rasm_engine_create(RASM_C_API_VERSION, &engineConfig, &engine) != RASM_OK)
        return fail("rasm_engine_create");
    renderer = rasm_engine_get_renderer(engine);
    loader = rasm_engine_get_asset_loader(engine);
    app.renderer = renderer;

    /* The scene: the helmet on a checker ground under a low sun, lit by a sunset. */
    scene = rasm_create_scene(renderer);
    sunset = rasm_asset_loader_load_environment(loader, "environments/venice_sunset_2k.hdr");
    helmet = rasm_asset_loader_load_gltf(loader, "models/gltf/DamagedHelmet/DamagedHelmet.gltf");
    rasm_scene_environment_init(&environment, sizeof(environment));
    environment.environment = sunset;
    environment.rotation = 1.15f;
    rasm_set_scene_environment(renderer, scene, &environment);
    {
        const rasm_transform helmetAt = placed(vec3(0.0f, 0.0f, 0.0f), -0.44f, 1.0f);
        const rasm_transform groundAt = placed(vec3(0.0f, -1.0f, 0.0f), 0.0f, 5.0f);
        helmetEntity = rasm_create_entity(renderer, "helmet", helmet, (rasm_material){0}, &helmetAt);
        rasm_add_entity_to_scene(renderer, scene, helmetEntity);
        rasm_add_entity_to_scene(renderer, scene, rasm_create_entity(renderer, "ground", createGround(renderer), createGroundMaterial(renderer), &groundAt));
    }
    rasm_light_desc_init(&lightDesc, sizeof(lightDesc));
    lightDesc.type = RASM_LIGHT_TYPE_DIRECTIONAL;
    lightDesc.color = vec3(1.0f, 0.85f, 0.7f);
    lightDesc.direction = normalize(vec3(0.6f, -1.0f, -0.35f));
    lightDesc.intensity = 3.0f;
    lightDesc.range = 0.0f;
    sun = rasm_create_light(renderer, &lightDesc);
    rasm_add_light_to_scene(renderer, scene, sun);

    rasm_camera_projection_init(&projection, sizeof(projection));
    projection.type = RASM_PROJECTION_TYPE_PERSPECTIVE;
    projection.fov_y = 40.0f;
    projection.aspect = (float)WIDTH / (float)HEIGHT;
    projection.near_z = 0.1f;
    {
        const rasm_transform eye = lookAtOrigin(vec3(1.9f, 0.9f, 3.6f));
        camera = rasm_create_camera(renderer, &projection, &eye);
    }
    rasm_add_camera_to_scene(renderer, scene, camera);
    if (sunset.id == 0 || helmet.id == 0 || sun.id == 0 || camera.id == 0)
    {
        fail("creating the scene");
        rasm_engine_destroy(engine);
        return 1;
    }

    /* The buffers, the graph with its targets and passes, and the pipelines, which take the targets' formats. */
    if (!createBuffers(&app) || rasm_render_graph_create(renderer, &graph) != RASM_OK || !buildGraph(graph, &app, scene, camera, sun) || !createPipelines(&app))
    {
        fail("building the graph");
        rasm_render_graph_destroy(graph);
        rasm_engine_destroy(engine);
        return 1;
    }
    if (args.dump)
    {
        static char dump[16384];
        rasm_render_graph_dump(graph, dump, sizeof(dump));
        printf("%s\n", dump);
    }

    if (args.waitForLoads)
        rasm_wait_for_loads(renderer);
    rasm_get_gpu_timer_count(renderer); /* turns the GPU timers on */
    image = rasm_image_create();
    for (frame = 1; rasm_running(renderer); ++frame)
    {
        const rasm_transform helmetAt = placed(vec3(0.0f, 0.0f, 0.0f), -0.44f + 0.01f * (float)frame, 1.0f);
        rasm_update_transform(renderer, helmetEntity, &helmetAt);
        if (args.headless && frame == args.frames)
            readback = rasm_read_frame(renderer, RASM_READBACK_FORMAT_RGBA8_SRGB);
        if (rasm_render_graph_execute(graph, camera) != RASM_OK)
            fail("rasm_render_graph_execute");
        if (args.headless && frame == args.frames)
        {
            written = rasm_take_readback(renderer, readback, image) == RASM_OK && rasm_image_write_png(image, args.out) == RASM_OK;
            if (written)
            {
                const rasm_image_data data = rasm_image_get(image);
                printf("Wrote frame %u to %s (%u x %u).\n", args.frames, args.out, data.width, data.height);
                checked = checkFrame(&app, image);
            }
            break;
        }
    }

    /* The graph before the engine, which would otherwise detach it. */
    rasm_render_graph_destroy(graph);
    rasm_image_destroy(image);
    rasm_engine_destroy(engine);
    if (args.headless && !written)
    {
        fprintf(stderr, "error: frame %u was not written to %s: %s\n", args.frames, args.out, rasm_get_last_error());
        return 1;
    }
    return checked ? 0 : 1;
}
