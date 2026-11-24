#include "rasm/engine.h"
#include "platform/platform.h"

namespace rasm
{
    namespace
    {
        std::unique_ptr<Platform> createPlatform(PlatformType type)
        {
            switch (type)
            {
            case PlatformType::SDL:
                return createSdlPlatform();
            case PlatformType::NATIVE:
                return createNativePlatform();
            case PlatformType::HEADLESS:
                break;
            }
            return createHeadlessPlatform();
        }
    }

    Engine::Engine(const EngineConfig &config) : Renderer(toRendererConfig(config), createPlatform(config.platform)) {}
}
