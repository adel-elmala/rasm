// clang-format off
#pragma once

#include "rasm/mesh.h"
#include "assets/ktxImage.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rasm
{
    struct GltfImageTarget
    {
        Format                          format      = Format::UNKNOWN;
        bool                            normals     = false;    // a normal map's levels are renormalized
        std::shared_ptr<TextureRaw>     levels;                 // also in GLTFRaw::ktxImages or imageLevels; empty when it failed
    };

    // One glTF image's job: its file bytes are decoded once (PNG, JPEG) or read as KTX2, and each target gets its levels.
    struct GltfImageWork
    {
        int                             source      = -1;       // the glTF image
        bool                            ktx         = false;
        std::string                     name;
        uint64_t                        bytes       = 0;        // the levels it makes, from the image's header
        uint64_t                        fileBytes   = 0;        // the image's bytes in the file, kept until the model's jobs end
        std::vector<GltfImageTarget>    targets;
    };

    GLTFRaw     importMeshFile(const std::string& path, uint64_t supportedTextureFormats);
    GLTFRaw     parseMeshFile(const std::string& path, uint64_t supportedTextureFormats, std::vector<GltfImageWork>& images);
    void        runImageWork(const tinygltf::Model& model, GltfImageWork& work);
    void        finishImageWork(GLTFRaw& raw);
    int         getBasisuSource(const tinygltf::Model& model, int textureIndex);
    uint64_t    getKtxSlotKey(int image, KtxSlot slot);
    uint64_t    getKtxImageKey(int image, Format format);
    uint64_t    getImageLevelsKey(int image, KtxSlot slot);
}
// clang-format on
