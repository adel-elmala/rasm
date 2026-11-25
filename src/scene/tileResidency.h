// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/tileStreamer.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <vector>

namespace rasm
{

    // What a tile takes of each budget: its bytes, and the rows of each fixed pool.
    enum class TileUnit : uint32_t
    {
        BYTES,
        VERTICES,
        INDICES,
        SUB_MESHES,
        MESHLETS,
        INSTANCES,
        MATERIALS,
        TEXTURES,       // bindless slots
        COUNT,
    };

    constexpr uint32_t  TILE_UNITS  = static_cast<uint32_t>(TileUnit::COUNT);

    struct TileCost
    {
        uint64_t                    units[TILE_UNITS]       = {};
    };

    // A view as the screen error needs it, in world space and doubles.
    struct TileView
    {
        glm::dvec3                  origin                  = glm::dvec3(0.0);  // the camera's position
        glm::dvec3                  forward                 = glm::dvec3(0.0, 0.0, -1.0);
        double                      pixelsPerUnit           = 0.0;              // at depth 1 for a perspective view, everywhere for an orthographic one
        bool                        orthographic            = false;
        glm::dvec4                  planes[4]               = {};               // left, right, bottom, top: dot(xyz, p) + w >= 0 inside, xyz unit length
    };

    struct TileRecord
    {
        TileHandle                  handle;
        TileHandle                  parent;
        glm::dvec3                  center                  = glm::dvec3(0.0);
        double                      radius                  = 0.0;
        double                      geometricError          = 0.0;
        TileRefine                  refine                  = TileRefine::REPLACE;
        uint64_t                    estimatedBytes          = 0;                // TileDesc::estimatedBytes
        uint32_t                    depth                   = 0;                // 0 for a root
        TileState                   state                   = TileState::EMPTY;
        TileCost                    cost;                                       // READY: measured; REQUESTED, LOADING: reserved
        TileCost                    real;                                       // what its content holds now: READY its cost, LOADING what was made so far
        TileCost                    lastCost;                                   // the last measured cost, the estimate when it loads again
        bool                        measured                = false;
        bool                        drawn                   = false;
        bool                        covered                 = false;            // this update: it, or its wanted children, fill its place
        uint64_t                    lastWanted              = 0;                // the update that last wanted it
        uint64_t                    since                   = 0;                // the update it entered its state
        double                      priority                = 0.0;              // this update's screen error, at least its wanted children's
        uint32_t                    firstChild              = 0;                // this update's wanted children, by index; 0: none
        uint32_t                    nextSibling             = 0;
        uint32_t                    heldAt                  = UINT32_MAX;       // its place in held, while it holds a cost
    };

    double                          getTileScreenError(const TileView* views, uint32_t viewCount, const glm::dvec3& center, double radius, double geometricError);

    // The streamer's decisions, on plain data: which wanted tiles load, which tiles go, and which are drawn. The caller
    // (TileStreamer) makes and destroys their content, and reports what it measured. Every list keeps its capacity.
    class TileResidency
    {
    public:
        explicit                    TileResidency(const TileStreamerConfig& config);
        TileHandle                  create(const TileDesc& desc);
        // False for a stale handle. The caller destroyed the content (and hid it) first.
        bool                        destroy(TileHandle tile);
        bool                        isLive(TileHandle tile) const;
        const TileRecord*           find(TileHandle tile) const;
        // Content arrived for a REQUESTED tile: it holds known, or what it reserved if that is more. False for any other state
        // (a tile the streamer cancelled meanwhile is EMPTY again).
        bool                        setLoading(TileHandle tile, const TileCost& known);
        // A LOADING tile's content so far (a texture is made before its pixels come): it holds at least that.
        void                        updateLoading(TileHandle tile, const TileCost& known);
        void                        setReady(TileHandle tile, const TileCost& cost);
        // Its content went: it holds nothing, and waits retryFailedAfter updates.
        void                        setFailed(TileHandle tile);
        // limits: what the tiles may hold of each unit (bytes: the budget; a pool: its rows not held by anything else).
        void                        update(const TileView* views, uint32_t viewCount, const TileHandle* wanted, uint32_t wantedCount, const TileCost& limits);

        const std::vector<uint32_t>& getToLoad() const      { return toLoad; }      // handed out: REQUESTED now
        const std::vector<uint32_t>& getToDrop() const      { return toDrop; }      // their content must go: EMPTY now
        const std::vector<uint32_t>& getToShow() const      { return toShow; }
        const std::vector<uint32_t>& getToHide() const      { return toHide; }      // the dropped ones that were drawn too
        const std::vector<uint32_t>& getLoading() const     { return loading; }     // LOADING tiles, for the caller to poll
        const TileRecord&           record(uint32_t index) const    { return records[index]; }
        TileHandle                  handleOf(uint32_t index) const  { return records[index].handle; }
        const TileCost&             getUsed() const         { return used; }
        const TileCost&             getReal() const         { return real; }        // what the tiles' content holds now, reservations left out
        const TileStreamerConfig&   getConfig() const       { return config; }
        const TileStreamerStats&    getStats() const        { return stats; }

    private:
        TileRecord*                 findRecord(TileHandle tile);
        bool                        parentLive(const TileRecord& record) const;
        TileCost                    estimate(const TileRecord& record) const;
        bool                        fits(const TileCost& cost, const TileCost& limits) const;
        bool                        canFree(uint32_t candidate, const TileCost& cost, const TileCost& limits);
        void                        hold(uint32_t index, const TileCost& cost);
        void                        release(uint32_t index);
        void                        setReal(uint32_t index, const TileCost& cost);
        void                        setState(uint32_t index, TileState state);
        void                        drop(uint32_t index);
        void                        sortVictims();
        bool                        isVictimBefore(uint32_t a, uint32_t b) const;
        bool                        takeVictim(uint32_t candidate);
        double                      propagatePriority(uint32_t index);
        void                        cover(uint32_t index);
        void                        draw(uint32_t index, bool standIn);

        TileStreamerConfig          config;
        HandlePool                  handles;
        std::vector<TileRecord>     records;                                    // by handle index
        std::vector<uint32_t>       held;                                       // tiles REQUESTED, LOADING or READY
        std::vector<uint32_t>       loading;
        std::vector<uint32_t>       wantedList;
        std::vector<uint32_t>       roots;
        std::vector<uint32_t>       candidates;
        std::vector<uint32_t>       victims;
        std::vector<uint32_t>       drawnList;
        std::vector<uint32_t>       newDrawn;
        std::vector<uint32_t>       toLoad;
        std::vector<uint32_t>       toDrop;
        std::vector<uint32_t>       toShow;
        std::vector<uint32_t>       toHide;
        size_t                      victimCursor            = 0;
        bool                        victimsSorted           = false;
        uint32_t                    dropsLeft               = 0;
        uint64_t                    updateCount             = 0;
        TileCost                    used;
        TileCost                    real;
        TileCost                    measuredSum;
        uint64_t                    measuredCount           = 0;
        TileStreamerStats           stats;
    };

}
// clang-format on
