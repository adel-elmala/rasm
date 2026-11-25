// clang-format off
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The RASM C API: a stable C ABI over the C++ engine, for C programs and for bindings in other languages.
 *
 * Link exactly one of the two shared libraries: rasm_c (the renderer, the Engine and the AssetLoader) or rasm_c_renderer
 * (the renderer alone: no SDL3, no file loaders, no rasm_engine_* or rasm_asset_loader_* functions). Both export the
 * same renderer functions.
 *
 * ABI rules. Within one major version the API only grows at its end: new functions, new enum constants and new struct
 * fields after the last one. Nothing is renumbered, reordered or removed. A struct that starts with `size` is read and
 * written only up to the size the caller set: on create a field past it takes its default, on update it keeps its value.
 * In an array of such structs every element has the size of the first one. A creation call takes the caller's
 * RASM_C_API_VERSION and fails when the major versions differ, or when the caller's minor version is newer.
 *
 * Handles are 64-bit ids (generation << 32 | index); 0 is invalid. Strings are UTF-8 and borrowed for the call. Pointers
 * the library returns stay valid until the call the function names. Call the renderer from one thread, as in C++. The
 * last error is per thread; the log callback can run on the loader's worker threads.
 */

// NOLINTBEGIN(readability-identifier-naming): C names, snake_case with the rasm_ prefix, as C APIs and their bindings expect
#define RASM_C_API_VERSION_MAJOR    1
#define RASM_C_API_VERSION_MINOR    0
#define RASM_C_API_VERSION          (((uint32_t)RASM_C_API_VERSION_MAJOR << 16) | (uint32_t)RASM_C_API_VERSION_MINOR)

#if defined(_WIN32)
    #if defined(RASM_C_BUILD)
        #define RASM_C_API          __declspec(dllexport)
    #else
        #define RASM_C_API          __declspec(dllimport)
    #endif
#else
    #define RASM_C_API              __attribute__((visibility("default")))
#endif

#if defined(_WIN32) && !defined(_WIN64)
    #define RASM_CALL               __cdecl
#else
    #define RASM_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t                    rasm_bool;
#define RASM_FALSE                  0u
#define RASM_TRUE                   1u

typedef int32_t                     rasm_result;
enum
{
    RASM_OK                         =  0,
    RASM_ERROR_INVALID_ARGUMENT     = -1,       /* a null pointer, a size too small, a value out of range */
    RASM_ERROR_FAILED               = -2,       /* RASM refused or failed the call and logged why: rasm_get_last_error */
    RASM_ERROR_OUT_OF_MEMORY        = -3,
    RASM_ERROR_INTERNAL             = -4,       /* an exception reached the C boundary */
    RASM_ERROR_VERSION              = -5,       /* the caller's RASM_C_API_VERSION does not match the library */
};

typedef uint32_t                    rasm_log_level;
enum
{
    RASM_LOG_LEVEL_TRACE            = 0,
    RASM_LOG_LEVEL_DEBUG            = 1,
    RASM_LOG_LEVEL_INFO             = 2,
    RASM_LOG_LEVEL_WARN             = 3,
    RASM_LOG_LEVEL_ERROR            = 4,
    RASM_LOG_LEVEL_CRITICAL         = 5,
};

typedef uint32_t                    rasm_platform_type;
enum
{
    RASM_PLATFORM_TYPE_SDL          = 0,
    RASM_PLATFORM_TYPE_HEADLESS     = 1,
    RASM_PLATFORM_TYPE_NATIVE       = 2,
};

typedef uint32_t                    rasm_native_window_type;
enum
{
    RASM_NATIVE_WINDOW_TYPE_NONE    = 0,
    RASM_NATIVE_WINDOW_TYPE_WINDOWS = 1,
    RASM_NATIVE_WINDOW_TYPE_X11     = 2,
    RASM_NATIVE_WINDOW_TYPE_XCB     = 3,
    RASM_NATIVE_WINDOW_TYPE_WAYLAND = 4,
    RASM_NATIVE_WINDOW_TYPE_METAL   = 5,
};

typedef uint32_t                    rasm_backend;
enum
{
    RASM_BACKEND_VULKAN             = 0,
    RASM_BACKEND_DX12               = 1,
    RASM_BACKEND_METAL              = 2,
    RASM_BACKEND_WEBGPU             = 3,
};

