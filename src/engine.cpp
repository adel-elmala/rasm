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

    // The loader is a member, so it is made after the renderer and destroyed before it, as an AssetLoader must be.
    Engine::Engine(const EngineConfig &config) : Renderer(toRendererConfig(config), createPlatform(config.platform)), platformType(config.platform), loader(*this, toAssetLoaderConfig(config)) {}

    MeshHandle Engine::loadMesh(const std::string &path)
    {
        return loader.loadMesh(path);
    }

    MeshHandle Engine::loadGLTF(const std::string &path)
    {
        return loader.loadGLTF(path);
    }

    TextureHandle Engine::loadTexture(const std::string &path, Format format)
    {
        return loader.loadTexture(path, format);
    }

    bool Engine::loadFont(const std::string &path, FontAtlas &atlas, const FontLoadOptions &options)
    {
        return loader.loadFont(path, atlas, options);
    }

    EnvironmentHandle Engine::loadEnvironment(const std::string &path)
    {
        return loader.loadEnvironment(path);
    }

    std::string Engine::resolveAssetPath(const std::string &path) const
    {
        return loader.resolveAssetPath(path);
    }

    EngineConfig Engine::getConfig() const
    {
        return toEngineConfig(Renderer::getConfig(), platformType, loader.getConfig());
    }

    AssetLoader &Engine::getAssetLoader()
    {
        return loader;
    }
}
