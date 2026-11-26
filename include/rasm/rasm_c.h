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
#define RASM_C_API_VERSION_MINOR    13
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
    RASM_FORMAT_R16_UNORM           = 26,       /* 1.4: a terrain tile's heights */
    RASM_FORMAT_R32_SFLOAT          = 27,       /* 1.4: a terrain tile's heights */
    RASM_FORMAT_R8_UNORM            = 28,       /* 1.6: a font's distance atlas */
    RASM_FORMAT_R32G32B32A32_UINT   = 29,       /* 1.7: the pick pass's ids and positions */
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
    RASM_MATERIAL_TYPE_TERRAIN      = 2,        /* 1.4: a terrain tile (rasm_create_terrain_tile) */
    RASM_MATERIAL_TYPE_VECTOR       = 3,        /* 1.5: lines, polygons and points (rasm_vector_*), styled by the desc's vector */
    RASM_MATERIAL_TYPE_POINTS       = 4,        /* 1.10: point cloud chunks (rasm_build_point_cloud_chunk), styled by the desc's points */
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
#define RASM_MAX_TERRAIN_CELLS      256         /* 1.4 */

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
typedef struct rasm_font            { uint64_t id; } rasm_font;                 /* 1.6: a font's atlas on the GPU */

typedef struct rasm_vec2            { float x, y; } rasm_vec2;
typedef struct rasm_vec3            { float x, y, z; } rasm_vec3;
typedef struct rasm_vec4            { float x, y, z, w; } rasm_vec4;
typedef struct rasm_quat            { float x, y, z, w; } rasm_quat;
typedef struct rasm_mat4            { float m[16]; } rasm_mat4;                 /* column-major */
typedef struct rasm_dvec3           { double x, y, z; } rasm_dvec3;             /* 1.2: a world position in doubles */

typedef struct rasm_transform
{
    rasm_vec3                       position;
    rasm_quat                       rotation;
    rasm_vec3                       scale;
} rasm_transform;

/* 1.2: a transform whose position is in doubles, as the engine keeps it: precise millions of metres from the origin. */
typedef struct rasm_transform_d
{
    rasm_dvec3                      position;
    rasm_quat                       rotation;
    rasm_vec3                       scale;
} rasm_transform_d;

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
    uint32_t                        max_bindless_textures;                     /* 1.3: sampled textures at once, 16384 by default, lowered to the device's limit */
    uint32_t                        geometry_pool_points;                      /* 1.10: point cloud chunks, 8 B units: a point takes 1 (1.5 with colors) */
    rasm_bool                       compute_points;                            /* 1.11: small points by the compute rasterizer (up to 6 pixels at a chunk's centre and 8 at its nearest point; 4 in an orthographic view, 2 under MSAA), where the GPU has 64-bit buffer atomics (RASM_TRUE by default) */
    uint32_t                        shader_threads;                            /* 1.13: threads that compile shaders in the background; 0 (the default): half the hardware threads, at most 8. A cold shader cache costs about 100 MB a thread for about 1 s */
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

/* 1.4: a TERRAIN material's heights. A height in metres is height_offset + height_scale * sample: an R16_UNORM sample is its
 * integer (0 to 65535), an R32_SFLOAT sample its value. A power-of-two height_scale and an offset that is a multiple of
 * it give neighbouring tiles bit-equal heights on their shared edge. */
typedef struct rasm_terrain_material
{
    rasm_texture                    heights;                                   /* R16_UNORM or R32_SFLOAT, (cells + 3)^2 samples: the grid's points and a border of one */
    float                           height_scale;
    float                           height_offset;
    float                           min_height;                                /* metres: the tile's box */
    float                           max_height;
    float                           skirt;                                     /* metres the skirt hangs below the edges */
    double                          sphere_radius;                             /* 0: flat; else the tile bends down by d^2 / 2R */
} rasm_terrain_material;

/* 1.5: a VECTOR material's style (VectorMaterial in C++). */
typedef uint32_t                    rasm_vector_unit;
enum
{
    RASM_VECTOR_UNIT_PIXELS         = 0,
    RASM_VECTOR_UNIT_METRES         = 1,
};

typedef uint32_t                    rasm_vector_facing;
enum
{
    RASM_VECTOR_FACING_SCREEN       = 0,        /* across the screen */
    RASM_VECTOR_FACING_GROUND       = 1,        /* in the ground's plane */
};

typedef uint32_t                    rasm_vector_depth;
enum
{
    RASM_VECTOR_DEPTH_TESTED        = 0,
    RASM_VECTOR_DEPTH_OVERLAY       = 1,        /* over everything */
};

typedef struct rasm_vector_material
{
    rasm_vec4                       color;                                     /* linear RGB and alpha */
    float                           width;                                     /* a line's width, a point's diameter, in width_unit */
    rasm_vector_unit                width_unit;
    rasm_vector_facing              facing;
    float                           dash_length;                               /* in width_unit; 0: solid */
    float                           gap_length;
    rasm_vec4                       outline_color;
    float                           outline_width;                             /* in width_unit, inside width; 0: none */
    int32_t                         draw_order;                                /* -127 to 127 */
    rasm_vector_depth               depth;
} rasm_vector_material;

/* 1.10: colormaps and a POINTS material's style (PointMaterial in C++). */
#define RASM_COLORMAP_SIZE          256u        /* 1.10: entries of a colormap's texture */
#define RASM_MAX_POINT_CHUNK_POINTS 65536u      /* 1.10 */

typedef uint32_t                    rasm_colormap_preset;
enum
{
    RASM_COLORMAP_VIRIDIS           = 0,
    RASM_COLORMAP_TURBO             = 1,
    RASM_COLORMAP_MAGMA             = 2,
    RASM_COLORMAP_INFERNO           = 3,
    RASM_COLORMAP_PLASMA            = 4,
    RASM_COLORMAP_CIVIDIS           = 5,
    RASM_COLORMAP_GRAY              = 6,
};

typedef uint32_t                    rasm_point_shape;
enum
{
    RASM_POINT_SHAPE_SQUARE         = 0,
    RASM_POINT_SHAPE_ROUND          = 1,
    RASM_POINT_SHAPE_SPHERE         = 2,        /* round, with the depth of a sphere */
};

typedef uint32_t                    rasm_point_size_mode;
enum
{
    RASM_POINT_SIZE_MODE_FIXED      = 0,        /* 1.12: size in size_unit */
    RASM_POINT_SIZE_MODE_ADAPTIVE   = 1,        /* 1.12: from size, grown where the chunk's spacing on screen leaves gaps, up to max_size pixels */
};

typedef uint32_t                    rasm_point_color_mode;
enum
{
    RASM_POINT_COLOR_MODE_COLOR     = 0,        /* the points' own colors, white without them */
    RASM_POINT_COLOR_MODE_SCALAR    = 1,        /* the points' scalars through the colormap and the range */
};

typedef struct rasm_scalar_range
{
    float                           min;
    float                           max;
    rasm_bool                       log;                                       /* both ends above 0 */
    rasm_bool                       clamp;                                     /* RASM_FALSE: values outside are not drawn */
} rasm_scalar_range;

typedef struct rasm_point_material
{
    rasm_vec4                       color;                                     /* linear, times the point's color */
    float                           size;                                      /* the diameter in size_unit, at least a pixel; 2 by default */
    rasm_vector_unit                size_unit;
    rasm_point_shape                shape;                                     /* ROUND by default */
    rasm_point_color_mode           color_mode;
    rasm_texture                    colormap;                                  /* rasm_create_colormap */
    rasm_scalar_range               range;                                     /* 0 to 1, clamped, by default */
    float                           density;                                   /* points drawn per point-size square of the chunk on screen; 2 by default, 0: all */
    rasm_point_size_mode            size_mode;                                 /* 1.12: FIXED by default */
    float                           max_size;                                  /* 1.12: ADAPTIVE: the largest diameter in pixels; 8 by default */
} rasm_point_material;

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
    rasm_terrain_material           terrain;                                   /* 1.4: TERRAIN only; textures[ALBEDO] is the imagery */
    rasm_vector_material            vector;                                    /* 1.5: VECTOR only; textures[ALBEDO] is the points' icon atlas */
    rasm_font                       font;                                      /* 1.6: VECTOR labels' glyphs; vector.width is the text's size, outline its halo */
    rasm_point_material             points;                                    /* 1.10: POINTS only */
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
    rasm_dvec3                      position_d;                                /* 1.2: added to position: the light is at position + position_d, in doubles */
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
    rasm_quat                       orientation;                               /* 1.2: the environment's frame after rotation: its +Y is the sky's up */
} rasm_scene_environment;

