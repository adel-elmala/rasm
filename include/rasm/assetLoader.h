// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rasm
{

    class Renderer;

    // Loads files into a Renderer on worker threads: glTF and OBJ meshes with their materials and textures, PNG, JPEG
    // and KTX2 textures, HDR environments. A load returns a handle at once; the renderer finishes the ready loads at the
    // start of each frame, within a budget, and Renderer::isLoaded says when one is in. The renderer must outlive its
    // loader: destroy the loader first (an Engine does). A renderer destroyed first logs an error and drops the loads,
    // and the loader refuses every call after that. One loader per renderer.
    class AssetLoader
    {
    public:
                                    AssetLoader(Renderer& renderer, const AssetLoaderConfig& config = {});
                                    ~AssetLoader();
                                    AssetLoader(const AssetLoader&) = delete;
        AssetLoader&                operator=(const AssetLoader&) = delete;
        // OBJ, glTF or GLB by the file's extension. A file loaded already returns its mesh; one that failed loads again.
        MeshHandle                  loadMesh(const std::string& path);
        MeshHandle                  loadGLTF(const std::string& path);
        // format names the slot: R8G8B8A8_SRGB for color (base color, emissive), R8G8B8A8_UNORM for data (normal,
        // metallic-roughness, occlusion). A .ktx2 file gets the best GPU format the device has for that slot.
        TextureHandle               loadTexture(const std::string& path, Format format = Format::R8G8B8A8_SRGB);
        EnvironmentHandle           loadEnvironment(const std::string& path);
        // Blocks until every load is in, its copies included.
        void                        waitForLoads();
        LoadStats                   getLoadStats() const;
        // The path a load opens: UTF-8, against assetRoot unless absolute.
        std::string                 resolveAssetPath(const std::string& path) const;
        // assetRoot resolved.
        AssetLoaderConfig           getConfig() const;

        struct Impl;

    private:
        std::unique_ptr<Impl>       impl;
    };

}
// clang-format on