typedef uint32_t                    rasm_format;
enum
{
    RASM_FORMAT_UNKNOWN             = 0,
    RASM_FORMAT_R8G8B8A8_UNORM      = 1,
    RASM_FORMAT_R8G8B8A8_SRGB       = 2,
    RASM_FORMAT_B8G8R8A8_SRGB       = 3,
    RASM_FORMAT_B8G8R8A8_UNORM      = 4,
    RASM_FORMAT_R8G8B8_UNORM        = 5,
    RASM_FORMAT_R16G16B16A16_SFLOAT = 6,
    RASM_FORMAT_R32G32B32_SFLOAT    = 7,
    RASM_FORMAT_R32G32B32A32_SFLOAT = 8,
    RASM_FORMAT_D24_UNORM_S8_UINT   = 9,
    RASM_FORMAT_D32_SFLOAT          = 10,
    RASM_FORMAT_D32_SFLOAT_S8_UINT  = 11,
    RASM_FORMAT_R32G32_SFLOAT       = 12,
    RASM_FORMAT_U16_UINT            = 13,
    RASM_FORMAT_U32_UINT            = 14,
    RASM_FORMAT_BC4_UNORM           = 15,
    RASM_FORMAT_BC5_UNORM           = 16,
    RASM_FORMAT_BC7_UNORM           = 17,
    RASM_FORMAT_BC7_SRGB            = 18,
    RASM_FORMAT_ASTC_4X4_UNORM      = 19,
    RASM_FORMAT_ASTC_4X4_SRGB       = 20,
    RASM_FORMAT_ETC2_R8G8B8A8_UNORM = 21,
    RASM_FORMAT_ETC2_R8G8B8A8_SRGB  = 22,
    RASM_FORMAT_EAC_R11_UNORM       = 23,
    RASM_FORMAT_EAC_R11G11_UNORM    = 24,
    RASM_FORMAT_R16G16_SFLOAT       = 25,
};

typedef uint32_t                    rasm_anti_aliasing;
enum
{
    RASM_ANTI_ALIASING_NONE         = 0,
    RASM_ANTI_ALIASING_MSAA_2X      = 1,
    RASM_ANTI_ALIASING_MSAA_4X      = 2,
    RASM_ANTI_ALIASING_MSAA_8X      = 3,
    RASM_ANTI_ALIASING_TAA          = 4,
    RASM_ANTI_ALIASING_DEFAULT      = 5,
};

typedef uint32_t                    rasm_meshlet_path;
enum
{
    RASM_MESHLET_PATH_OFF           = 0,
    RASM_MESHLET_PATH_VERTEX_PULLING = 1,
    RASM_MESHLET_PATH_MESH_SHADERS  = 2,
    RASM_MESHLET_PATH_AUTO          = 3,
};

typedef uint32_t                    rasm_material_type;
enum
{
    RASM_MATERIAL_TYPE_PBR          = 0,
    RASM_MATERIAL_TYPE_UNLIT        = 1,
};

typedef uint32_t                    rasm_pbr_slot;
enum
{
    RASM_PBR_SLOT_ALBEDO            = 0,
    RASM_PBR_SLOT_METALLIC          = 1,
    RASM_PBR_SLOT_ROUGHNESS         = 2,
    RASM_PBR_SLOT_NORMAL            = 3,
    RASM_PBR_SLOT_OCCLUSION         = 4,
    RASM_PBR_SLOT_EMISSIVE          = 5,
};

typedef uint32_t                    rasm_pbr_param;
enum
{
    RASM_PBR_PARAM_ALBEDO           = 0,        /* 4 floats */
    RASM_PBR_PARAM_METALLIC         = 4,
    RASM_PBR_PARAM_ROUGHNESS        = 5,
    RASM_PBR_PARAM_NORMAL_SCALE     = 6,
    RASM_PBR_PARAM_OCCLUSION_STRENGTH = 7,
    RASM_PBR_PARAM_EMISSIVE_FACTOR  = 8,        /* 3 floats */
    RASM_PBR_PARAM_EMISSIVE_STRENGTH = 11,
};

typedef uint32_t                    rasm_alpha_mode;
enum
{
    RASM_ALPHA_MODE_NONE            = 0,
    RASM_ALPHA_MODE_MASK            = 1,
    RASM_ALPHA_MODE_BLEND           = 2,
};

typedef uint32_t                    rasm_light_type;
enum
{
    RASM_LIGHT_TYPE_DIRECTIONAL     = 0,
    RASM_LIGHT_TYPE_POINT           = 1,
    RASM_LIGHT_TYPE_SPOT            = 2,
};

