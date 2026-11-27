// clang-format off
#pragma once

#include "rasm/handle.h"

#include <cstdint>
#include <string>

namespace rasm
{

    // An export's tiles are at most this many pixels on a side (the device's largest image may lower it).
    constexpr uint32_t              DEFAULT_EXPORT_TILE_SIZE    = 2048;
    constexpr uint32_t              MAX_EXPORT_SAMPLES          = 256;

    // What an export writes. A PNG is sRGB with straight alpha; an EXR is linear with premultiplied alpha (channels A,
    // B, G, R, ZIP compressed). Without a path an export keeps its image in memory (Renderer::takeExport): PNG8 as an
    // RGBA8_SRGB ReadbackImage, the others as RGBA32_FLOAT, linear and premultiplied.
    enum class ExportFormat
    {
        PNG8,
        PNG16,
        EXR_HALF,
        EXR_FLOAT,
    };

    enum class ExportState
    {
        RUNNING,                                                // waiting for the scene to settle, or drawing its tiles
        DONE,
        FAILED,
    };

    // A still of a view at any size, drawn in tiles over the frames of Renderer::render and written as it goes.
    struct ExportDesc
    {
        uint32_t                    width           = 0;
        uint32_t                    height          = 0;
        std::string                 path;                       // UTF-8; empty: kept in memory for takeExport
        ExportFormat                format          = ExportFormat::PNG8;
        uint32_t                    samples         = 16;       // jittered frames averaged per pixel, 1 to MAX_EXPORT_SAMPLES (a square count is a stratified grid)
        float                       pixelScale      = 1.0f;     // output pixels per pixel of every size given in pixels (lines, text, points, the grid) and of the LOD error
        bool                        toneMap         = true;     // false: the scene's linear HDR color (exposure and bloom, no AgX), for an EXR only
        bool                        transparent     = false;    // no sky or background: alpha is what covers each pixel
        bool                        msaa            = false;    // also the view's MSAA in each sample (the samples already smooth edges)
        uint32_t                    tileSize        = 0;        // 0: DEFAULT_EXPORT_TILE_SIZE; smaller tiles use less memory and cost more guard band
    };

    struct ExportProgress
    {
        ExportState                 state           = ExportState::FAILED;
        uint32_t                    tilesDone       = 0;        // written to the file, or into the image
        uint32_t                    tileCount       = 0;
        float                       fraction        = 0.0f;     // of the export's frames, 0 to 1
    };

}
// clang-format on
