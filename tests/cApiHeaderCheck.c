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
    printf("rasm_c.h %u.%u compiles as C (__STDC_VERSION__ %ld) and links rasm_c_renderer.\n", RASM_C_API_VERSION_MAJOR, RASM_C_API_VERSION_MINOR, (long)__STDC_VERSION__);
    return 0;
}