typedef struct rasm_image_data
{
    uint32_t                        width;
    uint32_t                        height;
    rasm_readback_format            format;
    const uint8_t*                  pixels;                                    /* until the image is filled again or destroyed */
    uint64_t                        byte_size;
} rasm_image_data;

/* Added in 1.1: windows, input, views, render targets, camera helpers, animation, stats. */

#define RASM_MAX_MESH_LODS          8
#define RASM_ALL_ANIMATION_CLIPS    0xFFFFFFFEu
#define RASM_MAX_VIEWS              32

typedef uint32_t                    rasm_key;
enum
{
    RASM_KEY_NONE                    = 0,
    RASM_KEY_A                       = 1,
    RASM_KEY_B                       = 2,
    RASM_KEY_C                       = 3,
    RASM_KEY_D                       = 4,
    RASM_KEY_E                       = 5,
    RASM_KEY_F                       = 6,
    RASM_KEY_G                       = 7,
    RASM_KEY_H                       = 8,
    RASM_KEY_I                       = 9,
    RASM_KEY_J                       = 10,
    RASM_KEY_K                       = 11,
    RASM_KEY_L                       = 12,
    RASM_KEY_M                       = 13,
    RASM_KEY_N                       = 14,
    RASM_KEY_O                       = 15,
    RASM_KEY_P                       = 16,
    RASM_KEY_Q                       = 17,
    RASM_KEY_R                       = 18,
    RASM_KEY_S                       = 19,
    RASM_KEY_T                       = 20,
    RASM_KEY_U                       = 21,
    RASM_KEY_V                       = 22,
    RASM_KEY_W                       = 23,
    RASM_KEY_X                       = 24,
    RASM_KEY_Y                       = 25,
    RASM_KEY_Z                       = 26,
    RASM_KEY_DIGIT_0                 = 27,
    RASM_KEY_DIGIT_1                 = 28,
    RASM_KEY_DIGIT_2                 = 29,
    RASM_KEY_DIGIT_3                 = 30,
    RASM_KEY_DIGIT_4                 = 31,
    RASM_KEY_DIGIT_5                 = 32,
    RASM_KEY_DIGIT_6                 = 33,
    RASM_KEY_DIGIT_7                 = 34,
    RASM_KEY_DIGIT_8                 = 35,
    RASM_KEY_DIGIT_9                 = 36,
    RASM_KEY_F1                      = 37,
    RASM_KEY_F2                      = 38,
    RASM_KEY_F3                      = 39,
    RASM_KEY_F4                      = 40,
    RASM_KEY_F5                      = 41,
    RASM_KEY_F6                      = 42,
    RASM_KEY_F7                      = 43,
    RASM_KEY_F8                      = 44,
    RASM_KEY_F9                      = 45,
    RASM_KEY_F10                     = 46,
    RASM_KEY_F11                     = 47,
    RASM_KEY_F12                     = 48,
    RASM_KEY_ESCAPE                  = 49,
    RASM_KEY_ENTER                   = 50,
    RASM_KEY_TAB                     = 51,
    RASM_KEY_BACKSPACE               = 52,
    RASM_KEY_SPACE                   = 53,
    RASM_KEY_INSERT                  = 54,
    RASM_KEY_DEL                     = 55,
    RASM_KEY_HOME                    = 56,
    RASM_KEY_END                     = 57,
    RASM_KEY_PAGE_UP                 = 58,
    RASM_KEY_PAGE_DOWN               = 59,
    RASM_KEY_LEFT                    = 60,
    RASM_KEY_RIGHT                   = 61,
    RASM_KEY_UP                      = 62,
    RASM_KEY_DOWN                    = 63,
    RASM_KEY_LEFT_SHIFT              = 64,
    RASM_KEY_RIGHT_SHIFT             = 65,
    RASM_KEY_LEFT_CTRL               = 66,
    RASM_KEY_RIGHT_CTRL              = 67,
    RASM_KEY_LEFT_ALT                = 68,
    RASM_KEY_RIGHT_ALT               = 69,
    RASM_KEY_LEFT_SUPER              = 70,
    RASM_KEY_RIGHT_SUPER             = 71,
    RASM_KEY_MENU                    = 72,
    RASM_KEY_APOSTROPHE              = 73,
    RASM_KEY_COMMA                   = 74,
    RASM_KEY_MINUS                   = 75,
    RASM_KEY_PERIOD                  = 76,
    RASM_KEY_SLASH                   = 77,
    RASM_KEY_SEMICOLON               = 78,
    RASM_KEY_EQUAL                   = 79,
    RASM_KEY_LEFT_BRACKET            = 80,
    RASM_KEY_BACKSLASH               = 81,
    RASM_KEY_RIGHT_BRACKET           = 82,
    RASM_KEY_GRAVE                   = 83,
    RASM_KEY_CAPS_LOCK               = 84,
    RASM_KEY_SCROLL_LOCK             = 85,
    RASM_KEY_NUM_LOCK                = 86,
    RASM_KEY_PRINT_SCREEN            = 87,
    RASM_KEY_PAUSE                   = 88,
    RASM_KEY_KEYPAD_0                = 89,
    RASM_KEY_KEYPAD_1                = 90,
    RASM_KEY_KEYPAD_2                = 91,
    RASM_KEY_KEYPAD_3                = 92,
    RASM_KEY_KEYPAD_4                = 93,
    RASM_KEY_KEYPAD_5                = 94,
    RASM_KEY_KEYPAD_6                = 95,
    RASM_KEY_KEYPAD_7                = 96,
    RASM_KEY_KEYPAD_8                = 97,
    RASM_KEY_KEYPAD_9                = 98,
    RASM_KEY_KEYPAD_DECIMAL          = 99,
    RASM_KEY_KEYPAD_DIVIDE           = 100,
    RASM_KEY_KEYPAD_MULTIPLY         = 101,
    RASM_KEY_KEYPAD_SUBTRACT         = 102,
    RASM_KEY_KEYPAD_ADD              = 103,
    RASM_KEY_KEYPAD_ENTER            = 104,
    RASM_KEY_KEYPAD_EQUAL            = 105,
    RASM_KEY_COUNT                   = 106,
};

typedef uint32_t                    rasm_mouse_button;
enum
{
    RASM_MOUSE_BUTTON_LEFT          = 0,
    RASM_MOUSE_BUTTON_RIGHT         = 1,
    RASM_MOUSE_BUTTON_MIDDLE        = 2,
    RASM_MOUSE_BUTTON_BACK          = 3,
    RASM_MOUSE_BUTTON_FORWARD       = 4,
};

typedef uint32_t                    rasm_input_event_type;
enum
{
    RASM_INPUT_EVENT_TYPE_NONE         = 0,
    RASM_INPUT_EVENT_TYPE_KEY          = 1,
    RASM_INPUT_EVENT_TYPE_MOUSE_BUTTON = 2,
    RASM_INPUT_EVENT_TYPE_MOUSE_MOVE   = 3,
    RASM_INPUT_EVENT_TYPE_MOUSE_WHEEL  = 4,
    RASM_INPUT_EVENT_TYPE_MOUSE_LEAVE  = 5,
    RASM_INPUT_EVENT_TYPE_TEXT         = 6,
    RASM_INPUT_EVENT_TYPE_FOCUS        = 7,
    RASM_INPUT_EVENT_TYPE_WINDOW_CLOSE = 8,
};

#define RASM_KEY_MOD_SHIFT          1u
#define RASM_KEY_MOD_CTRL           2u
#define RASM_KEY_MOD_ALT            4u
#define RASM_KEY_MOD_SUPER          8u

typedef uint32_t                    rasm_view_toggle;
enum
{
    RASM_VIEW_TOGGLE_DEFAULT        = 0,
    RASM_VIEW_TOGGLE_OFF            = 1,
    RASM_VIEW_TOGGLE_ON             = 2,
};

typedef uint32_t                    rasm_camera_controller_type;
enum
{
    RASM_CAMERA_CONTROLLER_TYPE_TRACK_BALL = 0,
    RASM_CAMERA_CONTROLLER_TYPE_FREE_FLY   = 1,
    RASM_CAMERA_CONTROLLER_TYPE_NONE       = 2,
};

typedef uint32_t                    rasm_orbit_style;
enum
{
    RASM_ORBIT_STYLE_TURNTABLE      = 0,    /* 1.8: turns around up and tilts, no roll */
    RASM_ORBIT_STYLE_ARCBALL        = 1,    /* 1.8: free rotation on a virtual sphere */
};

