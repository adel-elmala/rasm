// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/types.h"
#include "rasm/view.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <memory>

namespace rasm
{

    class Renderer;

    // How a tile's children refine it. REPLACE: they stand in for it, and it is drawn only while one of them is not
    // ready (a terrain or imagery quadtree). ADD: they add to it, and it stays drawn with them (buildings on terrain).
    enum class TileRefine
    {
        REPLACE,
        ADD,
    };

    enum class TileState
    {
        EMPTY,          // no content; the streamer hands it out (getTilesToLoad) when it is wanted and fits the budget
        REQUESTED,      // handed out: the streamer waits for setTileContent
        LOADING,        // content given; its meshes or textures still load
        READY,          // in memory: drawn when the cut needs it
        FAILED,         // its content failed: a mesh FAILED, or a handle was invalid. Handed out again after retryFailedAfter frames
    };

    // A node of the application's tree. The streamer keeps only these numbers; the tree is the application's.
    struct TileDesc
    {
        TileHandle                  parent;                                     // invalid: a root
        glm::dvec3                  center                  = glm::dvec3(0.0);  // the bounding sphere of its content, in world space; a child's sits inside its parent's
        double                      radius                  = 0.0;
        double                      geometricError          = 0.0;              // world units the tile is off from the full detail by; a leaf's is 0
        TileRefine                  refine                  = TileRefine::REPLACE;
        uint64_t                    estimatedBytes          = 0;                // its content's bytes, until it is measured once; 0: the mean of the tiles measured so far
    };

    // What a tile owns from setTileContent on: the streamer adds the entities to its scene while the tile is drawn, and
    // destroys everything listed when it drops the tile (the entities first). Content that several tiles share (one tree
    // mesh, a common material) is not listed. A mesh from AssetLoader::loadGLTF brings the materials and textures of
    // its import, which count with it.
    struct TileContent
    {
        const EntityHandle*         entities                = nullptr;          // not in a scene: the streamer adds them
        uint32_t                    entityCount             = 0;
        const MeshHandle*           meshes                  = nullptr;
        uint32_t                    meshCount               = 0;
        const MaterialHandle*       materials               = nullptr;
        uint32_t                    materialCount           = 0;
        const TextureHandle*        textures                = nullptr;
        uint32_t                    textureCount            = 0;
    };

    struct TileStreamerConfig
    {
        uint64_t                    memoryBudget            = 0;                // bytes of tile content in GPU memory; 0: deviceShare of what the device's budget leaves
        float                       deviceShare             = 0.8f;             // the automatic budget's share of the device-local budget left after everything that is not a tile
        uint32_t                    maxTiles                = 1u << 16;         // tile records at once
        uint32_t                    maxLoadsPerFrame        = 4;                // tiles getTilesToLoad hands out per update
        uint32_t                    maxLoadsInFlight        = 16;               // tiles REQUESTED or LOADING at once
        uint32_t                    maxDropsPerFrame        = 32;               // tiles whose content an update destroys or cancels
        uint32_t                    retryFailedAfter        = 120;              // updates before a FAILED tile is handed out again; 0: never
    };

    struct TileStreamerStats
    {
        uint32_t                    tiles                   = 0;                // records
        uint32_t                    wanted                  = 0;                // the cut and its ancestors
        uint32_t                    ready                   = 0;
        uint32_t                    loading                 = 0;                // REQUESTED and LOADING
        uint32_t                    failed                  = 0;
        uint32_t                    drawn                   = 0;
        uint32_t                    fallbacks               = 0;                // drawn in place of children that are not ready
        uint32_t                    holes                   = 0;                // wanted tiles with nothing drawn in their place
        uint32_t                    waitingForBudget        = 0;                // wanted EMPTY tiles that did not fit the budget or a pool
        uint32_t                    loadsStarted            = 0;                // in the last update
        uint32_t                    dropped                 = 0;                // in the last update: content destroyed, loads cancelled included
        uint64_t                    bytes                   = 0;                // ready tiles' measured bytes and the loading ones' reserved bytes
        uint64_t                    peakBytes               = 0;
        uint64_t                    budget                  = 0;                // the byte budget the last update kept to
        uint64_t                    deviceBudget            = 0;                // VK_EXT_memory_budget: the device-local heaps' budget and the process's use
        uint64_t                    deviceUsage             = 0;
        uint64_t                    totalLoads              = 0;
        uint64_t                    totalDrops              = 0;
    };

    // Keeps a scene's tiles in GPU memory under a byte budget. Each frame the application sets its views, walks its tree
    // with getScreenError, and passes the tiles it wants drawn (its cut) to update. The streamer then decides:
    // - what loads: the wanted tiles by screen error, the largest first, so a tile's parents load before it; it hands
    //   them out through getTilesToLoad, and the application makes their content and gives it with setTileContent;
    // - what stays: a tile's bytes are its textures' allocations and its meshes' share of the geometry pools, and every
    //   fixed pool is checked too; to make room it drops tiles nobody wants (the oldest first), then wanted ones of a
    //   lower screen error;
    // - what is drawn: each wanted tile once it is ready, else its nearest ready parent in its place.
    // Its work per update is bounded by the config's limits, it never waits for the GPU, and a frame where no tile
    // changes allocates nothing. The renderer must outlive it; destroying it destroys every tile's content.
    class TileStreamer
    {
    public:
                                    TileStreamer(Renderer& renderer, SceneHandle scene, const TileStreamerConfig& config = {});
                                    ~TileStreamer();
                                    TileStreamer(const TileStreamer&) = delete;
        TileStreamer&               operator=(const TileStreamer&) = delete;
        TileHandle                  createTile(const TileDesc& desc);
        // Destroys its content. Its children stay, as roots.
        void                        destroyTile(TileHandle tile);
        // The views of the scene this frame (other scenes' views are skipped); getScreenError and update use them.
        void                        setViews(const View* views, uint32_t viewCount);
        // The sphere's geometric error in pixels: the most over the views whose frustum it touches, 0 outside all of them.
        double                      getScreenError(const glm::dvec3& center, double radius, double geometricError) const;
        double                      getScreenError(TileHandle tile) const;
        // wanted: the tiles to draw (the cut); their parents are kept as stand-ins. Call it before Renderer::render.
        void                        update(const TileHandle* wanted, uint32_t wantedCount);
        // The tiles the last update handed out: give each its content with setTileContent, now or in a later frame.
        const TileHandle*           getTilesToLoad(uint32_t& count) const;
        // The streamer owns the content from now on, also when it returns false (it destroys it then). False for a tile
        // that has content already (LOADING or READY), an invalid tile, or an invalid handle in the content.
        bool                        setTileContent(TileHandle tile, const TileContent& content);
        TileState                   getTileState(TileHandle tile) const;
        bool                        isTileDrawn(TileHandle tile) const;
        const TileStreamerStats&    getStats() const;
        TileStreamerConfig          getConfig() const;

        struct Impl;

    private:
        std::unique_ptr<Impl>       impl;
    };

}
// clang-format on
