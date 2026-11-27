#include "rasm/rasm_c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * RASM from C, through the C API (rasm_c.h) and the rasm_c shared library. The program is compiled as C99.
 *
 * It makes an engine (an SDL window, or offscreen images with --headless) and builds a scene from both kinds of
 * source: the DamagedHelmet from a glTF file and a sunset environment from an HDR file, both loaded by the engine's
 * asset loader, and a ground square with a checker texture, both made here in memory. A sun and a warm point light
 * light it.
 *
 * The ground is streamed (1.3): a tile streamer holds it as a root tile and its four quarters, and each frame the
 * program passes the quarters as the tiles to draw while the root's error on screen is large, the root otherwise. The
 * streamer hands out each tile once, and the program gives it its mesh and entity then; the quarters draw the same
 * square as the root, so the picture does not change, only who draws it.
 *
 * Three views draw each frame: a top-down map with a reference grid (1.8) into a view texture (a render target), which
 * a board in the scene shows; the main camera into the window; and the map again into a corner of the window. Each
 * view draws the shadows of its list (1.18). The main camera has a trackball
 * controller, which a script drives with synthetic mouse events (rasm_add_input_event), the way an application with its
 * own event loop feeds RASM; with --fixed-step, which fixes the easing's time step, frame N is the same on every run. At frame 20 it picks the main view's centre (1.7) and prints what is there, a
 * couple of frames later; a headless run whose pick hit nothing exits with 1. At the end it prints the frame and pool stats.
 *
 * Flags, as the C++ examples take them: --headless, --frames N (60), --out <file.png> (frame.png), --fixed-step,
 * --wait-for-loads, --no-cache, --transfer-queue on|off, --no-shadows, and --bench (no validation and no UI; prints the
 * average CPU and GPU frame times after 120 frames). A headless run copies frame N back into a rasm_image, writes it as
 * a PNG and exits, with code 1 when the frame was not written.
 */

#define WIDTH 1280
#define HEIGHT 720
#define CHECKER_SIZE 256
#define MAP_SIZE 256
#define MAP_HALF_EXTENT 4.0f
#define GROUND_HALF_SIZE 30.0f
#define GROUND_TILES 5
#define REFINE_PIXELS 16.0

#define BENCH_WARMUP 120u

typedef struct ExampleArgs
{
    int headless;
    int fixedStep;
    int waitForLoads;
    int noCache;
    int bench;
    int transferQueue; /* 1 on, 0 off, -1 the default */
    int shadows;
    unsigned frames;
    const char *out;
} ExampleArgs;