typedef struct rasm_camera_rig          rasm_camera_rig;
typedef struct rasm_camera_input_state  rasm_camera_input_state;

typedef struct rasm_window          { uint64_t id; } rasm_window;
typedef struct rasm_render_target   { uint64_t id; } rasm_render_target;

typedef struct rasm_view_rect                                                  /* fractions of the output, from its top-left corner */
{
    float                           x;
    float                           y;
    float                           width;
    float                           height;
} rasm_view_rect;

typedef struct rasm_view_pixels
{
    uint32_t                        x;
    uint32_t                        y;
    uint32_t                        width;
    uint32_t                        height;
} rasm_view_pixels;

typedef struct rasm_input_area                                                 /* window points */
{
    float                           x;
    float                           y;
    float                           width;
    float                           height;
} rasm_input_area;

typedef struct rasm_ui_input_capture
{
    rasm_bool                       mouse;
    rasm_bool                       keyboard;
    rasm_bool                       text;
} rasm_ui_input_capture;

typedef struct rasm_window_desc
{
    uint32_t                        size;
    const char*                     title;
    uint32_t                        width;
    uint32_t                        height;
    rasm_native_window              native;
    rasm_bool                       hidden;
} rasm_window_desc;

typedef struct rasm_window_info
{
    uint32_t                        size;
    rasm_window                     window;
    void*                           platform_window;                           /* the SDL platform's SDL_Window */
    rasm_native_window              native;
    uint32_t                        width;
    uint32_t                        height;
    uint32_t                        pixel_width;
    uint32_t                        pixel_height;
    float                           content_scale;
    rasm_bool                       resized;
    rasm_bool                       minimized;
} rasm_window_info;

typedef struct rasm_input_event
{
    uint32_t                        size;
    rasm_input_event_type           type;
    rasm_window                     window;                                    /* 0: the main window */
    rasm_key                        key;
    rasm_key                        physical_key;
    rasm_bool                       down;
    rasm_bool                       repeat;
    uint32_t                        modifiers;                                 /* RASM_KEY_MOD_* */
    rasm_mouse_button               button;
    float                           x;
    float                           y;
    uint32_t                        codepoint;
    float                           dx;                                        /* 1.8: a move's motion in window points; */
    float                           dy;                                        /* during a mouse grab only these move */
} rasm_input_event;

typedef struct rasm_camera_input
{
    uint32_t                        size;
    rasm_vec2                       mouse_start;
    rasm_vec2                       mouse_end;
    float                           wheel_delta;
    float                           delta_time;
    rasm_bool                       forward;
    rasm_bool                       backward;
    rasm_bool                       left;
    rasm_bool                       right;
    rasm_vec2                       pan_start;                                 /* 1.8: the pan drag, NDC */
    rasm_vec2                       pan_end;
    rasm_vec2                       pointer;                                   /* 1.8: NDC, where an orthographic view zooms */
} rasm_camera_input;

typedef struct rasm_camera_controller
{
    uint32_t                        size;
    rasm_camera_controller_type     type;
    float                           easing_speed;
    float                           track_ball_radius;
    rasm_vec3                       track_ball_center;
    float                           track_ball_orbit_sensitivity;
    float                           free_fly_yaw;
    float                           free_fly_pitch;
    float                           free_fly_speed;
    float                           free_fly_look_sensitivity;
    rasm_dvec3                      track_ball_center_d;                       /* 1.2: added to track_ball_center, in doubles */
    rasm_vec3                       free_fly_up;                               /* 1.2: the axis yaw turns around; 0, 1, 0 by default */
    rasm_orbit_style                track_ball_style;                          /* 1.8: TURNTABLE by default */
    rasm_vec3                       track_ball_up;                             /* 1.8: the turntable's axis; 0, 1, 0 by default */
} rasm_camera_controller;

typedef struct rasm_view
{
    uint32_t                        size;
    rasm_scene                      scene;
    rasm_camera                     camera;
    rasm_view_rect                  rect;
    rasm_render_target              target;                                    /* set: into this view texture */
    rasm_window                     window;                                    /* without a target: this window; 0: the main one */
    rasm_view_toggle                bloom;
    rasm_anti_aliasing              anti_aliasing;
} rasm_view;

typedef struct rasm_render_target_desc
{
    uint32_t                        size;
    const char*                     name;
    uint32_t                        width;
    uint32_t                        height;
    rasm_format                     format;
    rasm_vec4                       clear_color;
    rasm_bool                       readable;
} rasm_render_target_desc;

typedef struct rasm_animation_play
{
    uint32_t                        size;
    uint32_t                        clip;                                      /* an index, or RASM_ALL_ANIMATION_CLIPS */
    double                          start_time;
    float                           clip_time;
    float                           speed;
    rasm_bool                       loop;
} rasm_animation_play;

typedef struct rasm_frame_stats
{
    uint32_t                        size;
    float                           cpu_frame_ms;
    float                           gpu_frame_ms;
    float                           upload_gpu_ms;
    uint32_t                        draws;
    uint32_t                        draws_visible;
    uint64_t                        triangles;
    uint64_t                        triangles_visible;
    uint64_t                        triangles_visible_lod0;
    uint64_t                        triangles_visible_lod;
    uint32_t                        draws_per_lod[RASM_MAX_MESH_LODS];
    uint32_t                        meshlets_visible;
    uint32_t                        meshlets_culled;
    uint32_t                        meshlet_draws_whole;
    uint32_t                        lights_global;
    uint32_t                        lights_visible;
    uint32_t                        lights_dropped;
} rasm_frame_stats;

typedef struct rasm_pool_stats
{
    uint32_t                        size;
    uint32_t                        entities;
    uint32_t                        meshes;
    uint32_t                        materials;
    uint32_t                        lights;
    uint32_t                        textures;
    uint32_t                        bindless_textures;
    uint64_t                        vertices;
    uint64_t                        indices;
    uint64_t                        sub_meshes;
    uint64_t                        meshlets;
    uint64_t                        instances;
    uint64_t                        animation_pool_bytes;
    uint32_t                        releases_waiting;
    uint64_t                        points;                                    /* 1.12: 8 B units of geometry_pool_points */
} rasm_pool_stats;

typedef struct rasm_load_stats
{
    uint32_t                        size;
    uint32_t                        workers;
    uint32_t                        queued;
    uint32_t                        running;
    uint64_t                        decoded_bytes;
    uint64_t                        peak_decoded_bytes;
} rasm_load_stats;

/* 1.3: tile streaming (TileStreamer in C++). */
typedef struct rasm_tile_streamer   rasm_tile_streamer;
typedef struct rasm_tile            { uint64_t id; } rasm_tile;

typedef uint32_t                    rasm_tile_refine;
enum
{
    RASM_TILE_REFINE_REPLACE        = 0,        /* the children stand in for it */
    RASM_TILE_REFINE_ADD            = 1,        /* the children add to it: it stays drawn with them */
};

typedef uint32_t                    rasm_tile_state;
enum
{
    RASM_TILE_STATE_EMPTY           = 0,
    RASM_TILE_STATE_REQUESTED       = 1,        /* handed out: give it its content */
    RASM_TILE_STATE_LOADING         = 2,
    RASM_TILE_STATE_READY           = 3,
    RASM_TILE_STATE_FAILED          = 4,
};

typedef struct rasm_tile_streamer_config
{
    uint32_t                        size;
    uint64_t                        memory_budget;                             /* bytes; 0: device_share of what the device's budget leaves */
    float                           device_share;
    uint32_t                        max_tiles;
    uint32_t                        max_loads_per_frame;
    uint32_t                        max_loads_in_flight;
    uint32_t                        max_drops_per_frame;
    uint32_t                        retry_failed_after;                        /* updates; 0: never */
} rasm_tile_streamer_config;

typedef struct rasm_tile_desc
{
    uint32_t                        size;
    rasm_tile                       parent;                                    /* 0: a root */
    rasm_dvec3                      center;                                    /* the bounding sphere, in world space */
    double                          radius;
    double                          geometric_error;                           /* world units; a leaf's is 0 */
    rasm_tile_refine                refine;
    uint64_t                        estimated_bytes;                           /* 0: the mean of the tiles measured so far */
} rasm_tile_desc;

/* What a tile owns from rasm_tile_streamer_set_tile_content on: the streamer adds the entities to its scene while the
 * tile is drawn, and destroys everything listed when it drops the tile, or at once when the tile does not take it
 * (RASM_ERROR_FAILED). An argument error (a NULL streamer, a size too small, a NULL array with a count) takes nothing:
 * the content stays the caller's. */