typedef uint32_t                    rasm_projection_type;
enum
{
    RASM_PROJECTION_TYPE_PERSPECTIVE  = 0,
    RASM_PROJECTION_TYPE_ORTHOGRAPHIC = 1,
};

typedef uint32_t                    rasm_readback_format;
enum
{
    RASM_READBACK_FORMAT_RGBA8_SRGB   = 0,
    RASM_READBACK_FORMAT_RGBA32_FLOAT = 1,
};

#define RASM_MAX_VERTEX_UV_SETS     3
#define RASM_MAX_TEXTURE_SLOTS      6
#define RASM_MAX_PARAM_SLOTS        12

typedef struct rasm_renderer        rasm_renderer;
typedef struct rasm_engine          rasm_engine;
typedef struct rasm_asset_loader    rasm_asset_loader;
typedef struct rasm_image           rasm_image;

typedef struct rasm_scene           { uint64_t id; } rasm_scene;
typedef struct rasm_entity          { uint64_t id; } rasm_entity;
typedef struct rasm_mesh            { uint64_t id; } rasm_mesh;
typedef struct rasm_material        { uint64_t id; } rasm_material;
typedef struct rasm_texture         { uint64_t id; } rasm_texture;
typedef struct rasm_light           { uint64_t id; } rasm_light;
typedef struct rasm_camera          { uint64_t id; } rasm_camera;
typedef struct rasm_environment     { uint64_t id; } rasm_environment;
typedef struct rasm_readback        { uint64_t id; } rasm_readback;

typedef struct rasm_vec2            { float x, y; } rasm_vec2;
typedef struct rasm_vec3            { float x, y, z; } rasm_vec3;
typedef struct rasm_vec4            { float x, y, z, w; } rasm_vec4;
typedef struct rasm_quat            { float x, y, z, w; } rasm_quat;
typedef struct rasm_mat4            { float m[16]; } rasm_mat4;                 /* column-major */

typedef struct rasm_transform
{
    rasm_vec3                       position;
    rasm_quat                       rotation;
    rasm_vec3                       scale;
} rasm_transform;

typedef struct rasm_native_window
{
    rasm_native_window_type         type;
    void*                           display;
    void*                           window;
    uint32_t                        pixel_width;
    uint32_t                        pixel_height;
    float                           content_scale;
} rasm_native_window;

typedef struct rasm_renderer_config
{
    uint32_t                        size;
    const char*                     app_name;
    int32_t                         window_width;
    int32_t                         window_height;
    rasm_native_window              native_window;
    rasm_bool                       enable_validation;
    rasm_bool                       enable_sync_validation;
    rasm_bool                       enable_gpu_validation;
    rasm_backend                    preferred_backend;
    const char*                     shader_dir;
    const char*                     cache_dir;
    uint32_t                        geometry_pool_vertices;
    uint32_t                        geometry_pool_colors;
    uint32_t                        geometry_pool_skins;
    uint32_t                        geometry_pool_morphs;
    uint32_t                        geometry_pool_indices;
    uint32_t                        geometry_pool_sub_meshes;
    uint32_t                        geometry_pool_meshlets;
    uint32_t                        geometry_pool_meshlet_data;
    uint32_t                        max_instances;
    uint32_t                        max_lights;
    uint32_t                        max_materials;
    uint32_t                        max_animated_entities;
    uint32_t                        animation_pool_bytes;
    uint32_t                        max_morph_weights;
    rasm_bool                       show_stats;
    float                           fixed_delta_time;
    rasm_bool                       occlusion_culling;
    rasm_bool                       mesh_lods;
    float                           lod_error_pixels;
    int32_t                         forced_lod;
    rasm_meshlet_path               meshlet_path;
    uint32_t                        max_view_meshlets;
    rasm_bool                       async_compute;
    rasm_bool                       transfer_queue;
    rasm_bool                       clustered_lighting;
    rasm_bool                       host_image_copy;
    rasm_bool                       bloom;
    rasm_bool                       ui;
    rasm_anti_aliasing              anti_aliasing;
} rasm_renderer_config;

typedef struct rasm_asset_loader_config
{
    uint32_t                        size;
    const char*                     asset_root;
    uint32_t                        load_threads;
    uint64_t                        load_memory_budget;
} rasm_asset_loader_config;

