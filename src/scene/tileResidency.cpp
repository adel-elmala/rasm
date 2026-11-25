#include "scene/tileResidency.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>

namespace rasm
{
    namespace
    {
        // A camera inside a tile's sphere: its error is huge, not infinite, so the coarser tile still sorts first.
        constexpr double MIN_DEPTH = 1e-6;
        // Wanted tiles an update tries to fit and cannot before it stops looking: it bounds the work.
        constexpr uint32_t MAX_MISSES = 16;
    }

    namespace
    {
        bool isInFlight(TileState state)
        {
            return state == TileState::REQUESTED || state == TileState::LOADING;
        }

        void addCost(TileCost &target, const TileCost &cost)
        {
            for (uint32_t k = 0; k < TILE_UNITS; ++k)
                target.units[k] += cost.units[k];
        }

        void subtractCost(TileCost &target, const TileCost &cost)
        {
            for (uint32_t k = 0; k < TILE_UNITS; ++k)
                target.units[k] -= std::min(target.units[k], cost.units[k]);
        }

        TileCost maxCost(const TileCost &a, const TileCost &b)
        {
            TileCost cost;
            for (uint32_t k = 0; k < TILE_UNITS; ++k)
                cost.units[k] = std::max(a.units[k], b.units[k]);
            return cost;
        }

        void eraseIndex(std::vector<uint32_t> &list, uint32_t index)
        {
            const auto found = std::find(list.begin(), list.end(), index);
            if (found == list.end())
                return;
            *found = list.back();
            list.pop_back();
        }
    }

    // Error in pixels = error in world units x pixels per world unit, which is the view's pixels per unit at depth 1 over
    // the depth of the sphere's nearest point (lodSelection.slang's rule for a mesh LOD).
    double getTileScreenError(const TileView *views, uint32_t viewCount, const glm::dvec3 &center, double radius, double geometricError)
    {
        double most = 0.0;
        for (uint32_t i = 0; i < viewCount; ++i)
        {
            const TileView &view = views[i];
            bool inside = true;
            for (const glm::dvec4 &plane : view.planes)
                inside = inside && glm::dot(glm::dvec3(plane), center) + plane.w >= -radius;
            if (!inside)
                continue;
            double pixels = geometricError * view.pixelsPerUnit;
            if (!view.orthographic)
                pixels /= std::max(glm::dot(view.forward, center - view.origin) - radius, MIN_DEPTH);
            most = std::max(most, pixels);
        }
        return most;
    }

    TileResidency::TileResidency(const TileStreamerConfig &config)
        : config(config), handles(static_cast<uint64_t>(std::max(config.maxTiles, 1u)) + 1), records(1)
    {
        this->config.maxLoadsInFlight = std::max(config.maxLoadsInFlight, 1u);
    }

    TileHandle TileResidency::create(const TileDesc &desc)
    {
        if (!std::isfinite(desc.center.x) || !std::isfinite(desc.center.y) || !std::isfinite(desc.center.z) || !std::isfinite(desc.radius) || desc.radius < 0.0 ||
            !std::isfinite(desc.geometricError) || desc.geometricError < 0.0)
        {
            spdlog::error("createTile: the center, radius and geometric error must be finite, and the radius and error not negative.");
            return {};
        }
        const HandleSlot slot = handles.allocate();
        if (slot.generation == 0)
        {
            spdlog::error("createTile: all {} tiles are in use (TileStreamerConfig::maxTiles).", config.maxTiles);
            return {};
        }
        if (records.size() <= slot.index)
            records.resize(slot.index + 1);
        TileRecord &record = records[slot.index];
        record = TileRecord{};
        record.handle = {slot.index, slot.generation};
        record.parent = desc.parent;
        record.center = desc.center;
        record.radius = desc.radius;
        record.geometricError = desc.geometricError;
        record.refine = desc.refine;
        record.estimatedBytes = desc.estimatedBytes;
        record.depth = parentLive(record) ? records[desc.parent.index].depth + 1 : 0;
        ++stats.tiles;
        return record.handle;
    }