typedef struct rasm_tile_content
{
    uint32_t                        size;
    const rasm_entity*              entities;
    uint32_t                        entity_count;
    const rasm_mesh*                meshes;
    uint32_t                        mesh_count;
    const rasm_material*            materials;
    uint32_t                        material_count;
    const rasm_texture*             textures;
    uint32_t                        texture_count;
} rasm_tile_content;

typedef struct rasm_tile_streamer_stats
{
    uint32_t                        size;
    uint32_t                        tiles;
    uint32_t                        wanted;
    uint32_t                        ready;
    uint32_t                        loading;
    uint32_t                        failed;
    uint32_t                        drawn;
    uint32_t                        fallbacks;
    uint32_t                        holes;
    uint32_t                        waiting_for_budget;
    uint32_t                        loads_started;
    uint32_t                        dropped;
    uint64_t                        bytes;
    uint64_t                        peak_bytes;
    uint64_t                        budget;
    uint64_t                        device_budget;
    uint64_t                        device_usage;
    uint64_t                        total_loads;
    uint64_t                        total_drops;
} rasm_tile_streamer_stats;

/* 1.4: a terrain tile, a square of its own frame (x and z across, y up) lifted by its heights. */
typedef struct rasm_terrain_tile_desc
{
    uint32_t                        size;
    rasm_dvec3                      center;                                    /* world space: the square's middle, at height 0 of its frame */
    rasm_quat                       orientation;                               /* the frame's axes in world space */
    double                          tile_size;                                 /* metres across */
    rasm_terrain_material           terrain;
    rasm_texture                    imagery;                                   /* the base color; 0: white */
    rasm_texture_transform          imagery_rect;                              /* where the tile is in the imagery */
    float                           roughness;
} rasm_terrain_tile_desc;

/* 1.4: a tile's height samples on the CPU, width x width, row by row, the same as its heights texture. */
typedef struct rasm_terrain_heights
{
    const uint16_t*                 unorm16;                                   /* R16_UNORM samples, or */
    const float*                    floats;                                    /* R32_SFLOAT samples */
    uint32_t                        width;                                     /* cells + 3 */
} rasm_terrain_heights;

/* 1.5: a mesh's data built on any thread (MeshBuild in C++), which rasm_create_mesh_from_build uploads. */
typedef struct rasm_mesh_build      rasm_mesh_build;

typedef struct rasm_mesh_build_options
{
    uint32_t                        size;
    rasm_bool                       lods;                                      /* simplified LODs; RASM_TRUE by default */
    rasm_bool                       meshlets;                                  /* meshlets for a sub-mesh of 1024 triangles or more; RASM_TRUE by default */
} rasm_mesh_build_options;

/* 1.5: vector features (rasm/vector.h): geometry the add functions append to, on any thread. */
typedef struct rasm_vector_geometry rasm_vector_geometry;

typedef uint32_t                    rasm_line_join;
enum
{
    RASM_LINE_JOIN_MITER            = 0,
    RASM_LINE_JOIN_BEVEL            = 1,
    RASM_LINE_JOIN_ROUND            = 2,
};

typedef uint32_t                    rasm_line_cap;
enum
{
    RASM_LINE_CAP_BUTT              = 0,
    RASM_LINE_CAP_SQUARE            = 1,
    RASM_LINE_CAP_ROUND             = 2,
};

#define RASM_MAX_VECTOR_MITER       2.0f        /* 1.5: half widths */

typedef struct rasm_line_shape
{
    uint32_t                        size;
    rasm_line_join                  join;                                      /* ROUND by default */
    rasm_line_cap                   cap;
    float                           miter_limit;                               /* half widths, 1 to RASM_MAX_VECTOR_MITER */
    float                           drape_width;                               /* 1.9: metres, a draped line's whole width built into its mesh; 0 by default */
} rasm_line_shape;

typedef struct rasm_vector_placement
{
    uint32_t                        size;
    rasm_vec3                       up;                                        /* the ground's up, world space; (0, 1, 0) by default */
    const rasm_terrain_tile_desc*   drape_tile;                                /* NULL: at the points' own heights; else draped on this tile */
    rasm_terrain_heights            drape_heights;
} rasm_vector_placement;

/* 1.6: text and labels (rasm/text.h). A font atlas is a signed distance field of a font's glyphs with their metrics,
 * built on any thread (FontAtlas in C++). */
typedef struct rasm_font_atlas      rasm_font_atlas;

#define RASM_MAX_FONTS              64          /* 1.6: fonts alive at once */
#define RASM_MAX_LABELS             65536       /* 1.6: labels of every entity alive at once */
#define RASM_MAX_LABEL_REACH        12.0f       /* 1.6: ems a label's text may reach from its anchor */

typedef struct rasm_font_load_options
{
    uint32_t                        size;
    float                           em_size;                                   /* atlas pixels per em; 32 by default */
    float                           distance_range;                            /* atlas pixels the stored distance spans; 16 by default */
    const uint32_t*                 codepoints;                                /* NULL: ASCII and Latin-1 */
    uint32_t                        codepoint_count;
} rasm_font_load_options;

typedef uint32_t                    rasm_label_anchor;
enum
{
    RASM_LABEL_ANCHOR_CENTER        = 0,        /* the text's middle on the anchor */
    RASM_LABEL_ANCHOR_BOTTOM        = 1,        /* the text above the anchor */
    RASM_LABEL_ANCHOR_TOP           = 2,        /* the text below the anchor */
    RASM_LABEL_ANCHOR_LEFT          = 3,        /* the text right of the anchor */
    RASM_LABEL_ANCHOR_RIGHT         = 4,        /* the text left of the anchor */
};

typedef struct rasm_label_desc
{
    uint32_t                        size;
    rasm_dvec3                      position;                                  /* the anchor, world space */
    const char*                     text;                                      /* UTF-8; a line feed starts a new line */
    float                           priority;                                  /* collision keeps the higher first */
    rasm_label_anchor               anchor;
    rasm_vec2                       offset;                                    /* ems from the anchor, +y up */
    uint32_t                        feature_id;
} rasm_label_desc;

/* 1.7: picking (rasm/picking.h). A pick asks a view what is under a rect of its pixels; its answer is ready a couple of
 * frames later. */
typedef struct rasm_pick            { uint64_t id; } rasm_pick;

#define RASM_MAX_PICK_SIZE          256         /* 1.7: pixels on each side of a pick's rect */

enum
{
    RASM_PICK_SKIP_MESHES           = 1u << 0,  /* 1.7: opaque and cut-out meshes, not terrain */
    RASM_PICK_SKIP_BLEND            = 1u << 1,  /* see-through meshes, not vector features */
    RASM_PICK_SKIP_TERRAIN          = 1u << 2,
    RASM_PICK_SKIP_VECTORS          = 1u << 3,  /* lines, polygons and points */
    RASM_PICK_SKIP_LABELS           = 1u << 4,
    RASM_PICK_SKIP_OVERLAYS         = 1u << 5,  /* overlay features, labels too */
    RASM_PICK_SKIP_POINTS           = 1u << 6,  /* 1.10: point cloud chunks */
    RASM_PICK_SKIP_ALL              = (1u << 7) - 1,
};

enum
{
    RASM_PICK_HIT_FEATURE           = 1u << 0,  /* 1.7: a vector feature (a label too): feature is its id */
    RASM_PICK_HIT_LABEL             = 1u << 1,
    RASM_PICK_HIT_OVERLAY           = 1u << 2,
    RASM_PICK_HIT_TERRAIN           = 1u << 3,
    RASM_PICK_HIT_BACK_FACE         = 1u << 4,  /* the back of a double-sided surface; the normal faces the camera */
    RASM_PICK_HIT_POINT             = 1u << 5,  /* 1.10: a point of a point cloud chunk: feature is its index */
};

typedef struct rasm_pick_desc
{
    uint32_t                        size;
    rasm_view_pixels                rect;                                      /* the view's pixels from its top-left corner; 1 x 1 at 0, 0 by default */
    float                           min_alpha;                                 /* a see-through surface is picked where its alpha is at least this; 0.5 by default */
    uint32_t                        skip;                                      /* RASM_PICK_SKIP_* */
} rasm_pick_desc;

