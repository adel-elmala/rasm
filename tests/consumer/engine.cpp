#include "rasm/rasm.h"

// Builds and links against rasm::rasm: the Engine, its SDL platform and its AssetLoader.
int main(int argc, char **)
{
    if (argc > 99)
    {
        rasm::Engine engine({.appName = "consumer", .platform = rasm::PlatformType::HEADLESS, .windowWidth = 64, .windowHeight = 64});
        return engine.loadMesh("missing.obj").isValid() ? 0 : 1;
    }
    return rasm::getViewPixels({}, 4, 4).width == 4 ? 0 : 1;
}
