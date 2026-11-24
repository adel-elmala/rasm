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
 * light it. The camera circles the helmet by frame, not by clock, so frame N is the same on every run.
 *
 * Flags, as the C++ examples take them: --headless, --frames N (60), --out <file.png> (frame.png), --fixed-step,
 * --wait-for-loads, --no-cache, --transfer-queue on|off. A headless run copies frame N back into a rasm_image, writes
 * it as a PNG and exits, with code 1 when the frame was not written.
 */

#define WIDTH 1280
#define HEIGHT 720
#define CHECKER_SIZE 256

typedef struct ExampleArgs
{
    int headless;
    int fixedStep;
    int waitForLoads;
    int noCache;
    int transferQueue; /* 1 on, 0 off, -1 the default */
    unsigned frames;
    const char *out;
} ExampleArgs;

static ExampleArgs parseArgs(int argc, char **argv)
{
    ExampleArgs args = {0, 0, 0, 0, -1, 60, "frame.png"};
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

/* A flat square on y = 0, facing up, its UVs repeating the checker 8 times. */
static rasm_mesh createGround(rasm_renderer *renderer, float halfSize)
{
    static const float corners[4][2] = {{-1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, -1.0f}};
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    rasm_vertex vertices[4];
    int i, set;
    memset(vertices, 0, sizeof(vertices));
    for (i = 0; i < 4; ++i)
    {
        vertices[i].position = vec3(corners[i][0] * halfSize, 0.0f, corners[i][1] * halfSize);
        vertices[i].normal = vec3(0.0f, 1.0f, 0.0f);
        for (set = 0; set < RASM_MAX_VERTEX_UV_SETS; ++set)
        {
            vertices[i].uv[set].x = (corners[i][0] * 0.5f + 0.5f) * 8.0f;
            vertices[i].uv[set].y = (corners[i][1] * 0.5f + 0.5f) * 8.0f;
        }
    }
    return rasm_create_mesh(renderer, vertices, 4, indices, 6, NULL);
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
    rasm_mesh helmet, ground;
    rasm_texture checker;
    rasm_environment sunset;
    rasm_scene_environment environment;
    rasm_light_desc light;
    rasm_camera_projection projection;
    rasm_camera camera;
    rasm_readback readback = {0};
    rasm_image *image;
    int written = 0;
    unsigned frame;

    printf("RASM C API %u.%u (header %u.%u)\n", rasm_get_version() >> 16, rasm_get_version() & 0xFFFFu, RASM_C_API_VERSION_MAJOR, RASM_C_API_VERSION_MINOR);

    /* 1. The engine: the renderer's and the loader's settings, and the platform. */
    rasm_renderer_config_init(&rendererConfig, sizeof(rendererConfig));
    rendererConfig.app_name = "example_22";
    rendererConfig.window_width = WIDTH;
    rendererConfig.window_height = HEIGHT;
    rendererConfig.enable_validation = RASM_TRUE;
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
    ground = createGround(renderer, 30.0f);
    checker = createChecker(renderer);
    if (sunset.id == 0 || helmet.id == 0 || ground.id == 0 || checker.id == 0)
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
        const rasm_transform groundAt = placed(vec3(0.0f, -1.0f, 0.0f), 0.0f);
        const rasm_transform helmetAt = placed(vec3(0.0f, 0.0f, 0.0f), -0.44f);
        rasm_add_entity_to_scene(renderer, scene, rasm_create_entity(renderer, "ground", ground, createGroundMaterial(renderer, checker), &groundAt));
        /* No material: the glTF's own materials draw it. */
        rasm_add_entity_to_scene(renderer, scene, rasm_create_entity(renderer, "helmet", helmet, (rasm_material){0}, &helmetAt));
    }

    /* 3. A sun from high on the left, and a warm point light below the helmet's visor. */
    rasm_light_desc_init(&light, sizeof(light));
    light.type = RASM_LIGHT_TYPE_DIRECTIONAL;
    light.color = vec3(1.0f, 0.85f, 0.7f);
    light.direction = normalize(vec3(0.6f, -1.0f, -0.35f));
    light.intensity = 3.0f;
    light.range = 0.0f;
    rasm_add_light_to_scene(renderer, scene, rasm_create_light(renderer, &light));
    light.type = RASM_LIGHT_TYPE_POINT;
    light.color = vec3(1.0f, 0.6f, 0.3f);
    light.position = vec3(0.8f, -0.4f, 1.4f);
    light.intensity = 6.0f;
    light.range = 6.0f;
    rasm_add_light_to_scene(renderer, scene, rasm_create_light(renderer, &light));

    /* 4. The camera. It has no controller: the loop below places it every frame. */
    memset(&projection, 0, sizeof(projection));
    projection.type = RASM_PROJECTION_TYPE_PERSPECTIVE;
    projection.fov_y = 40.0f;
    projection.aspect = (float)WIDTH / (float)HEIGHT;
    projection.near_z = 0.1f;
    camera = rasm_create_camera(renderer, &projection, NULL);
    rasm_add_camera_to_scene(renderer, scene, camera);

    if (args.waitForLoads)
        rasm_wait_for_loads(renderer);

    /* 5. The frame loop. Headless, frame N's copy is asked for before it starts and taken once it is drawn. */
    image = rasm_image_create();
    for (frame = 1; rasm_running(renderer); ++frame)
    {
        const float angle = 0.5f + 0.01f * (float)frame;
        const rasm_transform at = lookAt(vec3(3.6f * sinf(angle), 0.4f, 3.6f * cosf(angle)), vec3(0.0f, 0.0f, 0.0f));
        rasm_set_camera_transform(renderer, camera, &at);
        if (args.headless && frame == args.frames)
            readback = rasm_read_frame(renderer, RASM_READBACK_FORMAT_RGBA8_SRGB);
        rasm_render(renderer, scene, camera);
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

    rasm_image_destroy(image);
    rasm_engine_destroy(engine);
    if (args.headless && !written)
    {
        fprintf(stderr, "error: frame %u was not written to %s: %s\n", args.frames, args.out, rasm_get_last_error());
        return 1;
    }
    return 0;
}
