// clang-format off
#pragma once

#include "rasm/export.h"
#include "rasm/handle.h"
#include "rasm/readback.h"
#include "rasm/types.h"
#include "rasm/view.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace rasm
{

    class ImageWriter;

    // Tile origins, the guard band and the render rect are multiples of this: the bloom's 6 levels halve every tile
    // into whole texels on the same grid as the whole image's.
    constexpr uint32_t              EXPORT_ALIGNMENT        = 64;
    // The guard band around a tile, in pixels: without bloom, for what reads a few pixels around its own (the point
    // hole fill, the label probes' 3 x 3); with it, the bloom's reach, 189 pixels down its chain and 186 back up.
    constexpr uint32_t              EXPORT_GUARD            = 64;
    constexpr uint32_t              EXPORT_BLOOM_GUARD      = 384;
    // Frames an export waits at most for the scene's loads and tile streamers to settle before it starts its tiles.
    constexpr uint32_t              EXPORT_SETTLE_FRAMES    = 600;
    // Bands (rows of tiles) waiting for the writer at most: past it the tiles wait, so memory holds at most three bands
    // (one written, one waiting, one filling).
    constexpr uint32_t              EXPORT_MAX_QUEUED_BANDS = 1;

    // How an image is cut. The image is rounded up to EXPORT_ALIGNMENT (the virtual image, whose extra pixels are
    // drawn and dropped) and cut in tiles of tileSize; each tile is drawn as a render rect of renderWidth x renderHeight,
    // the tile and at least guard pixels around it, inside the virtual image.
    struct ExportLayout
    {
        uint32_t                    width                   = 0;
        uint32_t                    height                  = 0;
        uint32_t                    virtualWidth            = 0;
        uint32_t                    virtualHeight           = 0;
        uint32_t                    tileSize                = 0;
        uint32_t                    guard                   = 0;
        uint32_t                    columns                 = 0;
        uint32_t                    rows                    = 0;
        uint32_t                    renderWidth             = 0;
        uint32_t                    renderHeight            = 0;
    };

    // One tile: the pixels it keeps (inside the image) and the corner of the rect it is drawn in, in image pixels.
    struct ExportTile
    {
        uint32_t                    x                       = 0;
        uint32_t                    y                       = 0;
        uint32_t                    width                   = 0;
        uint32_t                    height                  = 0;
        uint32_t                    renderX                 = 0;
        uint32_t                    renderY                 = 0;
    };

    // False when the image is empty, or the device's largest image cannot hold a tile and its guard.
    bool                            getExportLayout(uint32_t width, uint32_t height, uint32_t tileSize, uint32_t guard, uint32_t maxImageSize, ExportLayout& layout);
    // Tiles go row by row from the top-left one.
    ExportTile                      getExportTile(const ExportLayout& layout, uint32_t tile);
    // Moves the whole image's clip space to the tile's render rect: clip' = M * clip, for any projection.
    glm::dmat4                      getExportTileClip(const ExportLayout& layout, const ExportTile& tile);
    // Sample i's offset in pixels, in (-0.5, 0.5): the centres of a stratified grid for a square count, else Halton (2, 3).
    glm::vec2                       getExportJitter(uint32_t sample, uint32_t samples);
    // Bytes a pixel takes in the format, as the Export pass writes it (EXPORT_OUTPUT_*, by ExportFormat or in memory).
    uint32_t                        getExportPixelBytes(ExportFormat format);

    enum class ExportPhase
    {
        SETTLE,                                             // the view's first frames, the scene's loads and its streamers
        SURVEY,                                             // one frame a tile: the label fades and the weighted span over the whole image
        DRAIN,                                              // the survey's last answers come back
        TILES,                                              // every tile, samples frames each
        FLUSH,                                              // the last tiles come back and the writer finishes
        FINISHED,
    };

    // What the export's scene view draws in this frame (prepareSceneView, placeViewLabels, prepareWeightedBlend).
    struct ExportViewFrame
    {
        bool                        active                  = false;    // an export view is in the graph
        bool                        idle                    = false;    // it draws nothing this frame
        bool                        survey                  = false;    // its label probes test only the tile's anchors, at once, and its weighted words are gathered
        bool                        frozen                  = false;    // its label fades and weighted span stay as the survey left them
        bool                        transparent             = false;
        SceneHandle                 scene;
        CameraHandle                camera;
        RenderTargetHandle          target;
        glm::mat4                   tileClip                = glm::mat4(1.0f);  // getExportTileClip, then the sample's jitter
        uint32_t                    imageWidth              = 0;                // the whole image's: the camera's aspect, the LOD, the labels' screen
        uint32_t                    imageHeight             = 0;
        ExportTile                  tile                    = {};
        float                       pixelScale              = 1.0f;
        float                       textureLodBias          = 0.0f;
        glm::vec2                   weightedDepths          = glm::vec2(0.0f);  // frozen: the span the survey found
    };

    // An export from exportView to takeExport or cancelExport. Its camera is a copy of the view's at the start, its
    // targets and buffers its own.
    struct ExportJob
    {
                                    ExportJob();
                                    ~ExportJob();
                                    ExportJob(const ExportJob&) = delete;
        ExportJob&                  operator=(const ExportJob&) = delete;

        ExportHandle                handle;
        View                        view;
        ExportDesc                  desc;
        ExportLayout                layout;
        ExportState                 state                   = ExportState::RUNNING;
        ExportPhase                 phase                   = ExportPhase::SETTLE;
        bool                        survey                  = false;    // the scene has labels or weighted surfaces
        CameraHandle                camera;
        RenderTargetHandle          target;
        BufferHandle                accum;
        BufferHandle                outputs[MAX_FRAMES_IN_FLIGHT]       = {};
        BufferHandle                params[MAX_FRAMES_IN_FLIGHT]        = {};
        uint32_t                    tile                    = 0;        // the phase's tile, and the sample of it
        uint32_t                    sample                  = 0;
        uint32_t                    phaseFrames             = 0;        // frames the phase has run
        // What each slot's last submitted frame left in its output buffer: a tile, or UINT32_MAX.
        uint32_t                    outputTiles[MAX_FRAMES_IN_FLIGHT]   = {UINT32_MAX, UINT32_MAX};
        bool                        surveySlots[MAX_FRAMES_IN_FLIGHT]   = {};   // the slot's last frame was a survey frame
        bool                        recorded                = false;    // this frame's sample is in the command buffer
        bool                        cancelled               = false;    // cancelled or taken: it goes once the graph no longer draws it
        float                       nearest                 = 0.0f;     // the survey's weighted span
        float                       farthest                = -1.0f;
        glm::vec2                   weightedDepths          = glm::vec2(0.0f);  // the span every tile's weights take
        std::vector<uint8_t>        band;                               // the tiles of the current row of tiles
        uint32_t                    bandTiles               = 0;
        uint32_t                    tilesDone               = 0;
        uint64_t                    framesDone              = 0;
        uint64_t                    framesTotal             = 0;
        ReadbackImage               image;                              // without a path
        std::unique_ptr<ImageWriter> writer;
    };

    struct Exports
    {
        HandlePool                  handles                 = HandlePool(1u << 16);
        std::vector<std::unique_ptr<ExportJob>> jobs;                   // in order: the first RUNNING one runs
        ExportViewFrame             view;
        uint64_t                    graphKey                = 0;        // changes when the graph's export passes must change
        bool                        streamBusy              = false;    // a tile streamer of the running export's scene loads or changes: set by its update, read by the next render
        PipelineHandle              pipeline;
    };

    // The scene view the active export draws, or null.
    const ExportViewFrame*          getExportViewFrame(const Exports& exports, SceneHandle scene, CameraHandle camera, const RenderTargetHandle& target);
    // The running export of a scene that a tile streamer must take into its screen error: its camera and the image's
    // size in pixels per pixelScale. False without one.
    bool                            getExportStreamView(const Exports& exports, SceneHandle scene, CameraHandle& camera, uint32_t& width, uint32_t& height, float& pixelScale);
    // A label's probe for the export's view this frame (LabelCandidate::probe, in the whole image's pixels): in the
    // survey the tile that keeps the anchor tests it at once, the other tiles leave it; in the tiles every label takes
    // the fade its survey found.
    glm::vec4                       getExportLabelProbe(const ExportViewFrame& frame, glm::vec4 probe);
    // The weighted words a slot's last frame wrote, when that frame was a survey frame of the running export.
    void                            gatherExportWeightedWords(Exports& exports, uint32_t slot, float nearest, float farthest);
    // While a scene's export draws its survey and tiles, its tile streamers change nothing that is drawn.
    bool                            isExportFrozen(const Exports& exports, SceneHandle scene);
    // A tile streamer of the scene has nothing loading nor changing, or not (it answers each update).
    void                            setExportStreamSettled(Exports& exports, SceneHandle scene, bool settled);

}
// clang-format on
