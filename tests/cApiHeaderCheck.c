#include "rasm/rasm_c.h"

#include <stdio.h>

/* Built twice, as strict C99 and as C11, against rasm_c_renderer: the header is plain C, and the renderer library has
 * every renderer function. It runs as a test, so its few calls must pass too. */

int main(void)
{
    rasm_renderer_config config;
    rasm_texture_desc texture;
    rasm_light_desc light;
    rasm_scene_environment environment;
    rasm_mesh mesh = {0};
    rasm_image *image;
    rasm_image_data data;
    char base[8];
    rasm_view views[2];
    rasm_view_rect rect = {0.0f, 0.0f, 0.5f, 1.0f};
    rasm_view_pixels pixels;
    rasm_dvec3 origin = {4197000.25, 168000.5, 4786000.75};
    rasm_vec3 offset = {2.5f, 0.0f, 0.0f};
    rasm_dvec3 world;
    rasm_transform_d pose;
    rasm_entity entity = {0};
    rasm_tile_desc tileDesc;
    rasm_tile_streamer_config streamerConfig;
    rasm_tile_streamer *streamer = NULL;
    rasm_scene scene = {0};
    rasm_terrain_tile_desc terrainDesc;
    rasm_terrain_heights heights = {NULL, NULL, 0};
    float samples[16] = {0.0f};
    rasm_vector_geometry *geometry = NULL;
    rasm_mesh_build *build = NULL;
    rasm_dvec3 line[2] = {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}};
    uint32_t lineSize = 2;
    rasm_label_desc label;
    rasm_pick_desc pickDesc;
    rasm_pick_hit pickHit;
    rasm_grid grid;
    rasm_camera_buttons buttons;
    rasm_camera camera = {0};
    rasm_point_cloud_chunk_desc chunk;
    rasm_vec3 points[2] = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    rasm_material_desc material;

    if (rasm_get_version() != RASM_C_API_VERSION)
        return 1;
    if (rasm_renderer_config_init(&config, sizeof(config)) != RASM_OK || config.size != sizeof(config) || config.occlusion_culling != RASM_TRUE)
        return 2;
    if (rasm_texture_desc_init(&texture, sizeof(texture)) != RASM_OK || rasm_light_desc_init(&light, sizeof(light)) != RASM_OK ||
        rasm_scene_environment_init(&environment, sizeof(environment)) != RASM_OK)
        return 3;
    if (rasm_is_mesh_loaded(NULL, mesh) != RASM_FALSE || rasm_get_last_error()[0] == '\0')
        return 4;
    image = rasm_image_create();
    data = rasm_image_get(image);
    rasm_image_destroy(image);
    if (data.pixels != NULL || data.byte_size != 0)
        return 5;
    if (rasm_get_base_path(base, sizeof(base)) == 0)
        return 6;
    if (rasm_view_init(&views[0], sizeof(views[0])) != RASM_OK || views[0].rect.width != 1.0f)
        return 7;
    views[1] = views[0];
    pixels = rasm_get_view_pixels(&rect, 100, 10);
    if (pixels.width != 50 || rasm_render_views(NULL, views, 2) != RASM_ERROR_INVALID_ARGUMENT)
        return 8;
    world = rasm_to_world_position(offset, origin);
    if (world.x != origin.x + 2.5 || rasm_to_render_position(world, origin).x != 2.5f)
        return 9;
    if (rasm_get_transform_d(NULL, entity, &pose) != RASM_ERROR_INVALID_ARGUMENT)
        return 10;
    if (rasm_tile_desc_init(&tileDesc, sizeof(tileDesc)) != RASM_OK || tileDesc.refine != RASM_TILE_REFINE_REPLACE ||
        rasm_tile_streamer_config_init(&streamerConfig, sizeof(streamerConfig)) != RASM_OK || streamerConfig.max_loads_per_frame == 0)
        return 11;
    if (rasm_tile_streamer_create(NULL, scene, &streamerConfig, &streamer) != RASM_ERROR_INVALID_ARGUMENT || streamer != NULL)
        return 12;
    samples[5] = 3.0f;
    heights.floats = samples;
    heights.width = 4;
    if (rasm_terrain_tile_desc_init(&terrainDesc, sizeof(terrainDesc)) != RASM_OK || terrainDesc.terrain.height_scale != 1.0f ||
        rasm_sample_terrain_height(&heights, &terrainDesc.terrain, 0.0, 0.0) != 3.0)
        return 13;
    if (rasm_vector_geometry_create(&geometry) != RASM_OK || rasm_vector_add_lines(geometry, line[0], line, &lineSize, 1, NULL, NULL, NULL) != RASM_OK ||
        rasm_build_vector_mesh(geometry, &build) != RASM_OK)
        return 14;
    rasm_mesh_build_destroy(build);
    if (rasm_label_desc_init(&label, sizeof(label)) != RASM_OK || label.size != sizeof(label) || label.anchor != RASM_LABEL_ANCHOR_CENTER ||
        rasm_vector_add_labels(geometry, line[0], NULL, &label, 1, NULL) != RASM_ERROR_INVALID_ARGUMENT)
        return 15;
    rasm_vector_geometry_destroy(geometry);
    if (rasm_pick_desc_init(&pickDesc, sizeof(pickDesc)) != RASM_OK || pickDesc.rect.width != 1 || pickDesc.min_alpha != 0.5f ||
        rasm_pick_hit_init(&pickHit, sizeof(pickHit)) != RASM_OK || rasm_is_pick_ready(NULL, rasm_pick_view(NULL, &views[0], &pickDesc)) != RASM_FALSE)
        return 16;
    if (rasm_grid_init(&grid, sizeof(grid)) != RASM_OK || grid.subdivisions != 10 || rasm_camera_buttons_init(&buttons, sizeof(buttons)) != RASM_OK ||
        buttons.right != RASM_CAMERA_DRAG_PAN || rasm_set_camera_grid(NULL, camera, &grid) != RASM_ERROR_INVALID_ARGUMENT)
        return 17;
    material.points.shape = RASM_POINT_SHAPE_SPHERE;
    material.points.range.log = RASM_FALSE;
    if (rasm_point_cloud_chunk_desc_init(&chunk, sizeof(chunk)) != RASM_OK || chunk.count != 0)
        return 18;
    chunk.positions = points;
    chunk.count = 2;
    if (rasm_build_point_cloud_chunk(&chunk, &build) != RASM_OK || build == NULL || material.points.shape != RASM_POINT_SHAPE_SPHERE)
        return 19;
    rasm_mesh_build_destroy(build);
    if (rasm_create_colormap_preset(NULL, RASM_COLORMAP_VIRIDIS).id != 0 || (RASM_PICK_SKIP_ALL & RASM_PICK_SKIP_POINTS) == 0)
        return 20;
    if (rasm_set_compute_points(NULL, RASM_FALSE) != RASM_ERROR_INVALID_ARGUMENT || rasm_get_compute_points(NULL) != RASM_FALSE)
        return 21;
    printf("rasm_c.h %u.%u compiles as C (__STDC_VERSION__ %ld) and links rasm_c_renderer.\n", RASM_C_API_VERSION_MAJOR, RASM_C_API_VERSION_MINOR, (long)__STDC_VERSION__);
    return 0;
}
