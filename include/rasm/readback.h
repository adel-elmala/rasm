// clang-format off
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rasm
{

    // What Engine::takeReadback converts the copied texels to. RGBA8_SRGB: 4 bytes per pixel, sRGB-encoded (8-bit sources
    // as they are, float sources clamped to 0..1 and encoded), ready for writePng. RGBA32_FLOAT: 16 bytes per pixel,
    // linear (sRGB sources decoded), for HDR values; a depth source gives its depth in red, and 0, 0, 1 in the others.
    enum class ReadbackFormat
    {
        RGBA8_SRGB,
        RGBA32_FLOAT,
    };

    struct ReadbackImage
    {
        uint32_t                    width       = 0;
        uint32_t                    height      = 0;
        ReadbackFormat              format      = ReadbackFormat::RGBA8_SRGB;
        std::vector<uint8_t>        pixels;                                     // rows top to bottom, tightly packed
    };

    // Writes an RGBA8_SRGB image as a PNG at a UTF-8 path. Returns false for a float image or when the file can't be written.
    bool                            writePng(const std::string& path, const ReadbackImage& image);

}
// clang-format on