typedef struct rasm_pick_hit
{
    uint32_t                        size;
    rasm_entity                     entity;                                    /* the entity the frame drew, which may be destroyed since */
    uint32_t                        sub_mesh;
    uint32_t                        feature;                                   /* a vector feature's id (RASM_PICK_HIT_FEATURE), else 0 */
    uint32_t                        flags;                                     /* RASM_PICK_HIT_* */
    uint32_t                        pixels;                                    /* of the rect that show it */
    uint32_t                        x;                                         /* its pixel nearest the rect's centre */
    uint32_t                        y;
    rasm_dvec3                      position;                                  /* world space, at that pixel's centre */
    rasm_vec3                       normal;                                    /* world space */
    double                          distance;                                  /* from the camera */
} rasm_pick_hit;

/* 1.8: what a mouse button's drag does (rasm/cameraInput.h), and a camera's reference grid (rasm/grid.h). */
typedef uint32_t                    rasm_camera_drag;
enum
{
    RASM_CAMERA_DRAG_NONE           = 0,
    RASM_CAMERA_DRAG_ORBIT          = 1,
    RASM_CAMERA_DRAG_PAN            = 2,
};

#define RASM_CAMERA_WHEEL_ZOOM      0.9f        /* 1.8: a wheel step's scale of a trackball's radius or an orthographic box */
#define RASM_MIN_TRACK_BALL_RADIUS  1e-4f       /* 1.8 */

typedef struct rasm_camera_buttons
{
    uint32_t                        size;
    rasm_camera_drag                left;                                      /* ORBIT by default */
    rasm_camera_drag                right;                                     /* PAN */
    rasm_camera_drag                middle;                                    /* PAN */
    rasm_camera_drag                back;                                      /* NONE */
    rasm_camera_drag                forward;                                   /* NONE */
    rasm_camera_drag                shift_left;                                /* PAN: the left button with shift held */
} rasm_camera_buttons;

typedef struct rasm_grid
{
    uint32_t                        size;
    rasm_bool                       enabled;
    rasm_dvec3                      origin;                                    /* world space */
    rasm_quat                       orientation;                               /* the plane is its local XZ plane */
    double                          cell_size;                                 /* the finest level's cells; 1 by default */
    uint32_t                        subdivisions;                              /* 2 to 1000; 10 by default */
    rasm_vec2                       extent;                                    /* half sizes along X and Z; 0: endless */
    rasm_vec4                       minor_color;                               /* linear, with alpha */
    rasm_vec4                       major_color;
    float                           line_width;                                /* pixels */
    float                           major_line_width;
    rasm_bool                       axes;
    rasm_vec4                       x_axis_color;
    rasm_vec4                       z_axis_color;
    float                           fade_distance;                             /* perspective; 0: no fade */
} rasm_grid;

/* 1.10: a chunk of a point cloud, up to RASM_MAX_POINT_CHUNK_POINTS points; positions are offsets from the entity's
 * origin, colors (RGBA8, sRGB-encoded) and scalars may be NULL. */
typedef struct rasm_point_cloud_chunk_desc
{
    uint32_t                        size;
    const rasm_vec3*                positions;
    const uint32_t*                 colors;
    const float*                    scalars;
    uint32_t                        count;
    float                           spacing;                                   /* 1.12: between neighbouring points, for an ADAPTIVE size; 0: measured */
} rasm_point_cloud_chunk_desc;

/* 1.12: a chunk entity's share of its points drawn (a fade), and over each octant of its entity's space (x >= 0: 1,
 * y >= 0: 2, z >= 0: 4) the drawn share of a finer chunk there, which takes an ADAPTIVE size back to the material's. */
typedef struct rasm_point_chunk_lod
{
    uint32_t                        size;
    float                           drawn;                                     /* 1 by default */
    float                           finer[8];                                  /* 0 by default */
} rasm_point_chunk_lod;

/* 1.12: Potree's octree of a point cloud in doubles (rasm_build_point_cloud_octree), its nodes chunks for a tile streamer. */
#define RASM_POINT_NODE_NONE        0xFFFFFFFFu

typedef struct rasm_point_cloud_octree rasm_point_cloud_octree;

typedef struct rasm_point_cloud_octree_desc
{
    uint32_t                        size;
    const rasm_dvec3*               positions;
    const uint32_t*                 colors;                                    /* as a chunk's; NULL: none */
    const float*                    scalars;                                   /* NULL: none */
    uint32_t                        count;
    uint32_t                        cells;                                     /* a node keeps a point of each cell of a cells^3 grid over its cube; 128 by default, a power of two */
    uint32_t                        max_node_points;                           /* a node with no more points left is a leaf with them all; 16384 by default */
} rasm_point_cloud_octree_desc;

typedef struct rasm_point_cloud_node
{
    uint32_t                        size;
    rasm_dvec3                      center;                                    /* its cube's centre: its points are offsets from it */
    double                          half_size;
    rasm_dvec3                      bounds_center;                             /* a sphere around its points and its children's: a tile's */
    double                          bounds_radius;
    float                           spacing;
    float                           geometric_error;                           /* its spacing, 0 for a leaf */
    uint32_t                        parent;                                    /* RASM_POINT_NODE_NONE: the root */
    uint32_t                        children[8];                               /* by octant; RASM_POINT_NODE_NONE: none */
    uint32_t                        first_point;
    uint32_t                        point_count;
    uint32_t                        level;
} rasm_point_cloud_node;

/* 1.12: a LAS or LAZ file's points (rasm_asset_loader_load_point_cloud), with the rasm_c library only. */
typedef struct rasm_point_cloud     rasm_point_cloud;

typedef uint32_t                    rasm_point_cloud_scalar;
enum
{
    RASM_POINT_CLOUD_SCALAR_NONE            = 0,
    RASM_POINT_CLOUD_SCALAR_INTENSITY       = 1,
    RASM_POINT_CLOUD_SCALAR_CLASSIFICATION  = 2,
    RASM_POINT_CLOUD_SCALAR_ELEVATION       = 3,
    RASM_POINT_CLOUD_SCALAR_GPS_TIME        = 4,
    RASM_POINT_CLOUD_SCALAR_RETURN_NUMBER   = 5,
};

typedef struct rasm_point_cloud_load_options
{
    uint32_t                        size;
    rasm_point_cloud_scalar         scalar;                                    /* INTENSITY by default */
    rasm_bool                       z_up_to_y_up;                              /* (x, y, z) to (x, z, -y) */
} rasm_point_cloud_load_options;

/* What a loaded cloud holds; the pointers stay valid until rasm_point_cloud_destroy. */
typedef struct rasm_point_cloud_info
{
    uint32_t                        size;
    const rasm_dvec3*               positions;                                 /* as stored: ECEF, or a projection's metres */
    const uint32_t*                 colors;                                    /* RGBA8 sRGB; NULL for a point format without color */
    const float*                    scalars;                                   /* NULL for SCALAR_NONE */
    uint32_t                        count;
    rasm_dvec3                      bounds_min;
    rasm_dvec3                      bounds_max;
    const char*                     coordinate_system;                         /* OGC WKT, "" without */
    uint32_t                        point_format;
    uint32_t                        version_minor;
    rasm_bool                       compressed;                                /* LAZ */
} rasm_point_cloud_info;

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

