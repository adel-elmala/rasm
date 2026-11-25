// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/text.h"
#include "rasm/types.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rasm
{

    class Renderer;

    // How a font file becomes a distance atlas (buildFontAtlas, AssetLoader::loadFont).
    struct FontLoadOptions
    {
        float                       emSize          = 32.0f;            // the atlas's pixels per em: larger is sharper for big text, and a larger atlas
        float                       distanceRange   = 16.0f;            // atlas pixels the stored distance spans (FontAtlas::distanceRange): a halo reaches half of it
        const uint32_t*             codepoints      = nullptr;          // the characters to hold; null: ASCII and Latin-1 (32 to 126, 160 to 255)
        uint32_t                    codepointCount  = 0;
    };

    enum class PointCloudScalar : uint32_t
    {
        NONE,
        INTENSITY,
        CLASSIFICATION,
        ELEVATION,      // its height: z, or y with zUpToYUp
        GPS_TIME,
        RETURN_NUMBER,
    };

    struct PointCloudLoadOptions
    {
        PointCloudScalar            scalar          = PointCloudScalar::INTENSITY;
        bool                        zUpToYUp        = false;            // (x, y, z) to (x, z, -y): a projected file's east, north and up to RASM's east, up and south
    };

    struct PointCloudData
    {
        std::vector<glm::dvec3>     positions;                          // the file's coordinates scaled and offset, as stored: ECEF, or a projection's metres
        std::vector<uint32_t>       colors;                             // RGBA8 sRGB, alpha 255 (packVertexColor); empty for a point format without color
        std::vector<float>          scalars;                            // PointCloudLoadOptions::scalar; empty for NONE
        glm::dvec3                  boundsMin       = glm::dvec3(0.0);
        glm::dvec3                  boundsMax       = glm::dvec3(0.0);
        std::string                 coordinateSystem;                   // the OGC WKT of its variable length records, empty without
        uint32_t                    pointFormat     = 0;                // 0 to 10
        uint32_t                    versionMinor    = 0;                // LAS 1.x
        bool                        compressed      = false;            // LAZ
    };

    // A TrueType or OpenType font's signed distance field atlas, with stb_truetype: each codepoint's glyph, its metrics
    // and the kerning between every pair the atlas holds. Safe on any thread. Complex scripts (shaping, right-to-left)
    // are not laid out. stb_truetype trusts the font's tables: load only font files you trust.
    bool                            buildFontAtlas(const uint8_t* data, size_t size, const FontLoadOptions& options, FontAtlas& atlas);

    // Loads files into a Renderer on worker threads: glTF and OBJ meshes with their materials and textures, PNG, JPEG
    // and KTX2 textures, HDR environments. A load returns a handle at once; the renderer finishes the ready loads at the
    // start of each frame, within a budget, and Renderer::isLoaded says when one is in. The renderer must outlive its
    // loader: destroy the loader first (an Engine does). A renderer destroyed first logs an error and drops the loads,
    // and the loader refuses every call after that. A loader destroyed while its renderer lives fails the meshes and
    // environments still loading and destroys the textures still loading. One loader per renderer at a time.
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
        // Reads a font file and builds its atlas (buildFontAtlas), on this thread: upload it with Renderer::createFont,
        // and lay labels out with it (addVectorLabels). False with an error when the file cannot be read or is no font.
        bool                        loadFont(const std::string& path, FontAtlas& atlas, const FontLoadOptions& options = {});
        bool                        loadPointCloud(const std::string& path, PointCloudData& cloud, const PointCloudLoadOptions& options = {});
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