    bool TileResidency::destroy(TileHandle tile)
    {
        TileRecord *record = findRecord(tile);
        if (record == nullptr)
            return false;
        const auto index = static_cast<uint32_t>(tile.index);
        if (record->heldAt != UINT32_MAX)
            release(index);
        if (record->state == TileState::LOADING)
            eraseIndex(loading, index);
        if (record->drawn)
            eraseIndex(drawnList, index);
        setState(index, TileState::EMPTY);
        *record = TileRecord{};
        handles.release(tile.index, tile.generation);
        --stats.tiles;
        return true;
    }

    bool TileResidency::isLive(TileHandle tile) const
    {
        return handles.isAlive(tile.index, tile.generation);
    }

    const TileRecord *TileResidency::find(TileHandle tile) const
    {
        return isLive(tile) ? &records[tile.index] : nullptr;
    }

    TileRecord *TileResidency::findRecord(TileHandle tile)
    {
        return isLive(tile) ? &records[tile.index] : nullptr;
    }

    bool TileResidency::parentLive(const TileRecord &record) const
    {
        return isLive(record.parent);
    }

    // Its last measured cost; else the mean of every measured tile's, with the application's estimate of its bytes when
    // it gave one; else nothing (the first loads find out).
    TileCost TileResidency::estimate(const TileRecord &record) const
    {
        if (record.measured)
            return record.lastCost;
        TileCost cost;
        for (uint32_t k = 0; measuredCount > 0 && k < TILE_UNITS; ++k)
            cost.units[k] = (measuredSum.units[k] + measuredCount - 1) / measuredCount;
        if (record.estimatedBytes > 0)
            cost.units[static_cast<uint32_t>(TileUnit::BYTES)] = record.estimatedBytes;
        return cost;
    }

    // extra fits next to what the tiles hold. A unit it does not use fits even when the tiles are over it.
    bool TileResidency::fits(const TileCost &extra, const TileCost &limits) const
    {
        for (uint32_t k = 0; k < TILE_UNITS; ++k)
        {
            if (extra.units[k] > 0 && used.units[k] + extra.units[k] > limits.units[k])
                return false;
        }
        return true;
    }

    void TileResidency::hold(uint32_t index, const TileCost &cost)
    {
        TileRecord &record = records[index];
        if (record.heldAt != UINT32_MAX)
            subtractCost(used, record.cost);
        else
        {
            record.heldAt = static_cast<uint32_t>(held.size());
            held.push_back(index);
        }
        record.cost = cost;
        addCost(used, cost);
    }

    void TileResidency::release(uint32_t index)
    {
        TileRecord &record = records[index];
        subtractCost(used, record.cost);
        record.cost = {};
        const uint32_t last = held.back();
        held[record.heldAt] = last;
        records[last].heldAt = record.heldAt;
        held.pop_back();
        record.heldAt = UINT32_MAX;
    }

    void TileResidency::setState(uint32_t index, TileState state)
    {
        TileRecord &record = records[index];
        record.state = state;
        record.since = updateCount;
    }

    bool TileResidency::setLoading(TileHandle tile, const TileCost &known)
    {
        TileRecord *record = findRecord(tile);
        if (record == nullptr || record->state == TileState::LOADING || record->state == TileState::READY)
            return false;
        const auto index = static_cast<uint32_t>(tile.index);
        const TileCost reserved = record->state == TileState::REQUESTED ? record->cost : estimate(*record);
        hold(index, maxCost(reserved, known));
        setState(index, TileState::LOADING);
        loading.push_back(index);
        return true;
    }

    void TileResidency::updateLoading(TileHandle tile, const TileCost &known)
    {
        TileRecord *record = findRecord(tile);
        if (record != nullptr && record->state == TileState::LOADING)
            hold(static_cast<uint32_t>(tile.index), maxCost(record->cost, known));
    }

    void TileResidency::setReady(TileHandle tile, const TileCost &cost)
    {
        TileRecord *record = findRecord(tile);
        if (record == nullptr || record->state != TileState::LOADING)
            return;
        const auto index = static_cast<uint32_t>(tile.index);
        hold(index, cost);
        record->lastCost = cost;
        record->measured = true;
        addCost(measuredSum, cost);
        ++measuredCount;
        setState(index, TileState::READY);
        eraseIndex(loading, index);
    }