/* Added in 1.1: defaults of the new sized structs */
RASM_C_API rasm_result      RASM_CALL rasm_window_desc_init(rasm_window_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_input_event_init(rasm_input_event* event, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_camera_input_init(rasm_camera_input* input, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_camera_controller_init(rasm_camera_controller* controller, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_view_init(rasm_view* view, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_render_target_desc_init(rasm_render_target_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_animation_play_init(rasm_animation_play* play, uint32_t size);

/* Windows */
RASM_C_API rasm_window      RASM_CALL rasm_get_main_window(rasm_renderer* renderer);
RASM_C_API rasm_window      RASM_CALL rasm_add_window(rasm_renderer* renderer, const rasm_window_desc* desc);
RASM_C_API rasm_result      RASM_CALL rasm_remove_window(rasm_renderer* renderer, rasm_window window);
RASM_C_API rasm_result      RASM_CALL rasm_get_window(rasm_renderer* renderer, rasm_window window, rasm_window_info* info);
RASM_C_API rasm_result      RASM_CALL rasm_set_window_size(rasm_renderer* renderer, rasm_window window, uint32_t width, uint32_t height, uint32_t pixel_width, uint32_t pixel_height);
RASM_C_API rasm_result      RASM_CALL rasm_set_window_content_scale(rasm_renderer* renderer, rasm_window window, float scale);
RASM_C_API rasm_readback    RASM_CALL rasm_read_window_frame(rasm_renderer* renderer, rasm_window window, rasm_readback_format format);

/* Input */
RASM_C_API void             RASM_CALL rasm_poll_events(rasm_renderer* renderer);
RASM_C_API rasm_result      RASM_CALL rasm_add_input_event(rasm_renderer* renderer, const rasm_input_event* event);
RASM_C_API uint32_t         RASM_CALL rasm_get_input_event_count(rasm_renderer* renderer);
RASM_C_API rasm_result      RASM_CALL rasm_get_input_event(rasm_renderer* renderer, uint32_t index, rasm_input_event* event);
RASM_C_API rasm_result      RASM_CALL rasm_get_camera_input(rasm_renderer* renderer, rasm_camera_input* input);
RASM_C_API rasm_ui_input_capture RASM_CALL rasm_get_ui_input_capture(rasm_renderer* renderer);
RASM_C_API float            RASM_CALL rasm_get_delta_time(rasm_renderer* renderer);

/* Camera controllers and their input */
RASM_C_API rasm_result      RASM_CALL rasm_get_camera_controller(rasm_renderer* renderer, rasm_camera camera, rasm_camera_controller* controller);
RASM_C_API rasm_result      RASM_CALL rasm_set_camera_controller(rasm_renderer* renderer, rasm_camera camera, const rasm_camera_controller* controller);
RASM_C_API rasm_result      RASM_CALL rasm_update_camera(rasm_renderer* renderer, rasm_camera camera, const rasm_camera_input* input);
RASM_C_API rasm_camera_rig* RASM_CALL rasm_camera_rig_create(const rasm_camera_controller* controller, const rasm_transform* transform);
RASM_C_API void             RASM_CALL rasm_camera_rig_destroy(rasm_camera_rig* rig);
RASM_C_API rasm_result      RASM_CALL rasm_camera_rig_update(rasm_camera_rig* rig, const rasm_camera_input* input);
RASM_C_API rasm_result      RASM_CALL rasm_camera_rig_get_transform(const rasm_camera_rig* rig, rasm_transform* transform, rasm_transform* target_transform);
RASM_C_API rasm_camera_input_state* RASM_CALL rasm_camera_input_state_create(void);
RASM_C_API void             RASM_CALL rasm_camera_input_state_destroy(rasm_camera_input_state* state);
RASM_C_API rasm_result      RASM_CALL rasm_camera_input_state_add_event(rasm_camera_input_state* state, const rasm_input_event* event, const rasm_input_area* area, const rasm_ui_input_capture* capture);
RASM_C_API rasm_result      RASM_CALL rasm_camera_input_state_take(rasm_camera_input_state* state, const rasm_ui_input_capture* capture, float delta_time, rasm_camera_input* input);
RASM_C_API rasm_vec2        RASM_CALL rasm_to_input_ndc(float x, float y, const rasm_input_area* area);
RASM_C_API rasm_input_area  RASM_CALL rasm_get_input_area(rasm_renderer* renderer, rasm_window window, const rasm_view_rect* rect);

/* Views and render targets */
RASM_C_API rasm_result      RASM_CALL rasm_render_views(rasm_renderer* renderer, const rasm_view* views, uint32_t view_count);
RASM_C_API rasm_view_pixels RASM_CALL rasm_get_view_pixels(const rasm_view_rect* rect, uint32_t width, uint32_t height);
RASM_C_API rasm_render_target RASM_CALL rasm_create_render_target(rasm_renderer* renderer, const rasm_render_target_desc* desc);
RASM_C_API rasm_result      RASM_CALL rasm_resize_render_target(rasm_renderer* renderer, rasm_render_target target, uint32_t width, uint32_t height);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_render_target(rasm_renderer* renderer, rasm_render_target target);
RASM_C_API rasm_texture     RASM_CALL rasm_get_render_target_texture(rasm_renderer* renderer, rasm_render_target target);

/* Animation */
RASM_C_API void             RASM_CALL rasm_set_animation_time(rasm_renderer* renderer, double seconds);
RASM_C_API rasm_result      RASM_CALL rasm_play_animation(rasm_renderer* renderer, rasm_entity entity, const rasm_animation_play* play);
RASM_C_API rasm_result      RASM_CALL rasm_stop_animation(rasm_renderer* renderer, rasm_entity entity);
RASM_C_API uint32_t         RASM_CALL rasm_get_animation_count(rasm_renderer* renderer, rasm_mesh mesh);
RASM_C_API uint32_t         RASM_CALL rasm_get_animation_name(rasm_renderer* renderer, rasm_mesh mesh, uint32_t index, char* buffer, uint32_t capacity);
RASM_C_API float            RASM_CALL rasm_get_animation_duration(rasm_renderer* renderer, rasm_mesh mesh, uint32_t index);
RASM_C_API int32_t          RASM_CALL rasm_find_animation(rasm_renderer* renderer, rasm_mesh mesh, const char* name);
RASM_C_API rasm_result      RASM_CALL rasm_set_morph_weights(rasm_renderer* renderer, rasm_entity entity, const float* weights, uint32_t count);
RASM_C_API rasm_result      RASM_CALL rasm_clear_morph_weights(rasm_renderer* renderer, rasm_entity entity);
RASM_C_API uint32_t         RASM_CALL rasm_get_morph_target_count(rasm_renderer* renderer, rasm_mesh mesh);
RASM_C_API uint32_t         RASM_CALL rasm_get_morph_target_name(rasm_renderer* renderer, rasm_mesh mesh, uint32_t index, char* buffer, uint32_t capacity);

/* Stats */
RASM_C_API rasm_result      RASM_CALL rasm_get_frame_stats(rasm_renderer* renderer, rasm_frame_stats* stats);
RASM_C_API rasm_result      RASM_CALL rasm_get_pool_stats(rasm_renderer* renderer, rasm_pool_stats* stats);
RASM_C_API rasm_result      RASM_CALL rasm_get_load_stats(rasm_renderer* renderer, rasm_load_stats* stats);
RASM_C_API rasm_result      RASM_CALL rasm_asset_loader_get_load_stats(rasm_asset_loader* loader, rasm_load_stats* stats);

/* Added in 1.2: positions in doubles. Each view draws in its camera's render space, the world minus the camera's
 * position; rasm_to_render_position and rasm_to_world_position convert between the two (picking, readbacks). */
RASM_C_API rasm_result      RASM_CALL rasm_get_transform_d(rasm_renderer* renderer, rasm_entity entity, rasm_transform_d* transform);
RASM_C_API rasm_result      RASM_CALL rasm_update_transform_d(rasm_renderer* renderer, rasm_entity entity, const rasm_transform_d* transform);
RASM_C_API rasm_result      RASM_CALL rasm_get_camera_transform_d(rasm_renderer* renderer, rasm_camera camera, rasm_transform_d* transform);
RASM_C_API rasm_result      RASM_CALL rasm_set_camera_transform_d(rasm_renderer* renderer, rasm_camera camera, const rasm_transform_d* transform);
RASM_C_API rasm_result      RASM_CALL rasm_get_light_position_d(rasm_renderer* renderer, rasm_light light, rasm_dvec3* position);
RASM_C_API rasm_result      RASM_CALL rasm_set_light_position_d(rasm_renderer* renderer, rasm_light light, const rasm_dvec3* position);
RASM_C_API rasm_camera_rig* RASM_CALL rasm_camera_rig_create_d(const rasm_camera_controller* controller, const rasm_transform_d* transform);
RASM_C_API rasm_result      RASM_CALL rasm_camera_rig_get_transform_d(const rasm_camera_rig* rig, rasm_transform_d* transform, rasm_transform_d* target_transform);
RASM_C_API rasm_result      RASM_CALL rasm_camera_rig_set_free_fly_up(rasm_camera_rig* rig, rasm_vec3 up);
RASM_C_API rasm_vec3        RASM_CALL rasm_to_render_position(rasm_dvec3 world_position, rasm_dvec3 origin);
RASM_C_API rasm_dvec3       RASM_CALL rasm_to_world_position(rasm_vec3 render_position, rasm_dvec3 origin);

/* Added in 1.3: tile streaming. The streamer keeps a scene's tiles in GPU memory under a byte budget: each frame set the
 * views, walk your tree with the screen error, pass the tiles to draw to update, and give each tile it hands out its
 * content. Destroy it before its renderer. The tiles to load stay valid until the next update. */
RASM_C_API rasm_result      RASM_CALL rasm_tile_streamer_config_init(rasm_tile_streamer_config* config, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_tile_desc_init(rasm_tile_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_tile_content_init(rasm_tile_content* content, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_tile_streamer_create(rasm_renderer* renderer, rasm_scene scene, const rasm_tile_streamer_config* config, rasm_tile_streamer** streamer);
RASM_C_API void             RASM_CALL rasm_tile_streamer_destroy(rasm_tile_streamer* streamer);
RASM_C_API rasm_tile        RASM_CALL rasm_tile_streamer_create_tile(rasm_tile_streamer* streamer, const rasm_tile_desc* desc);
RASM_C_API rasm_result      RASM_CALL rasm_tile_streamer_destroy_tile(rasm_tile_streamer* streamer, rasm_tile tile);
RASM_C_API rasm_result      RASM_CALL rasm_tile_streamer_set_views(rasm_tile_streamer* streamer, const rasm_view* views, uint32_t view_count);
RASM_C_API double           RASM_CALL rasm_tile_streamer_get_screen_error(rasm_tile_streamer* streamer, rasm_dvec3 center, double radius, double geometric_error);
RASM_C_API double           RASM_CALL rasm_tile_streamer_get_tile_screen_error(rasm_tile_streamer* streamer, rasm_tile tile);
RASM_C_API rasm_result      RASM_CALL rasm_tile_streamer_update(rasm_tile_streamer* streamer, const rasm_tile* wanted, uint32_t wanted_count);
RASM_C_API const rasm_tile* RASM_CALL rasm_tile_streamer_get_tiles_to_load(rasm_tile_streamer* streamer, uint32_t* count);
RASM_C_API rasm_result      RASM_CALL rasm_tile_streamer_set_tile_content(rasm_tile_streamer* streamer, rasm_tile tile, const rasm_tile_content* content);
RASM_C_API rasm_tile_state  RASM_CALL rasm_tile_streamer_get_tile_state(rasm_tile_streamer* streamer, rasm_tile tile);
RASM_C_API rasm_bool        RASM_CALL rasm_tile_streamer_is_tile_drawn(rasm_tile_streamer* streamer, rasm_tile tile);
RASM_C_API rasm_result      RASM_CALL rasm_tile_streamer_get_stats(rasm_tile_streamer* streamer, rasm_tile_streamer_stats* stats);

/* Added in 1.4: terrain from height map tiles. rasm_create_terrain_tile makes a TERRAIN material and an entity (not in a
 * scene) on the grid every tile of its cells shares (rasm_get_terrain_mesh, which a tile's content does not list). The
 * CPU helpers follow the rule the shader draws: to put objects and cameras on the ground, and to check a picked point. */
RASM_C_API rasm_result      RASM_CALL rasm_terrain_tile_desc_init(rasm_terrain_tile_desc* desc, uint32_t size);
RASM_C_API rasm_mesh        RASM_CALL rasm_get_terrain_mesh(rasm_renderer* renderer, uint32_t cells);
RASM_C_API rasm_result      RASM_CALL rasm_create_terrain_tile(rasm_renderer* renderer, const rasm_terrain_tile_desc* desc, rasm_entity* entity, rasm_material* material);
RASM_C_API rasm_result      RASM_CALL rasm_get_terrain_transform(const rasm_terrain_tile_desc* desc, rasm_transform_d* transform);
RASM_C_API double           RASM_CALL rasm_sample_terrain_height(const rasm_terrain_heights* heights, const rasm_terrain_material* terrain, double u, double v);
RASM_C_API rasm_result      RASM_CALL rasm_get_terrain_position(const rasm_terrain_tile_desc* desc, const rasm_terrain_heights* heights, double u, double v, rasm_dvec3* position);

/* Added in 1.5: meshes built on any thread, and vector features. rasm_build_mesh checks and builds a mesh's data with no
 * renderer, so any thread may call it; rasm_create_mesh_from_build uploads it on the renderer's thread and destroys the
 * build, whatever it returns. The vector geometry functions append lines, polygons and points to a geometry, also on any
 * thread (one thread per geometry); rasm_build_vector_mesh builds its data with no LODs and no meshlets and empties it.
 * Draw the mesh with a RASM_MATERIAL_TYPE_VECTOR material, on an entity at the origin the features were given. */
RASM_C_API rasm_result      RASM_CALL rasm_mesh_build_options_init(rasm_mesh_build_options* options, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_build_mesh(const rasm_vertex* vertices, uint32_t vertex_count, const uint32_t* indices, uint32_t index_count, const uint32_t* colors, const rasm_mesh_build_options* options, rasm_mesh_build** build);
RASM_C_API void             RASM_CALL rasm_mesh_build_destroy(rasm_mesh_build* build);
RASM_C_API rasm_mesh        RASM_CALL rasm_create_mesh_from_build(rasm_renderer* renderer, rasm_mesh_build* build);
RASM_C_API rasm_result      RASM_CALL rasm_line_shape_init(rasm_line_shape* shape, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_vector_placement_init(rasm_vector_placement* placement, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_vector_geometry_create(rasm_vector_geometry** geometry);
RASM_C_API void             RASM_CALL rasm_vector_geometry_destroy(rasm_vector_geometry* geometry);
RASM_C_API rasm_result      RASM_CALL rasm_vector_geometry_get_size(const rasm_vector_geometry* geometry, uint32_t* vertex_count, uint32_t* index_count);
RASM_C_API rasm_result      RASM_CALL rasm_vector_add_lines(rasm_vector_geometry* geometry, rasm_dvec3 origin, const rasm_dvec3* points, const uint32_t* line_sizes, uint32_t line_count, const rasm_line_shape* shape, const rasm_vector_placement* placement, const uint32_t* feature_ids);
RASM_C_API rasm_result      RASM_CALL rasm_vector_add_polygons(rasm_vector_geometry* geometry, rasm_dvec3 origin, const rasm_dvec3* points, const uint32_t* ring_sizes, const uint32_t* ring_counts, uint32_t polygon_count, const rasm_vector_placement* placement, const uint32_t* feature_ids);
RASM_C_API rasm_result      RASM_CALL rasm_vector_add_points(rasm_vector_geometry* geometry, rasm_dvec3 origin, const rasm_dvec3* points, uint32_t point_count, const rasm_vector_placement* placement, const uint32_t* feature_ids, const rasm_vec4* icon_rects);
RASM_C_API rasm_result      RASM_CALL rasm_build_vector_mesh(rasm_vector_geometry* geometry, rasm_mesh_build** build);

/* Added in 1.6: text and labels. rasm_build_font_atlas builds a TrueType or OpenType font's atlas on any thread, and
 * rasm_asset_loader_load_font reads the file first; rasm_create_font uploads an atlas, which the caller keeps for
 * laying labels out. rasm_vector_add_labels lays each label's text out with the atlas (kerning, lines centred, no
 * complex shaping) into the geometry, on any thread; labels is an array of sized structs, each the size of the first.
 * Each view and frame the renderer places the labels: the higher priority first, never overlapping, fading in and out.
 * Draw the mesh with a VECTOR material whose font is the atlas's rasm_create_font. */
RASM_C_API rasm_result      RASM_CALL rasm_font_load_options_init(rasm_font_load_options* options, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_label_desc_init(rasm_label_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_build_font_atlas(const void* data, uint64_t data_size, const rasm_font_load_options* options, rasm_font_atlas** atlas);
RASM_C_API rasm_result      RASM_CALL rasm_asset_loader_load_font(rasm_asset_loader* loader, const char* path, const rasm_font_load_options* options, rasm_font_atlas** atlas);
RASM_C_API void             RASM_CALL rasm_font_atlas_destroy(rasm_font_atlas* atlas);
RASM_C_API rasm_font        RASM_CALL rasm_create_font(rasm_renderer* renderer, const rasm_font_atlas* atlas);
RASM_C_API rasm_result      RASM_CALL rasm_destroy_font(rasm_renderer* renderer, rasm_font font);
RASM_C_API rasm_result      RASM_CALL rasm_vector_add_labels(rasm_vector_geometry* geometry, rasm_dvec3 origin, const rasm_font_atlas* atlas, const rasm_label_desc* labels, uint32_t label_count, const rasm_vector_placement* placement);

/* Added in 1.7: picking. rasm_pick_view asks a view of rasm_render_views (rasm_pick_target: a scene view of the
 * application's own graph) what is under desc->rect of its pixels, as that frame drew it: the pixels' centres, no TAA
 * jitter, one sample per pixel. The next frame's end draws it, and rasm_is_pick_ready says when its answer is in,
 * without waiting. rasm_take_pick writes up to capacity hits, nearest the rect's centre first, into an array whose
 * elements have the size of the first (set hits[0].size, with rasm_pick_hit_init; the others get it); hit_count gets
 * how many there are. A hit names the entity the frame drew, which may be destroyed since: rasm_is_entity_alive tells.
 * rasm_get_view_pixel turns a point of a window (a mouse position) into a pixel of the window view at rect. */
RASM_C_API rasm_result      RASM_CALL rasm_pick_desc_init(rasm_pick_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_pick_hit_init(rasm_pick_hit* hit, uint32_t size);
RASM_C_API rasm_pick        RASM_CALL rasm_pick_view(rasm_renderer* renderer, const rasm_view* view, const rasm_pick_desc* desc);
RASM_C_API rasm_pick        RASM_CALL rasm_pick_target(rasm_renderer* renderer, rasm_scene scene, rasm_camera camera, rasm_render_target target, const rasm_pick_desc* desc);
RASM_C_API rasm_bool        RASM_CALL rasm_is_pick_ready(rasm_renderer* renderer, rasm_pick pick);
RASM_C_API rasm_result      RASM_CALL rasm_take_pick(rasm_renderer* renderer, rasm_pick pick, rasm_pick_hit* hits, uint32_t capacity, uint32_t* hit_count);
RASM_C_API rasm_bool        RASM_CALL rasm_is_entity_alive(rasm_renderer* renderer, rasm_entity entity);
RASM_C_API rasm_bool        RASM_CALL rasm_get_view_pixel(rasm_renderer* renderer, rasm_window window, const rasm_view_rect* rect, float x, float y, uint32_t* pixel_x, uint32_t* pixel_y);

/* Added in 1.8: orthographic controls and reference grids. A camera input state's buttons decide which drag each mouse
 * button starts (rasm_camera_buttons_init: left orbits, right, middle and shift + left pan); rasm_set_camera_buttons sets
 * the renderer's own. A drag follows rasm_input_event's dx and dy (a move without them: its change of position), and
 * rasm_set_mouse_grab hides the pointer during a drag so it goes on past the window's edge (the renderer's own camera
 * input does it by itself). A trackball orbits as a turntable (track_ball_style, around track_ball_up, no roll) or an
 * arcball, from the drag's motion. A trackball pans its center with a pan drag, and its wheel scales the radius by
 * RASM_CAMERA_WHEEL_ZOOM a step, or, for an orthographic projection, the box at the pointer: rasm_update_camera passes the
 * camera's projection, rasm_camera_rig_update_projection takes one and writes it back. rasm_set_camera_grid draws a
 * reference grid in every shaded view of the camera (rasm_grid_init: disabled, the XZ plane through the origin, 1 unit
 * cells in levels of 10). */
RASM_C_API rasm_result      RASM_CALL rasm_camera_buttons_init(rasm_camera_buttons* buttons, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_set_camera_buttons(rasm_renderer* renderer, const rasm_camera_buttons* buttons);
RASM_C_API rasm_result      RASM_CALL rasm_camera_input_state_set_buttons(rasm_camera_input_state* state, const rasm_camera_buttons* buttons);
RASM_C_API rasm_result      RASM_CALL rasm_set_mouse_grab(rasm_renderer* renderer, rasm_window window, rasm_bool on);
RASM_C_API rasm_result      RASM_CALL rasm_camera_rig_update_projection(rasm_camera_rig* rig, rasm_camera_projection* projection, const rasm_camera_input* input);
RASM_C_API rasm_result      RASM_CALL rasm_grid_init(rasm_grid* grid, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_set_camera_grid(rasm_renderer* renderer, rasm_camera camera, const rasm_grid* grid);
RASM_C_API rasm_result      RASM_CALL rasm_get_camera_grid(rasm_renderer* renderer, rasm_camera camera, rasm_grid* grid);

/* Added in 1.10: colormaps and point clouds. rasm_create_colormap makes a 256 x 1 colormap from 1 to RASM_COLORMAP_SIZE
 * linear colors, evenly spread, or from a preset; destroy it with rasm_destroy_texture. rasm_build_point_cloud_chunk
 * checks and builds a chunk's data on any thread (16-bit positions in the chunk's box, then a 16-bit scalar, then the
 * colors: 8 B a point, 12 B with colors) in an order whose every prefix is an even sample, so a view draws only the first
 * points of a chunk small on screen; rasm_create_mesh_from_build uploads it. Draw it with a RASM_MATERIAL_TYPE_POINTS
 * material; a pick's feature is the point's index in desc's arrays. */
RASM_C_API rasm_texture     RASM_CALL rasm_create_colormap(rasm_renderer* renderer, const rasm_vec4* colors, uint32_t count);
RASM_C_API rasm_texture     RASM_CALL rasm_create_colormap_preset(rasm_renderer* renderer, rasm_colormap_preset preset);
RASM_C_API rasm_result      RASM_CALL rasm_point_cloud_chunk_desc_init(rasm_point_cloud_chunk_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_build_point_cloud_chunk(const rasm_point_cloud_chunk_desc* desc, rasm_mesh_build** build);

/* Added in 1.11: small points by compute. With it on (the default, rasm_renderer_config's compute_points), a chunk whose
 * points are 2 pixels across or less is written by a compute pass with 64-bit atomics instead of drawn by triangles, where
 * the GPU has them; the two take the same pixels. Off: every point by triangles. */
RASM_C_API rasm_result      RASM_CALL rasm_set_compute_points(rasm_renderer* renderer, rasm_bool on);
RASM_C_API rasm_bool        RASM_CALL rasm_get_compute_points(rasm_renderer* renderer);

/* Added in 1.12: point cloud streaming. rasm_build_point_cloud_octree builds Potree's octree of points in doubles on any
 * thread: each node a sample of its cube (a point of each grid cell its points take), its children the detail it lacks.
 * Make each node a tile of a rasm_tile_streamer with RASM_TILE_REFINE_ADD and, when it is handed out, a chunk of
 * rasm_get_point_cloud_node_chunk (its arrays are the octree's, which must outlive the build) on an entity at the node's
 * centre. rasm_set_point_chunk_lod fades a chunk in and out by its prefix and lets an ADAPTIVE parent shrink as a finer
 * chunk fills its place. rasm_asset_loader_load_point_cloud reads a LAS file (1.0 to 1.4, point formats 0 to 10) with
 * RASM's own reader, or a LAZ file through LASzip, on the calling thread. A pick's feature on a node's entity is the
 * point's index in its chunk: the octree's indices[first_point + feature] is its index in the desc's arrays. */
RASM_C_API rasm_result      RASM_CALL rasm_point_chunk_lod_init(rasm_point_chunk_lod* lod, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_set_point_chunk_lod(rasm_renderer* renderer, rasm_entity entity, const rasm_point_chunk_lod* lod);
RASM_C_API rasm_result      RASM_CALL rasm_point_cloud_octree_desc_init(rasm_point_cloud_octree_desc* desc, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_build_point_cloud_octree(const rasm_point_cloud_octree_desc* desc, rasm_point_cloud_octree** octree);
RASM_C_API void             RASM_CALL rasm_point_cloud_octree_destroy(rasm_point_cloud_octree* octree);
RASM_C_API uint32_t         RASM_CALL rasm_get_point_cloud_node_count(const rasm_point_cloud_octree* octree);
RASM_C_API rasm_result      RASM_CALL rasm_get_point_cloud_node(const rasm_point_cloud_octree* octree, uint32_t node, rasm_point_cloud_node* record);
RASM_C_API rasm_result      RASM_CALL rasm_get_point_cloud_node_chunk(const rasm_point_cloud_octree* octree, uint32_t node, rasm_point_cloud_chunk_desc* desc);
RASM_C_API const uint32_t*  RASM_CALL rasm_get_point_cloud_octree_indices(const rasm_point_cloud_octree* octree, uint32_t* count);
RASM_C_API rasm_result      RASM_CALL rasm_point_cloud_load_options_init(rasm_point_cloud_load_options* options, uint32_t size);
RASM_C_API rasm_result      RASM_CALL rasm_asset_loader_load_point_cloud(rasm_asset_loader* loader, const char* path, const rasm_point_cloud_load_options* options, rasm_point_cloud** cloud);
RASM_C_API rasm_result      RASM_CALL rasm_get_point_cloud_info(const rasm_point_cloud* cloud, rasm_point_cloud_info* info);
RASM_C_API void             RASM_CALL rasm_point_cloud_destroy(rasm_point_cloud* cloud);

#ifdef __cplusplus
}
#endif
// NOLINTEND(readability-identifier-naming)
// clang-format on
