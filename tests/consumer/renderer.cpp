#include "rasm/renderer.h"

// Builds and links against rasm::renderer alone: no SDL3, tinyobjloader or libktx in the consumer's build.
int main(int argc, char **)
{
    const rasm::RendererConfig config = {.appName = "consumer", .windowWidth = 64, .windowHeight = 64};
    if (argc > 99)
    {
        rasm::Renderer renderer(config);
        return renderer.running() ? 0 : 1;
    }
    return rasm::getViewPixels({}, 4, 4).width == 4 ? 0 : 1;
}