    void TileResidency::setFailed(TileHandle tile)
    {
        TileRecord *record = findRecord(tile);
        if (record == nullptr || record->state == TileState::READY)
            return;
        const auto index = static_cast<uint32_t>(tile.index);
        if (record->heldAt != UINT32_MAX)
            release(index);
        if (record->state == TileState::LOADING)
            eraseIndex(loading, index);
        setState(index, TileState::FAILED);
    }

    void TileResidency::drop(uint32_t index)
    {
        TileRecord &record = records[index];
        // What a load held so far (textures made before their pixels) is a better estimate for its next load.
        if (record.state == TileState::LOADING)
        {
            eraseIndex(loading, index);
            record.lastCost = record.measured ? maxCost(record.lastCost, record.cost) : record.cost;
            record.measured = true;
        }
        release(index);
        setState(index, TileState::EMPTY);
        toDrop.push_back(index);
        --dropsLeft;
        ++stats.dropped;
        ++stats.totalDrops;
    }

    // The order tiles go in: tiles nobody wants this update, the longest unwanted first; then wanted ones, the lowest
    // screen error first, the deepest first among equals (a parent outlives its children).
    bool TileResidency::isVictimBefore(uint32_t a, uint32_t b) const
    {
        const TileRecord &ra = records[a];
        const TileRecord &rb = records[b];
        const bool wantedA = ra.lastWanted == updateCount;
        const bool wantedB = rb.lastWanted == updateCount;
        if (wantedA != wantedB)
            return !wantedA;
        if (!wantedA && ra.lastWanted != rb.lastWanted)
            return ra.lastWanted < rb.lastWanted;
        if (wantedA && ra.priority != rb.priority)
            return ra.priority < rb.priority;
        if (ra.depth != rb.depth)
            return ra.depth > rb.depth;
        return a > b;
    }

    void TileResidency::sortVictims()
    {
        victims.assign(held.begin(), held.end());
        std::sort(victims.begin(), victims.end(), [this](uint32_t a, uint32_t b)
                  { return isVictimBefore(a, b); });
        victimCursor = 0;
        victimsSorted = true;
    }

    // Whether dropping the tiles that go before candidate, as many as an update may still drop, makes room for cost. It
    // drops nothing, so a tile that can never fit costs no other tile its place.
    bool TileResidency::canFree(uint32_t candidate, const TileCost &cost, const TileCost &limits)
    {
        if (!victimsSorted)
            sortVictims();
        TileCost freed;
        uint32_t drops = 0;
        size_t cursor = victimCursor;
        while (true)
        {
            bool fit = true;
            for (uint32_t k = 0; k < TILE_UNITS; ++k)
                fit = fit && (cost.units[k] == 0 || used.units[k] - freed.units[k] + cost.units[k] <= limits.units[k]);
            if (fit)
                return true;
            while (cursor < victims.size() && records[victims[cursor]].heldAt == UINT32_MAX)
                ++cursor;
            if (drops == dropsLeft || cursor == victims.size() || !isVictimBefore(victims[cursor], candidate))
                return false;
            addCost(freed, records[victims[cursor]].cost);
            ++drops;
            ++cursor;
        }
    }

    // Drops the next tile in the order, if it goes before candidate (UINT32_MAX: any tile). False when none is left.
    bool TileResidency::takeVictim(uint32_t candidate)
    {
        if (!victimsSorted)
            sortVictims();
        while (victimCursor < victims.size() && records[victims[victimCursor]].heldAt == UINT32_MAX)
            ++victimCursor;
        if (dropsLeft == 0 || victimCursor == victims.size())
            return false;
        const uint32_t victim = victims[victimCursor];
        if (candidate != UINT32_MAX && !isVictimBefore(victim, candidate))
            return false;
        ++victimCursor;
        drop(victim);
        return true;
    }

    // A tile's priority is at least its wanted children's: they need it as their stand-in.
    double TileResidency::propagatePriority(uint32_t index)
    {
        TileRecord &record = records[index];
        double priority = record.priority;
        for (uint32_t child = record.firstChild; child != 0; child = records[child].nextSibling)
            priority = std::max(priority, propagatePriority(child));
        record.priority = priority;
        return priority;
    }