typedef struct rasm_engine_config
{
    uint32_t                        size;
    rasm_platform_type              platform;
    const rasm_renderer_config*     renderer;
    const rasm_asset_loader_config* loader;                                    /* NULL: the defaults */
} rasm_engine_config;

typedef struct rasm_vertex
{
    rasm_vec3                       position;
    rasm_vec3                       normal;
    rasm_vec2                       uv[RASM_MAX_VERTEX_UV_SETS];
    rasm_vec4                       tangent;
} rasm_vertex;

typedef struct rasm_texture_transform
{
    rasm_vec2                       offset;
    float                           rotation;
    rasm_vec2                       scale;
} rasm_texture_transform;

typedef struct rasm_material_desc
{
    uint32_t                        size;
    rasm_material_type              type;
    rasm_texture                    textures[RASM_MAX_TEXTURE_SLOTS];
    uint32_t                        tex_coords[RASM_MAX_TEXTURE_SLOTS];
    rasm_texture_transform          texture_transforms[RASM_MAX_TEXTURE_SLOTS];
    float                           params[RASM_MAX_PARAM_SLOTS];
    rasm_bool                       double_sided;
    rasm_alpha_mode                 alpha_mode;
    float                           alpha_cutoff;
} rasm_material_desc;

typedef struct rasm_texture_desc
{
    uint32_t                        size;
    const char*                     name;
    uint32_t                        width;
    uint32_t                        height;
    rasm_format                     format;
} rasm_texture_desc;

typedef struct rasm_light_desc
{
    uint32_t                        size;
    rasm_light_type                 type;
    float                           intensity;
    float                           range;
    rasm_vec3                       color;
    rasm_vec3                       position;
    rasm_vec3                       direction;
    float                           inner_cone_angle;
    float                           outer_cone_angle;
} rasm_light_desc;

typedef struct rasm_camera_projection
{
    uint32_t                        size;
    rasm_projection_type            type;
    float                           fov_y;                                     /* perspective, degrees */
    float                           aspect;
    float                           near_z;
    float                           far_z;
    float                           left;                                      /* orthographic */
    float                           right;
    float                           bottom;
    float                           top;
} rasm_camera_projection;

typedef struct rasm_scene_environment
{
    uint32_t                        size;
    rasm_environment                environment;
    float                           intensity;
    float                           rotation;
    rasm_bool                       sky;
} rasm_scene_environment;

typedef struct rasm_image_data
{
    uint32_t                        width;
    uint32_t                        height;
    rasm_readback_format            format;
    const uint8_t*                  pixels;                                    /* until the image is filled again or destroyed */
    uint64_t                        byte_size;
} rasm_image_data;

typedef void (RASM_CALL* rasm_log_callback)(void* user_data, rasm_log_level level, const char* message);

/* The library */
RASM_C_API uint32_t         RASM_CALL rasm_get_version(void);
RASM_C_API const char*      RASM_CALL rasm_get_last_error(void);
RASM_C_API void             RASM_CALL rasm_clear_last_error(void);
RASM_C_API void             RASM_CALL rasm_set_log_callback(rasm_log_callback callback, void* user_data);
RASM_C_API uint32_t         RASM_CALL rasm_get_base_path(char* buffer, uint32_t capacity);