static ExampleArgs parseArgs(int argc, char **argv)
{
    ExampleArgs args = {0, 0, 0, 0, 0, -1, 1, 60, "frame.png"};
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
        else if (strcmp(argv[i], "--bench") == 0)
            args.bench = 1;
        else if (strcmp(argv[i], "--no-shadows") == 0)
            args.shadows = 0;
        else if (i + 1 < argc && strcmp(argv[i], "--frames") == 0)
        {
            const int frames = atoi(argv[++i]);
            args.frames = frames > 0 ? (unsigned)frames : 1u;
        }
        else if (i + 1 < argc && strcmp(argv[i], "--out") == 0)
            args.out = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--transfer-queue") == 0)
            args.transferQueue = strcmp(argv[++i], "on") == 0 ? 1 : 0;
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

static rasm_vec3 cross(rasm_vec3 a, rasm_vec3 b)
{
    return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

/* The rotation that turns the camera's -Z toward target, with +Y up: the rows of its matrix as a quaternion. */
static rasm_transform lookAt(rasm_vec3 eye, rasm_vec3 target)
{
    const rasm_vec3 forward = normalize(vec3(target.x - eye.x, target.y - eye.y, target.z - eye.z));
    const rasm_vec3 right = normalize(cross(forward, vec3(0.0f, 1.0f, 0.0f)));
    const rasm_vec3 up = cross(right, forward);
    const float m00 = right.x, m01 = up.x, m02 = -forward.x;
    const float m10 = right.y, m11 = up.y, m12 = -forward.y;
    const float m20 = right.z, m21 = up.z, m22 = -forward.z;
    const float trace = m00 + m11 + m22;
    rasm_transform transform;
    float s;
    if (trace > 0.0f)
    {
        s = 0.5f / sqrtf(trace + 1.0f);
        transform.rotation.w = 0.25f / s;
        transform.rotation.x = (m21 - m12) * s;
        transform.rotation.y = (m02 - m20) * s;
        transform.rotation.z = (m10 - m01) * s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        s = 2.0f * sqrtf(1.0f + m00 - m11 - m22);
        transform.rotation.w = (m21 - m12) / s;
        transform.rotation.x = 0.25f * s;
        transform.rotation.y = (m01 + m10) / s;
        transform.rotation.z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        s = 2.0f * sqrtf(1.0f + m11 - m00 - m22);
        transform.rotation.w = (m02 - m20) / s;
        transform.rotation.x = (m01 + m10) / s;
        transform.rotation.y = 0.25f * s;
        transform.rotation.z = (m12 + m21) / s;
    }
    else
    {
        s = 2.0f * sqrtf(1.0f + m22 - m00 - m11);
        transform.rotation.w = (m10 - m01) / s;
        transform.rotation.x = (m02 + m20) / s;
        transform.rotation.y = (m12 + m21) / s;
        transform.rotation.z = 0.25f * s;
    }
    transform.position = eye;
    transform.scale = vec3(1.0f, 1.0f, 1.0f);
    return transform;
}

static rasm_transform placed(rasm_vec3 position, float yaw)
{
    rasm_transform transform;
    transform.position = position;
    transform.rotation.x = 0.0f;
    transform.rotation.y = sinf(0.5f * yaw);
    transform.rotation.z = 0.0f;
    transform.rotation.w = cosf(0.5f * yaw);
    transform.scale = vec3(1.0f, 1.0f, 1.0f);
    return transform;
}

/* A flat square of the ground on y = 0, facing up: the part of half size halfSize around (centerX, centerZ). The UVs
 * come from the position, so the whole ground repeats the checker 8 times however it is cut in parts. */
static rasm_mesh createGround(rasm_renderer *renderer, float centerX, float centerZ, float halfSize)
{
    static const float corners[4][2] = {{-1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, -1.0f}};
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    rasm_vertex vertices[4];
    int i, set;
    memset(vertices, 0, sizeof(vertices));
    for (i = 0; i < 4; ++i)
    {
        const float x = centerX + corners[i][0] * halfSize;
        const float z = centerZ + corners[i][1] * halfSize;
        vertices[i].position = vec3(x, 0.0f, z);
        vertices[i].normal = vec3(0.0f, 1.0f, 0.0f);
        for (set = 0; set < RASM_MAX_VERTEX_UV_SETS; ++set)
        {
            vertices[i].uv[set].x = (x / GROUND_HALF_SIZE * 0.5f + 0.5f) * 8.0f;
            vertices[i].uv[set].y = (z / GROUND_HALF_SIZE * 0.5f + 0.5f) * 8.0f;
        }
    }
    return rasm_create_mesh(renderer, vertices, 4, indices, 6, NULL);
}

/* The ground's tiles: 0 is the whole square, 1 to 4 its quarters. */
static void groundTileBounds(int tile, float *centerX, float *centerZ, float *halfSize)
{
    *halfSize = tile == 0 ? GROUND_HALF_SIZE : 0.5f * GROUND_HALF_SIZE;
    *centerX = tile == 0 ? 0.0f : ((tile - 1) % 2 == 0 ? -0.5f : 0.5f) * GROUND_HALF_SIZE;
    *centerZ = tile == 0 ? 0.0f : ((tile - 1) / 2 == 0 ? -0.5f : 0.5f) * GROUND_HALF_SIZE;
}

/* The tiles the last update handed out get their mesh and their entity, which the streamer owns from then on. */
static void loadGroundTiles(rasm_renderer *renderer, rasm_tile_streamer *tiles, const rasm_tile *groundTiles, rasm_material material)
{
    uint32_t count = 0, i;
    const rasm_tile *toLoad = rasm_tile_streamer_get_tiles_to_load(tiles, &count);
    for (i = 0; i < count; ++i)
    {
        int tile;
        for (tile = 0; tile < GROUND_TILES; ++tile)
        {
            if (groundTiles[tile].id == toLoad[i].id)
            {
                const rasm_transform at = placed(vec3(0.0f, -1.0f, 0.0f), 0.0f);
                float centerX, centerZ, halfSize;
                rasm_mesh mesh;
                rasm_entity entity;
                rasm_tile_content content;
                groundTileBounds(tile, &centerX, &centerZ, &halfSize);
                mesh = createGround(renderer, centerX, centerZ, halfSize);
                entity = rasm_create_entity(renderer, "ground", mesh, material, &at);
                rasm_tile_content_init(&content, sizeof(content));
                content.entities = &entity;
                content.entity_count = 1;
                content.meshes = &mesh;
                content.mesh_count = 1;
                rasm_tile_streamer_set_tile_content(tiles, groundTiles[tile], &content);
            }
        }
    }
}

/* Light and dark gray squares, 32 texels each: sRGB color, so the mips are made in linear light. */
static rasm_texture createChecker(rasm_renderer *renderer)
{
    static uint8_t pixels[CHECKER_SIZE * CHECKER_SIZE * 4];
    rasm_texture_desc desc;
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
    return rasm_create_texture(renderer, &desc, pixels);
}

static rasm_material createGroundMaterial(rasm_renderer *renderer, rasm_texture checker)
{
    const rasm_material material = rasm_create_material(renderer, RASM_MATERIAL_TYPE_PBR, &checker, 1, RASM_FALSE);
    rasm_material_desc desc;
    desc.size = sizeof(desc);
    if (rasm_get_material(renderer, material, &desc) != RASM_OK)
        return material;
    desc.params[RASM_PBR_PARAM_METALLIC] = 0.0f;
    desc.params[RASM_PBR_PARAM_ROUGHNESS] = 0.85f;
    rasm_update_material(renderer, material, &desc);
    return material;
}

/* A 1 x 1 square in the XY plane, facing +Z, for the board that shows the map. */
static rasm_mesh createBoard(rasm_renderer *renderer)
{
    static const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    rasm_vertex vertices[4];
    int i, set;
    memset(vertices, 0, sizeof(vertices));
    for (i = 0; i < 4; ++i)
    {
        vertices[i].position = vec3(corners[i][0], corners[i][1], 0.0f);
        vertices[i].normal = vec3(0.0f, 0.0f, 1.0f);
        for (set = 0; set < RASM_MAX_VERTEX_UV_SETS; ++set)
        {
            vertices[i].uv[set].x = corners[i][0] + 0.5f;
            vertices[i].uv[set].y = 0.5f - corners[i][1];
        }
    }
    return rasm_create_mesh(renderer, vertices, 4, indices, 6, NULL);
}

/* One mouse event in the main window, as an application's event loop would send it. */
static void sendMouse(rasm_renderer *renderer, rasm_input_event_type type, float x, float y, int down)
{
    rasm_input_event event;
    rasm_input_event_init(&event, sizeof(event));
    event.type = type;
    event.button = RASM_MOUSE_BUTTON_LEFT;
    event.down = down ? RASM_TRUE : RASM_FALSE;
    event.x = x;
    event.y = y;
    rasm_add_input_event(renderer, &event);
}

/* The script: press in the middle on frame 1, drag right, let go 5 frames before the last (frame 2 for a short run). */
static void sendScriptedOrbit(rasm_renderer *renderer, unsigned frame, unsigned frames)
{
    const unsigned release = frames > 7 ? frames - 5 : 2;
    const float x = 0.5f * WIDTH + 5.0f * (float)(frame - 1);
    const float y = 0.5f * HEIGHT;
    if (frame == 1)
    {
        sendMouse(renderer, RASM_INPUT_EVENT_TYPE_MOUSE_MOVE, x, y, 0);
        sendMouse(renderer, RASM_INPUT_EVENT_TYPE_MOUSE_BUTTON, x, y, 1);
    }
    else if (frame < release)
        sendMouse(renderer, RASM_INPUT_EVENT_TYPE_MOUSE_MOVE, x, y, 0);
    else if (frame == release)
        sendMouse(renderer, RASM_INPUT_EVENT_TYPE_MOUSE_BUTTON, x, y, 0);
}

static int fail(const char *what)
{
    fprintf(stderr, "error: %s: %s\n", what, rasm_get_last_error());
    return 1;
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
    rasm_scene scene;
    rasm_mesh helmet;
    rasm_texture checker;
    rasm_material groundMaterial;
    rasm_tile_streamer *tiles = NULL;
    rasm_tile groundTiles[GROUND_TILES];
    rasm_tile_streamer_stats tileStats;
    rasm_environment sunset;
    rasm_scene_environment environment;
    rasm_light_desc light;
    rasm_camera_projection projection;
    rasm_camera camera, map;
    rasm_camera_controller controller;
    rasm_render_target_desc mapDesc;
    rasm_render_target mapTarget;
    rasm_view views[3];
    rasm_shadow_desc mainShadows[2], mapShadow;
    rasm_frame_stats frameStats;
    rasm_pool_stats poolStats;
    rasm_readback readback = {0};
    rasm_pick pick = {0};
    int picked = 1;
    rasm_image *image;
    int written = 0;
    unsigned frame;
    unsigned benchFrames = 0;
    double benchCpuMs = 0.0, benchGpuMs = 0.0;

    printf("RASM C API %u.%u (header %u.%u)\n", rasm_get_version() >> 16, rasm_get_version() & 0xFFFFu, RASM_C_API_VERSION_MAJOR, RASM_C_API_VERSION_MINOR);

    /* 1. The engine: the renderer's and the loader's settings, and the platform. */
    rasm_renderer_config_init(&rendererConfig, sizeof(rendererConfig));
    rendererConfig.app_name = "example_22";
    rendererConfig.window_width = WIDTH;
    rendererConfig.window_height = HEIGHT;
    rendererConfig.enable_validation = args.bench ? RASM_FALSE : RASM_TRUE;
    /* --bench: no validation and no UI, which would change the timings. */
    if (args.bench)
        rendererConfig.ui = RASM_FALSE;
    rendererConfig.shader_dir = RASM_EXAMPLE_SHADER_DIR;
    if (args.fixedStep)
        rendererConfig.fixed_delta_time = 1.0f / 60.0f;
    if (args.noCache)
        rendererConfig.cache_dir = NULL;
    if (args.transferQueue >= 0)
        rendererConfig.transfer_queue = args.transferQueue == 1 ? RASM_TRUE : RASM_FALSE;
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

    /* 2. What the scene is made of: two files, and a mesh and a texture from memory. */
    scene = rasm_create_scene(renderer);
    sunset = rasm_asset_loader_load_environment(loader, "environments/venice_sunset_2k.hdr");
    helmet = rasm_asset_loader_load_gltf(loader, "models/gltf/DamagedHelmet/DamagedHelmet.gltf");
    checker = createChecker(renderer);
    groundMaterial = createGroundMaterial(renderer, checker);
    if (sunset.id == 0 || helmet.id == 0 || checker.id == 0 || groundMaterial.id == 0)
    {
        fail("creating the scene");
        rasm_engine_destroy(engine);
        return 1;
    }

    rasm_scene_environment_init(&environment, sizeof(environment));
    environment.environment = sunset;
    environment.rotation = 1.15f;
    rasm_set_scene_environment(renderer, scene, &environment);

    {
        const rasm_transform helmetAt = placed(vec3(0.0f, 0.0f, 0.0f), -0.44f);
        /* No material: the glTF's own materials draw it. */
        rasm_add_entity_to_scene(renderer, scene, rasm_create_entity(renderer, "helmet", helmet, (rasm_material){0}, &helmetAt));
    }

    /* The ground's tiles. The material is shared, so no tile lists it: the streamer destroys only what a tile owns. */
    if (rasm_tile_streamer_create(renderer, scene, NULL, &tiles) != RASM_OK)
    {
        fail("rasm_tile_streamer_create");
        rasm_engine_destroy(engine);
        return 1;
    }
    {
        int tile;
        for (tile = 0; tile < GROUND_TILES; ++tile)
        {
            rasm_tile_desc desc;
            float centerX, centerZ, halfSize;
            groundTileBounds(tile, &centerX, &centerZ, &halfSize);
            rasm_tile_desc_init(&desc, sizeof(desc));
            desc.parent = tile == 0 ? (rasm_tile){0} : groundTiles[0];
            desc.center.x = centerX;
            desc.center.y = -1.0;
            desc.center.z = centerZ;
            desc.radius = 1.415 * halfSize;
            desc.geometric_error = tile == 0 ? 4.0 : 0.0;
            groundTiles[tile] = rasm_tile_streamer_create_tile(tiles, &desc);
        }
    }

    /* 3. A sun from high on the left, and a warm point light below the helmet's visor. */
    rasm_light_desc_init(&light, sizeof(light));
    light.type = RASM_LIGHT_TYPE_DIRECTIONAL;
    light.color = vec3(1.0f, 0.85f, 0.7f);
    light.direction = normalize(vec3(0.6f, -1.0f, -0.35f));
    light.intensity = 3.0f;
    light.range = 0.0f;
    rasm_shadow_desc_init(&mainShadows[0], sizeof(mainShadows[0]));
    mainShadows[0].light = rasm_create_light(renderer, &light);
    mainShadows[0].distance = 12.0f;
    rasm_add_light_to_scene(renderer, scene, mainShadows[0].light);
    light.type = RASM_LIGHT_TYPE_POINT;
    light.color = vec3(1.0f, 0.6f, 0.3f);
    light.position = vec3(0.8f, -0.4f, 1.4f);
    light.intensity = 6.0f;
    light.range = 6.0f;
    rasm_shadow_desc_init(&mainShadows[1], sizeof(mainShadows[1]));
    mainShadows[1].light = rasm_create_light(renderer, &light);
    mainShadows[1].map_size = 1024;
    mainShadows[1].distance = 6.0f;
    rasm_add_light_to_scene(renderer, scene, mainShadows[1].light);
    /* The map's sun shadow: one cascade over the whole ground. */
    mapShadow = mainShadows[0];
    mapShadow.cascade_count = 1;
    mapShadow.map_size = 1024;

    /* 4. The main camera, with a trackball around the helmet: the script below turns it. */
    rasm_camera_projection_init(&projection, sizeof(projection));
    projection.type = RASM_PROJECTION_TYPE_PERSPECTIVE;
    projection.fov_y = 40.0f;
    projection.aspect = (float)WIDTH / (float)HEIGHT;
    projection.near_z = 0.1f;
    {
        const rasm_transform eye = lookAt(vec3(1.8f, 0.4f, 3.1f), vec3(0.0f, 0.0f, 0.0f));
        camera = rasm_create_camera(renderer, &projection, &eye);
    }
    rasm_camera_controller_init(&controller, sizeof(controller));
    controller.type = RASM_CAMERA_CONTROLLER_TYPE_TRACK_BALL;
    controller.track_ball_radius = sqrtf(1.8f * 1.8f + 0.4f * 0.4f + 3.1f * 3.1f);
    controller.track_ball_center = vec3(0.0f, 0.0f, 0.0f);
    rasm_set_camera_controller(renderer, camera, &controller);
    rasm_add_camera_to_scene(renderer, scene, camera);

    /* 5. A top-down map camera, drawn into a view texture that a board behind the helmet shows. */
    rasm_camera_projection_init(&projection, sizeof(projection));
    projection.type = RASM_PROJECTION_TYPE_ORTHOGRAPHIC;
    projection.left = -MAP_HALF_EXTENT;
    projection.right = MAP_HALF_EXTENT;
    projection.bottom = -MAP_HALF_EXTENT;
    projection.top = MAP_HALF_EXTENT;
    projection.near_z = 0.1f;
    projection.far_z = 30.0f;
    {
        /* -90 degrees around X: it looks straight down. */
        rasm_transform above = placed(vec3(0.0f, 10.0f, 0.0f), 0.0f);
        above.rotation.x = -0.70710678f;
        above.rotation.w = 0.70710678f;
        map = rasm_create_camera(renderer, &projection, &above);
    }
    rasm_add_camera_to_scene(renderer, scene, map);
    {
        /* The map's reference grid (1.8), just over the ground: orange half-metre cells, every second line major. */
        rasm_grid grid;
        rasm_grid_init(&grid, sizeof(grid));
        grid.enabled = RASM_TRUE;
        grid.origin.y = 0.01;
        grid.cell_size = 0.5;
        grid.subdivisions = 2;
        grid.minor_color = (rasm_vec4){1.0f, 0.45f, 0.05f, 0.5f};
        grid.major_color = (rasm_vec4){1.0f, 0.45f, 0.05f, 0.9f};
        rasm_set_camera_grid(renderer, map, &grid);
    }
    rasm_render_target_desc_init(&mapDesc, sizeof(mapDesc));
    mapDesc.name = "map";
    mapDesc.width = MAP_SIZE;
    mapDesc.height = MAP_SIZE;
    mapTarget = rasm_create_render_target(renderer, &mapDesc);
    if (map.id == 0 || mapTarget.id == 0)
    {
        fail("the map");
        rasm_tile_streamer_destroy(tiles);
        rasm_engine_destroy(engine);
        return 1;
    }
    {
        const rasm_texture mapTexture = rasm_get_render_target_texture(renderer, mapTarget);
        const rasm_material board = rasm_create_material(renderer, RASM_MATERIAL_TYPE_UNLIT, &mapTexture, 1, RASM_FALSE);
        rasm_transform at = placed(vec3(0.0f, 0.4f, -2.4f), 0.0f);
        at.scale = vec3(1.6f, 1.6f, 1.0f);
        rasm_add_entity_to_scene(renderer, scene, rasm_create_entity(renderer, "board", createBoard(renderer), board, &at));
    }

    /* The views, in drawing order: the map texture first, so the board shows this frame's map, then the window's. Each
     * draws its shadows fitted to its own camera (1.18): the main view the sun's in four cascades and the point light's
     * cube, the maps the sun's in one cascade (they share a camera, so each has its own maps). --no-shadows: none. */
    rasm_view_init(&views[0], sizeof(views[0]));
    views[0].scene = scene;
    views[0].camera = map;
    views[0].target = mapTarget;
    views[0].bloom = RASM_VIEW_TOGGLE_OFF;
    views[0].anti_aliasing = RASM_ANTI_ALIASING_NONE;
    views[0].shadows = &mapShadow;
    views[0].shadow_count = args.shadows ? 1 : 0;
    rasm_view_init(&views[1], sizeof(views[1]));
    views[1].scene = scene;
    views[1].camera = camera;
    views[1].shadows = mainShadows;
    views[1].shadow_count = args.shadows ? 2 : 0;
    views[2] = views[0];
    views[2].target.id = 0;
    views[2].rect.height = 0.32f;
    views[2].rect.width = 0.32f * (float)HEIGHT / (float)WIDTH;
    views[2].rect.x = 0.98f - views[2].rect.width;
    views[2].rect.y = 0.03f;

    if (args.waitForLoads)
        rasm_wait_for_loads(renderer);

    /* The first call turns the frame stats on; they fill in a few frames later. */
    frameStats.size = sizeof(frameStats);
    rasm_get_frame_stats(renderer, &frameStats);

    /* 6. The frame loop. Headless, frame N's copy is asked for before it starts and taken once it is drawn. At frame 20
     * it picks a 5 x 5 rect at the main view's centre (1.7), and prints what it hit a couple of frames later. */
    image = rasm_image_create();
    for (frame = 1; rasm_running(renderer); ++frame)
    {
        if (frame == 20)
        {
            rasm_pick_desc pickDesc;
            rasm_window_info window;
            window.size = sizeof(window);
            rasm_get_window(renderer, (rasm_window){0}, &window);
            rasm_pick_desc_init(&pickDesc, sizeof(pickDesc));
            pickDesc.rect.x = window.pixel_width / 2 - 2;
            pickDesc.rect.y = window.pixel_height / 2 - 2;
            pickDesc.rect.width = 5;
            pickDesc.rect.height = 5;
            pick = rasm_pick_view(renderer, &views[1], &pickDesc);
        }
        if (pick.id != 0 && rasm_is_pick_ready(renderer, pick))
        {
            rasm_pick_hit hit;
            uint32_t hitCount = 0;
            rasm_pick_hit_init(&hit, sizeof(hit));
            if (rasm_take_pick(renderer, pick, &hit, 1, &hitCount) == RASM_OK && hitCount > 0 && rasm_is_entity_alive(renderer, hit.entity))
                printf("Picked entity %llu, sub-mesh %u, at (%.3f, %.3f, %.3f), %.2f m away; %u things under the rect.\n", (unsigned long long)hit.entity.id, hit.sub_mesh, hit.position.x,
                       hit.position.y, hit.position.z, hit.distance, hitCount);
            else
                picked = 0;
            pick.id = 0;
        }
        sendScriptedOrbit(renderer, frame, args.frames);
        /* The cut: the quarters while the root's error on screen is large, else the root. */
        rasm_tile_streamer_set_views(tiles, views, 3);
        if (rasm_tile_streamer_get_tile_screen_error(tiles, groundTiles[0]) > REFINE_PIXELS)
            rasm_tile_streamer_update(tiles, groundTiles + 1, GROUND_TILES - 1);
        else
            rasm_tile_streamer_update(tiles, groundTiles, 1);
        loadGroundTiles(renderer, tiles, groundTiles, groundMaterial);
        if (args.headless && frame == args.frames)
            readback = rasm_read_frame(renderer, RASM_READBACK_FORMAT_RGBA8_SRGB);
        rasm_render_views(renderer, views, 3);
        /* --bench averages the frame times after BENCH_WARMUP frames. */
        if (args.bench && frame > BENCH_WARMUP && rasm_get_frame_stats(renderer, &frameStats) == RASM_OK)
        {
            benchCpuMs += frameStats.cpu_frame_ms;
            benchGpuMs += frameStats.gpu_frame_ms;
            ++benchFrames;
        }
        if (args.headless && frame == args.frames)
        {
            written = rasm_take_readback(renderer, readback, image) == RASM_OK && rasm_image_write_png(image, args.out) == RASM_OK;
            if (written)
            {
                const rasm_image_data data = rasm_image_get(image);
                printf("Wrote frame %u to %s (%u x %u).\n", args.frames, args.out, data.width, data.height);
            }
            break;
        }
    }

    if (benchFrames > 0)
        printf("bench: %u frames after %u: CPU %.3f ms, GPU %.3f ms a frame.\n", benchFrames, BENCH_WARMUP, benchCpuMs / benchFrames, benchGpuMs / benchFrames);
    poolStats.size = sizeof(poolStats);
    if (rasm_get_frame_stats(renderer, &frameStats) == RASM_OK && rasm_get_pool_stats(renderer, &poolStats) == RASM_OK)
        printf("%u entities, %u meshes, %u materials; %u draws, %u visible.\n", poolStats.entities, poolStats.meshes, poolStats.materials, frameStats.draws, frameStats.draws_visible);

    tileStats.size = sizeof(tileStats);
    if (rasm_tile_streamer_get_stats(tiles, &tileStats) == RASM_OK)
        printf("Ground tiles: %u drawn, %u ready, %llu loads, %.1f KiB of %.0f MiB.\n", tileStats.drawn, tileStats.ready, (unsigned long long)tileStats.total_loads,
               (double)tileStats.bytes / 1024.0, (double)tileStats.budget / (1024.0 * 1024.0));

    /* Before the engine: it destroys the tiles' content. */
    rasm_tile_streamer_destroy(tiles);
    rasm_image_destroy(image);
    rasm_engine_destroy(engine);
    if (args.headless && !written)
    {
            fprintf(stderr, "error: frame %u was not written to %s: %s\n", args.frames, args.out, rasm_get_last_error());
            return 1;
    }
    if (args.headless && !picked)
    {
            fprintf(stderr, "error: the pick at the view's centre hit nothing: %s\n", rasm_get_last_error());
            return 1;
    }
    return 0;
}
