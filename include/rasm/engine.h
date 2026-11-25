// clang-format off
#pragma once

#include "rasm/assetLoader.h"
#include "rasm/renderer.h"

#include <string>

namespace rasm
{

    // The all-in-one class: a Renderer whose windows come from EngineConfig::platform (SDL opens them and reads their
    // events itself; NATIVE draws into the application's windows and HEADLESS into offscreen images, as a Renderer
    // does), with an AssetLoader of its own for files. It overrides nothing of the Renderer: the load calls and
    // getConfig below are its own, and reach its loader.
    class Engine final : public Renderer
    {
    public:
        explicit                    Engine(const EngineConfig& config);
        MeshHandle                  loadMesh(const std::string& path);
        MeshHandle                  loadGLTF(const std::string& path);
        TextureHandle               loadTexture(const std::string& path, Format format = Format::R8G8B8A8_SRGB);
        EnvironmentHandle           loadEnvironment(const std::string& path);
        bool                        loadFont(const std::string& path, FontAtlas& atlas, const FontLoadOptions& options = {});
        std::string                 resolveAssetPath(const std::string& path) const;
        // The config with the paths resolved and the values the frame changes (F1's showStats, F2's forcedLod).
        EngineConfig                getConfig() const;
        AssetLoader&                getAssetLoader();

    private:
        PlatformType                platformType;
        AssetLoader                 loader;
    };

}
// clang-format on
