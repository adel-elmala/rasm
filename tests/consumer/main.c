#define _CRT_SECURE_NO_WARNINGS
#include "rasm/rasm_c.h"

#include <stdio.h>
#include <string.h>

/* Runs from the install: the library loads, its version is the header's, and its shaders are where a relative
 * shader_dir looks for them, beside the library. */
int main(void)
{
    char base[1024];
    char path[1100];
    rasm_renderer_config config;
    FILE *file;

    if (rasm_get_version() != RASM_C_API_VERSION)
    {
        fprintf(stderr, "library version %u, header %u\n", rasm_get_version(), RASM_C_API_VERSION);
        return 1;
    }
    if (rasm_renderer_config_init(&config, sizeof(config)) != RASM_OK)
        return 1;
    if (rasm_get_base_path(base, sizeof(base)) >= sizeof(base))
        return 1;
    snprintf(path, sizeof(path), "%s/shaders/lib/core.slang", base);
    file = fopen(path, "rb");
    if (file == NULL)
    {
        fprintf(stderr, "no shaders beside the library: %s\n", path);
        return 1;
    }
    fclose(file);
    printf("rasm_c %u.%u from %s\n", rasm_get_version() >> 16, rasm_get_version() & 0xFFFFu, base);
    return 0;
}
