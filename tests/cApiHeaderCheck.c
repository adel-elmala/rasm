#include "rasm/rasm_c.h"

#include <stdio.h>

/* Built twice, as strict C99 and as C11, against rasm_c_renderer: the header is plain C, and the renderer library has
 * every renderer function. It runs as a test, so its few calls must pass too. */

/* A pass callback as C writes one: it records through the commands it is given. */
static void RASM_CALL recordNothing(void *userData, rasm_commands *commands)
{
    (void)userData;
    rasm_cmd_draw(commands, 3, 1);
}

/* A UI callback: a binding's ImGui calls would go here. */
static void RASM_CALL drawUi(void *userData)
{
    (void)userData;
}

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
    rasm_point_chunk_lod lod;
    rasm_point_cloud_octree_desc octreeDesc;
    rasm_point_cloud_octree *octree = NULL;
    rasm_point_cloud_node node;
    rasm_dvec3 cloud[3] = {{4197000.0, 168000.0, 4786000.0}, {4197001.0, 168000.0, 4786000.0}, {4197000.0, 168001.0, 4786000.5}};
    rasm_glyph_chunk_desc glyphs;
    rasm_quat turned;
    rasm_vec3 down = {0.0f, -1.0f, 0.0f};
    float speeds[2] = {0.0f, 1.0f};
    rasm_export_desc exportDesc;
    rasm_export_progress exportProgress;
    rasm_pass_desc passDesc;
    rasm_pass_resource passResource = {RASM_PASS_ACCESS_WRITE_SWAPCHAIN, 0, 0};
    rasm_pipeline_desc pipelineDesc;
    rasm_buffer_desc bufferDesc;
    rasm_render_graph *graph = NULL;
    rasm_shadow_desc viewShadow;
    rasm_inspection inspection;
    const char *imguiVersion = NULL;
    uint32_t imguiVersionNum = 0;
    rasm_imgui_alloc_func imguiAlloc = NULL;
    rasm_imgui_free_func imguiFree = NULL;
    void *imguiUserData = NULL;
    rasm_texture uiTexture = {0};
    char inspected[16];

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
    material.points.size_mode = RASM_POINT_SIZE_MODE_ADAPTIVE;
    material.points.max_size = 8.0f;
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
    if (rasm_point_chunk_lod_init(&lod, sizeof(lod)) != RASM_OK || lod.drawn != 1.0f || lod.finer[7] != 0.0f || rasm_set_point_chunk_lod(NULL, entity, &lod) != RASM_ERROR_INVALID_ARGUMENT)
        return 22;
    if (rasm_point_cloud_octree_desc_init(&octreeDesc, sizeof(octreeDesc)) != RASM_OK || octreeDesc.cells != 128)
        return 23;
    octreeDesc.positions = cloud;
    octreeDesc.count = 3;
    if (rasm_build_point_cloud_octree(&octreeDesc, &octree) != RASM_OK || rasm_get_point_cloud_node_count(octree) != 1)
        return 24;
    node.size = sizeof(node);
    if (rasm_get_point_cloud_node(octree, 0, &node) != RASM_OK || node.parent != RASM_POINT_NODE_NONE || node.point_count != 3 ||
        rasm_get_point_cloud_node_chunk(octree, 0, &chunk) != RASM_OK || chunk.count != 3)
        return 25;
    rasm_point_cloud_octree_destroy(octree);
    if (rasm_glyph_chunk_desc_init(&glyphs, sizeof(glyphs)) != RASM_OK || glyphs.count != 0 || glyphs.orientations != NULL)
        return 26;
    glyphs.shape = mesh;
    glyphs.positions = points;
    glyphs.count = 2;
    if (rasm_build_glyph_chunk(&glyphs, &build) != RASM_ERROR_INVALID_ARGUMENT || build != NULL)
        return 27;
    turned = rasm_get_glyph_orientation(down);
    material.glyphs.color_mode = RASM_POINT_COLOR_MODE_SCALAR;
    material.vector_colors.range.clamp = RASM_TRUE;
    material.blend_method = RASM_BLEND_METHOD_WEIGHTED;
    if (turned.x != 1.0f || (RASM_PICK_SKIP_ALL & RASM_PICK_SKIP_GLYPHS) == 0 || RASM_MATERIAL_TYPE_GLYPHS != 5 || material.glyphs.color_mode != RASM_POINT_COLOR_MODE_SCALAR ||
        material.blend_method != RASM_BLEND_METHOD_WEIGHTED || RASM_BLEND_METHOD_ADDITIVE != 2)
        return 28;
    if (rasm_vector_geometry_create(&geometry) != RASM_OK || rasm_vector_add_data_lines(geometry, line[0], line, &lineSize, 1, NULL, NULL, NULL, NULL, speeds) != RASM_OK)
        return 29;
    rasm_vector_geometry_destroy(geometry);
    if (rasm_export_desc_init(&exportDesc, sizeof(exportDesc)) != RASM_OK || exportDesc.samples != 16 || exportDesc.tone_map != RASM_TRUE || exportDesc.format != RASM_EXPORT_FORMAT_PNG8 ||
        rasm_export_progress_init(&exportProgress, sizeof(exportProgress)) != RASM_OK || rasm_export_view(NULL, &views[0], &exportDesc).id != 0 ||
        rasm_get_export_progress(NULL, rasm_export_view(NULL, &views[0], &exportDesc), &exportProgress) != RASM_ERROR_INVALID_ARGUMENT || RASM_EXPORT_STATE_FAILED != 2)
        return 30;
    if (rasm_pass_desc_init(&passDesc, sizeof(passDesc)) != RASM_OK || rasm_pipeline_desc_init(&pipelineDesc, sizeof(pipelineDesc)) != RASM_OK ||
        rasm_buffer_desc_init(&bufferDesc, sizeof(bufferDesc)) != RASM_OK || pipelineDesc.depth_test != RASM_TRUE || bufferDesc.usage != RASM_BUFFER_USAGE_STORAGE)
        return 31;
    passDesc.execute = recordNothing;
    passDesc.resources = &passResource;
    passDesc.resource_count = 1;
    if (rasm_render_graph_create(NULL, &graph) != RASM_ERROR_INVALID_ARGUMENT || graph != NULL || rasm_render_graph_add_pass(graph, &passDesc) != RASM_ERROR_INVALID_ARGUMENT ||
        rasm_cmd_draw(NULL, 3, 1) != RASM_ERROR_INVALID_ARGUMENT || RASM_PUSH_CONSTANTS_SIZE != 16 || sizeof(rasm_pass_resource) != 16)
        return 32;
    if (rasm_shadow_desc_init(&viewShadow, sizeof(viewShadow)) != RASM_OK || rasm_view_init(&views[0], sizeof(views[0])) != RASM_OK || views[0].shadow_count != 0 || views[0].shadows != NULL)
        return 33;
    views[0].shadows = &viewShadow;
    views[0].shadow_count = 1;
    if (rasm_render_views(NULL, views, 1) != RASM_ERROR_INVALID_ARGUMENT || rasm_set_debug_view(NULL, RASM_DEBUG_VIEW_NORMALS) != RASM_ERROR_INVALID_ARGUMENT ||
        rasm_get_debug_view(NULL) != RASM_DEBUG_VIEW_NONE || rasm_set_culling_frozen(NULL, RASM_TRUE) != RASM_ERROR_INVALID_ARGUMENT || rasm_get_meshlet_path(NULL) != RASM_MESHLET_PATH_AUTO)
        return 34;
    if (rasm_inspection_init(&inspection, sizeof(inspection)) != RASM_OK || inspection.exposure != 1.0f || inspection.target != NULL || rasm_set_inspection(NULL, &inspection) != RASM_ERROR_INVALID_ARGUMENT ||
        rasm_get_inspection(NULL, &inspection, inspected, sizeof(inspected)) != 0 || RASM_MAX_VIEW_SHADOWS != 4 || RASM_INSPECT_KIND_LIGHT_CLUSTERS != 3)
        return 35;
    if (rasm_set_ui_callback(NULL, drawUi, NULL) != RASM_ERROR_INVALID_ARGUMENT || rasm_get_imgui_context(NULL) != NULL || rasm_get_ui_texture(NULL, uiTexture) != 0 ||
        rasm_check_imgui_layout(NULL, 0, 0, 0, 0, 0, 0) != RASM_ERROR_INVALID_ARGUMENT)
        return 36;
    /* With Dear ImGui (RASM_IMGUI) the library has a version and allocators; without, neither. */
    imguiVersion = rasm_get_imgui_version(&imguiVersionNum);
    if (rasm_get_imgui_allocator_functions(&imguiAlloc, &imguiFree, &imguiUserData) != (imguiVersion != NULL ? RASM_OK : RASM_ERROR_FAILED) || (imguiVersion != NULL) != (imguiAlloc != NULL) ||
        (imguiVersion != NULL) != (imguiVersionNum != 0))
        return 37;
    printf("rasm_c.h %u.%u compiles as C (__STDC_VERSION__ %ld) and links rasm_c_renderer.\n", RASM_C_API_VERSION_MAJOR, RASM_C_API_VERSION_MINOR, (long)__STDC_VERSION__);
    return 0;
}