/* Defaults of the sized structs */
RASM_C_API rasm_result      RASM_CALL rasm_renderer_config_init(rasm_renderer_config* config, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_asset_loader_config_init(rasm_asset_loader_config* config, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_engine_config_init(rasm_engine_config* config, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_texture_desc_init(rasm_texture_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_light_desc_init(rasm_light_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_scene_environment_init(rasm_scene_environment* environment, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_camera_projection_init(rasm_camera_projection* projection, uint32_t size);

/* Renderer, Engine, AssetLoader */
RASM_C_API rasm_result      RASM_CALL rasm_renderer_create(uint32_t api_version, const rasm_renderer_config* config, rasm_renderer** renderer);
RASM_C_API void             RASM_CALL rasm_renderer_destroy(rasm_renderer* renderer);
RASM_C_API rasm_result      RASM_CALL rasm_engine_create(uint32_t api_version, const rasm_engine_config* config, rasm_engine** engine);
RASM_C_API void             RASM_CALL rasm_engine_destroy(rasm_engine* engine);
RASM_C_API rasm_renderer*   RASM_CALL rasm_engine_get_renderer(rasm_engine* engine);
RASM_C_API rasm_asset_loader* RASM_CALL rasm_engine_get_asset_loader(rasm_engine* engine);
RASM_C_API rasm_result      RASM_CALL rasm_asset_loader_create(rasm_renderer* renderer, const rasm_asset_loader_config* config, rasm_asset_loader** loader);
RASM_C_API void             RASM_CALL rasm_asset_loader_destroy(rasm_asset_loader* loader);

/* Frame */
RASM_C_API rasm_bool        RASM_CALL rasm_running(rasm_renderer* renderer);
RASM_C_API void             RASM_CALL rasm_render(rasm_renderer* renderer, rasm_scene scene, rasm_camera camera);
RASM_C_API void             RASM_CALL rasm_wait_for_loads(rasm_renderer* renderer);

/* Scenes */
RASM_C_API rasm_scene       RASM_CALL rasm_create_scene(rasm_renderer* renderer);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_scene(rasm_renderer* renderer, rasm_scene scene);
RASM_C_API rasm_result      RASM_CALL rasm_add_entity_to_scene(rasm_renderer* renderer, rasm_scene scene, rasm_entity entity);
RASM_C_API rasm_result      RASM_CALL rasm_remove_entity_from_scene(rasm_renderer* renderer, rasm_scene scene, rasm_entity entity);
RASM_C_API rasm_result      RASM_CALL rasm_add_camera_to_scene(rasm_renderer* renderer, rasm_scene scene, rasm_camera camera);
RASM_C_API rasm_result      RASM_CALL rasm_add_light_to_scene(rasm_renderer* renderer, rasm_scene scene, rasm_light light);
RASM_C_API rasm_result      RASM_CALL rasm_remove_light_from_scene(rasm_renderer* renderer, rasm_scene scene, rasm_light light);
RASM_C_API rasm_result      RASM_CALL rasm_set_scene_environment(rasm_renderer* renderer, rasm_scene scene, const rasm_scene_environment* environment);

/* Entities */
RASM_C_API rasm_entity      RASM_CALL rasm_create_entity(rasm_renderer* renderer, const char* name, rasm_mesh mesh, rasm_material material, const rasm_transform* transform);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_entity(rasm_renderer* renderer, rasm_entity entity);
RASM_C_API rasm_result      RASM_CALL rasm_get_transform(rasm_renderer* renderer, rasm_entity entity, rasm_transform* transform);
RASM_C_API rasm_result      RASM_CALL rasm_update_transform(rasm_renderer* renderer, rasm_entity entity, const rasm_transform* transform);

/* Meshes */
RASM_C_API rasm_mesh        RASM_CALL rasm_create_mesh(rasm_renderer* renderer, const rasm_vertex* vertices, uint32_t vertex_count, const uint32_t* indices, uint32_t index_count, const uint32_t* colors);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_mesh(rasm_renderer* renderer, rasm_mesh mesh);
RASM_C_API rasm_bool        RASM_CALL rasm_is_mesh_loaded(rasm_renderer* renderer, rasm_mesh mesh);
RASM_C_API uint32_t         RASM_CALL rasm_pack_vertex_color(rasm_vec4 linear_color);

/* Materials and textures */
RASM_C_API rasm_material    RASM_CALL rasm_create_material(rasm_renderer* renderer, rasm_material_type type, const rasm_texture* textures, uint32_t texture_count, rasm_bool double_sided);
RASM_C_API rasm_result      RASM_CALL rasm_get_material(rasm_renderer* renderer, rasm_material material, rasm_material_desc* desc);
RASM_C_API rasm_result      RASM_CALL rasm_update_material(rasm_renderer* renderer, rasm_material material, const rasm_material_desc* desc);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_material(rasm_renderer* renderer, rasm_material material);
RASM_C_API rasm_texture     RASM_CALL rasm_create_texture(rasm_renderer* renderer, const rasm_texture_desc* desc, const void* pixels);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_texture(rasm_renderer* renderer, rasm_texture texture);
RASM_C_API rasm_bool        RASM_CALL rasm_is_texture_loaded(rasm_renderer* renderer, rasm_texture texture);

/* Lights */
RASM_C_API rasm_light       RASM_CALL rasm_create_light(rasm_renderer* renderer, const rasm_light_desc* desc);
RASM_C_API rasm_result      RASM_CALL rasm_get_light(rasm_renderer* renderer, rasm_light light, rasm_light_desc* desc);
RASM_C_API rasm_result      RASM_CALL rasm_update_light(rasm_renderer* renderer, rasm_light light, const rasm_light_desc* desc);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_light(rasm_renderer* renderer, rasm_light light);

/* Cameras */
RASM_C_API rasm_camera      RASM_CALL rasm_create_camera(rasm_renderer* renderer, const rasm_camera_projection* projection, const rasm_transform* transform);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_camera(rasm_renderer* renderer, rasm_camera camera);
RASM_C_API rasm_result      RASM_CALL rasm_get_camera_transform(rasm_renderer* renderer, rasm_camera camera, rasm_transform* transform);
RASM_C_API rasm_result      RASM_CALL rasm_set_camera_transform(rasm_renderer* renderer, rasm_camera camera, const rasm_transform* transform);
RASM_C_API rasm_result      RASM_CALL rasm_get_camera_projection(rasm_renderer* renderer, rasm_camera camera, rasm_camera_projection* projection);
RASM_C_API rasm_result      RASM_CALL rasm_set_camera_projection(rasm_renderer* renderer, rasm_camera camera, const rasm_camera_projection* projection);
RASM_C_API float            RASM_CALL rasm_get_camera_exposure(rasm_renderer* renderer, rasm_camera camera);
RASM_C_API rasm_result      RASM_CALL rasm_set_camera_exposure(rasm_renderer* renderer, rasm_camera camera, float exposure);
RASM_C_API float            RASM_CALL rasm_get_camera_bloom_strength(rasm_renderer* renderer, rasm_camera camera);
RASM_C_API rasm_result      RASM_CALL rasm_set_camera_bloom_strength(rasm_renderer* renderer, rasm_camera camera, float strength);
RASM_C_API rasm_result      RASM_CALL rasm_reset_camera_history(rasm_renderer* renderer, rasm_camera camera);
RASM_C_API rasm_result      RASM_CALL rasm_get_view_matrix(rasm_renderer* renderer, rasm_camera camera, rasm_mat4* matrix);
RASM_C_API rasm_result      RASM_CALL rasm_get_projection_matrix(rasm_renderer* renderer, rasm_camera camera, float aspect, rasm_mat4* matrix);

/* Environments */
RASM_C_API rasm_result      RASM_CALL rasm_destroy_environment(rasm_renderer* renderer, rasm_environment environment);
RASM_C_API rasm_bool        RASM_CALL rasm_is_environment_loaded(rasm_renderer* renderer, rasm_environment environment);

/* Loading files (rasm_c only) */
RASM_C_API rasm_mesh        RASM_CALL rasm_asset_loader_load_mesh(rasm_asset_loader* loader, const char* path);
RASM_C_API rasm_mesh        RASM_CALL rasm_asset_loader_load_gltf(rasm_asset_loader* loader, const char* path);
RASM_C_API rasm_texture     RASM_CALL rasm_asset_loader_load_texture(rasm_asset_loader* loader, const char* path, rasm_format format);
RASM_C_API rasm_environment RASM_CALL rasm_asset_loader_load_environment(rasm_asset_loader* loader, const char* path);
RASM_C_API void             RASM_CALL rasm_asset_loader_wait_for_loads(rasm_asset_loader* loader);
RASM_C_API uint32_t         RASM_CALL rasm_asset_loader_resolve_asset_path(rasm_asset_loader* loader, const char* path, char* buffer, uint32_t capacity);

/* Readback */
RASM_C_API rasm_readback    RASM_CALL rasm_read_frame(rasm_renderer* renderer, rasm_readback_format format);
RASM_C_API rasm_bool        RASM_CALL rasm_is_readback_ready(rasm_renderer* renderer, rasm_readback readback);
RASM_C_API rasm_result      RASM_CALL rasm_take_readback(rasm_renderer* renderer, rasm_readback readback, rasm_image* image);
RASM_C_API rasm_image*      RASM_CALL rasm_image_create(void);
RASM_C_API void             RASM_CALL rasm_image_destroy(rasm_image* image);
RASM_C_API rasm_image_data  RASM_CALL rasm_image_get(const rasm_image* image);
RASM_C_API rasm_result      RASM_CALL rasm_image_write_png(const rasm_image* image, const char* path);
RASM_C_API rasm_result      RASM_CALL rasm_image_read_png(rasm_image* image, const char* path);

#ifdef __cplusplus
}
#endif
// NOLINTEND(readability-identifier-naming)
// clang-format on