    // Covered: the tile's place is filled, by itself or (REPLACE) by its wanted children. An ADD tile's children do not
    // stand in for its own content.
    void TileResidency::cover(uint32_t index)
    {
        TileRecord &record = records[index];
        bool children = record.firstChild != 0;
        for (uint32_t child = record.firstChild; child != 0; child = records[child].nextSibling)
        {
            cover(child);
            children = children && records[child].covered;
        }
        const bool ready = record.state == TileState::READY;
        record.covered = ready || (record.refine == TileRefine::REPLACE && children);
    }

    // standIn: a drawn ancestor fills this place already (an ADD parent).
    void TileResidency::draw(uint32_t index, bool standIn)
    {
        TileRecord &record = records[index];
        const bool ready = record.state == TileState::READY;
        bool childrenCovered = record.firstChild != 0;
        for (uint32_t child = record.firstChild; child != 0; child = records[child].nextSibling)
            childrenCovered = childrenCovered && records[child].covered;

        bool drawnHere = false;
        bool descend = record.firstChild != 0;
        if (record.firstChild == 0 || record.refine == TileRefine::ADD)
            drawnHere = ready;
        else if (!childrenCovered && ready)
        {
            // REPLACE with a child not ready: the tile stands in for all of them.
            drawnHere = true;
            descend = false;
            ++stats.fallbacks;
        }
        if (drawnHere)
            newDrawn.push_back(index);
        else if (record.firstChild == 0 && !standIn)
            ++stats.holes;
        if (!descend)
            return;
        for (uint32_t child = record.firstChild; child != 0; child = records[child].nextSibling)
            draw(child, standIn || (drawnHere && record.refine == TileRefine::ADD));
    }

    void TileResidency::update(const TileView *views, uint32_t viewCount, const TileHandle *wanted, uint32_t wantedCount, const TileCost &limits)
    {
        ++updateCount;
        toLoad.clear();
        toDrop.clear();
        toShow.clear();
        toHide.clear();
        victimsSorted = false;
        dropsLeft = config.maxDropsPerFrame;
        stats.fallbacks = 0;
        stats.holes = 0;
        stats.waitingForBudget = 0;
        stats.loadsStarted = 0;
        stats.dropped = 0;

        // The cut and every ancestor of it, each once, with its wanted children linked.
        wantedList.clear();
        roots.clear();
        for (uint32_t i = 0; wanted != nullptr && i < wantedCount; ++i)
        {
            if (!isLive(wanted[i]))
                continue;
            auto index = static_cast<uint32_t>(wanted[i].index);
            while (records[index].lastWanted != updateCount)
            {
                TileRecord &record = records[index];
                record.lastWanted = updateCount;
                record.firstChild = 0;
                record.nextSibling = 0;
                wantedList.push_back(index);
                if (!parentLive(record))
                    break;
                index = static_cast<uint32_t>(record.parent.index);
            }
        }
        for (uint32_t index : wantedList)
        {
            TileRecord &record = records[index];
            // A tile is wanted for the error of its parent, which it fixes: that error, seen at the tile's own bounds, ranks
            // it (a leaf's own error is 0).
            const double error = parentLive(record) ? std::max(record.geometricError, records[record.parent.index].geometricError) : record.geometricError;
            record.priority = getTileScreenError(views, viewCount, record.center, record.radius, error);
            if (parentLive(record) && records[record.parent.index].lastWanted == updateCount)
            {
                TileRecord &parent = records[record.parent.index];
                record.nextSibling = parent.firstChild;
                parent.firstChild = index;
            }
            else
                roots.push_back(index);
        }
        for (uint32_t root : roots)
            propagatePriority(root);

        // Over a limit (a load came in larger than its estimate, the budget fell): first a tile that alone is over a limit,
        // which can never fit, then from the front of the order.
        for (size_t i = 0; i < held.size() && dropsLeft > 0;)
        {
            bool alone = false;
            for (uint32_t k = 0; k < TILE_UNITS; ++k)
                alone = alone || records[held[i]].cost.units[k] > limits.units[k];
            if (alone)
                drop(held[i]);
            else
                ++i;
        }
        bool over = false;
        for (uint32_t k = 0; k < TILE_UNITS; ++k)
            over = over || used.units[k] > limits.units[k];
        while (over && dropsLeft > 0 && takeVictim(UINT32_MAX))
        {
            over = false;
            for (uint32_t k = 0; k < TILE_UNITS; ++k)
                over = over || used.units[k] > limits.units[k];
        }

        // Loads: wanted tiles without content, the largest screen error first, as far as the limits let them.
        candidates.clear();
        for (uint32_t index : wantedList)
        {
            const TileRecord &record = records[index];
            const bool retry = record.state == TileState::FAILED && config.retryFailedAfter > 0 && updateCount - record.since >= config.retryFailedAfter;
            if (record.state == TileState::EMPTY || retry)
                candidates.push_back(index);
        }
        std::sort(candidates.begin(), candidates.end(), [this](uint32_t a, uint32_t b)
                  {
                      const TileRecord &ra = records[a];
                      const TileRecord &rb = records[b];
                      if (ra.priority != rb.priority)
                          return ra.priority > rb.priority;
                      if (ra.depth != rb.depth)
                          return ra.depth < rb.depth;
                      return a < b; });
        auto inFlight = static_cast<uint32_t>(loading.size());
        for (uint32_t index : held)
            inFlight += records[index].state == TileState::REQUESTED ? 1 : 0;
        uint32_t misses = 0;
        for (size_t c = 0; c < candidates.size(); ++c)
        {
            const uint32_t candidate = candidates[c];
            if (stats.loadsStarted >= config.maxLoadsPerFrame)
                break;
            if (inFlight >= config.maxLoadsInFlight)
            {
                // A load nobody wants any more gives its place up, the longest unwanted first.
                uint32_t cancel = UINT32_MAX;
                for (uint32_t index : held)
                {
                    const TileRecord &record = records[index];
                    if (isInFlight(record.state) && record.lastWanted != updateCount && (cancel == UINT32_MAX || record.lastWanted < records[cancel].lastWanted))
                        cancel = index;
                }
                if (cancel == UINT32_MAX || dropsLeft == 0)
                    break;
                drop(cancel);
                --inFlight;
            }
            // A tile that does not fit, even with every tile before it dropped, waits and lets the next ones try.
            const TileCost cost = estimate(records[candidate]);
            if (!fits(cost, limits) && !canFree(candidate, cost, limits))
            {
                ++stats.waitingForBudget;
                if (++misses == MAX_MISSES)
                {
                    stats.waitingForBudget += static_cast<uint32_t>(candidates.size() - c - 1);
                    break;
                }
                continue;
            }
            while (!fits(cost, limits) && takeVictim(candidate))
            {
            }
            hold(candidate, cost);
            setState(candidate, TileState::REQUESTED);
            toLoad.push_back(candidate);
            ++inFlight;
            ++stats.loadsStarted;
            ++stats.totalLoads;
        }

        // The drawn set: each wanted tile once it is ready, else its nearest ready ancestor in its place.
        newDrawn.clear();
        for (uint32_t root : roots)
            cover(root);
        for (uint32_t root : roots)
            draw(root, false);
        for (uint32_t index : newDrawn)
        {
            if (!records[index].drawn)
                toShow.push_back(index);
        }
        for (uint32_t index : drawnList)
            records[index].drawn = false;
        for (uint32_t index : newDrawn)
            records[index].drawn = true;
        for (uint32_t index : drawnList)
        {
            if (!records[index].drawn)
                toHide.push_back(index);
        }
        std::swap(drawnList, newDrawn);

        stats.wanted = static_cast<uint32_t>(wantedList.size());
        stats.ready = 0;
        stats.loading = 0;
        stats.failed = 0;
        for (uint32_t index : held)
        {
            stats.ready += records[index].state == TileState::READY ? 1 : 0;
            stats.loading += isInFlight(records[index].state) ? 1 : 0;
        }
        for (uint32_t index : wantedList)
            stats.failed += records[index].state == TileState::FAILED ? 1 : 0;
        stats.drawn = static_cast<uint32_t>(drawnList.size());
        stats.bytes = used.units[static_cast<uint32_t>(TileUnit::BYTES)];
        stats.peakBytes = std::max(stats.peakBytes, stats.bytes);
        stats.budget = limits.units[static_cast<uint32_t>(TileUnit::BYTES)];
    }
}
