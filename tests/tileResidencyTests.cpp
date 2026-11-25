#include "rasm/renderer.h"
#include "scene/tileResidency.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <vector>

using rasm::TileCost;
using rasm::TileDesc;
using rasm::TileHandle;
using rasm::TileRefine;
using rasm::TileResidency;
using rasm::TileState;
using rasm::TileStreamerConfig;
using rasm::TileUnit;
using rasm::TileView;

namespace
{
    constexpr uint64_t TILE_BYTES = 100;

    // A camera at the origin looking down -z, 500 pixels per unit at depth 1, with no side planes: every tile is seen.
    TileView makeView()
    {
        TileView view;
        view.pixelsPerUnit = 500.0;
        for (glm::dvec4 &plane : view.planes)
            plane = glm::dvec4(0.0, 0.0, 0.0, 1.0);
        return view;
    }

    TileCost bytes(uint64_t count)
    {
        TileCost cost;
        cost.units[static_cast<uint32_t>(TileUnit::BYTES)] = count;
        return cost;
    }

    // Plenty of every pool; budget bytes.
    TileCost limits(uint64_t budget)
    {
        TileCost cost;
        for (uint64_t &value : cost.units)
            value = 1u << 30;
        cost.units[static_cast<uint32_t>(TileUnit::BYTES)] = budget;
        return cost;
    }

    // A tile at depth z ahead of the camera: the nearer or the larger its error, the larger its screen error.
    TileDesc tileAt(double z, double error, TileHandle parent = {}, TileRefine refine = TileRefine::REPLACE)
    {
        return {.parent = parent, .center = glm::dvec3(0.0, 0.0, -z), .radius = 1.0, .geometricError = error, .refine = refine};
    }

    bool contains(const std::vector<uint32_t> &list, TileHandle tile)
    {
        return std::find(list.begin(), list.end(), static_cast<uint32_t>(tile.index)) != list.end();
    }

    // The application's side: every tile handed out gets its content at once, and is ready in the next update.
    void giveContent(TileResidency &residency, uint64_t size = TILE_BYTES)
    {
        for (uint32_t index : residency.getToLoad())
            residency.setLoading(residency.handleOf(index), {});
        std::vector<uint32_t> loading = residency.getLoading();
        for (uint32_t index : loading)
            residency.setReady(residency.handleOf(index), bytes(size));
    }

    void step(TileResidency &residency, const TileHandle *wanted, uint32_t count, uint64_t budget)
    {
        const TileView view = makeView();
        residency.update(&view, 1, wanted, count, limits(budget));
    }
}

TEST_CASE("the screen error is the error over the depth of the sphere's near side, the most over the views")
{
    TileView views[2] = {makeView(), makeView()};
    // 10 units ahead, radius 2: depth 8, so 0.5 units of error are 0.5 * 500 / 8 pixels.
    CHECK(rasm::getTileScreenError(views, 1, glm::dvec3(0.0, 0.0, -10.0), 2.0, 0.5) == doctest::Approx(31.25));
    // The second view is nearer: it wins.
    views[1].origin = glm::dvec3(0.0, 0.0, -5.0);
    CHECK(rasm::getTileScreenError(views, 2, glm::dvec3(0.0, 0.0, -10.0), 2.0, 0.5) == doctest::Approx(250.0 / 3.0));
    // Outside a side plane (x >= 0 inside) by more than its radius, it counts for nothing.
    views[0].planes[0] = glm::dvec4(1.0, 0.0, 0.0, 0.0);
    CHECK(rasm::getTileScreenError(views, 1, glm::dvec3(-3.0, 0.0, -10.0), 2.0, 0.5) == 0.0);
    CHECK(rasm::getTileScreenError(views, 1, glm::dvec3(-1.0, 0.0, -10.0), 2.0, 0.5) > 0.0);
    // Orthographic: the same pixels per unit at every depth.
    views[0] = makeView();
    views[0].orthographic = true;
    views[0].pixelsPerUnit = 4.0;
    CHECK(rasm::getTileScreenError(views, 1, glm::dvec3(0.0, 0.0, -1000.0), 2.0, 0.5) == doctest::Approx(2.0));
    // A camera inside the sphere: huge, and still larger for a larger error.
    CHECK(rasm::getTileScreenError(views + 1, 1, glm::dvec3(0.0, 0.0, -5.0), 2.0, 1.0) > rasm::getTileScreenError(views + 1, 1, glm::dvec3(0.0, 0.0, -5.0), 2.0, 0.5));
}

TEST_CASE("parents load first, within the loads per frame and in flight")
{
    TileResidency residency({.maxLoadsPerFrame = 2, .maxLoadsInFlight = 3});
    const TileHandle root = residency.create(tileAt(10.0, 8.0));
    TileHandle children[4];
    for (uint32_t i = 0; i < 4; ++i)
        children[i] = residency.create(tileAt(10.0 + i, 4.0, root));

    step(residency, children, 4, 1u << 20);
    // The root has the largest error, then the nearest child.
    REQUIRE(residency.getToLoad().size() == 2);
    CHECK(residency.handleOf(residency.getToLoad()[0]) == root);
    CHECK(residency.handleOf(residency.getToLoad()[1]) == children[0]);
    CHECK(residency.find(root)->state == TileState::REQUESTED);

    // No content yet: one more fits in flight.
    step(residency, children, 4, 1u << 20);
    REQUIRE(residency.getToLoad().size() == 1);
    CHECK(residency.handleOf(residency.getToLoad()[0]) == children[1]);
    step(residency, children, 4, 1u << 20);
    CHECK(residency.getToLoad().empty());
    CHECK(residency.getStats().loading == 3);
}

TEST_CASE("a parent stands in until all its children are ready, and they swap in one update")
{
    TileResidency residency({.maxLoadsPerFrame = 1});
    const TileHandle root = residency.create(tileAt(10.0, 8.0));
    TileHandle children[4];
    for (uint32_t i = 0; i < 4; ++i)
        children[i] = residency.create(tileAt(10.0 + i, 4.0, root));

    // Nothing is ready: a hole for each wanted tile.
    step(residency, children, 4, 1u << 20);
    CHECK(residency.getStats().holes == 4);
    giveContent(residency);
    step(residency, children, 4, 1u << 20);
    CHECK(residency.find(root)->drawn);
    CHECK(contains(residency.getToShow(), root));
    CHECK(residency.getStats().fallbacks == 1);
    CHECK(residency.getStats().holes == 0);

    // Three children come in: still the root.
    for (uint32_t i = 0; i < 3; ++i)
    {
        giveContent(residency);
        step(residency, children, 4, 1u << 20);
        CHECK(residency.find(root)->drawn);
        CHECK_FALSE(residency.find(children[0])->drawn);
    }
    // The last one: the children show and the root hides in the same update.
    giveContent(residency);
    step(residency, children, 4, 1u << 20);
    CHECK_FALSE(residency.find(root)->drawn);
    CHECK(contains(residency.getToHide(), root));
    for (const TileHandle child : children)
    {
        CHECK(residency.find(child)->drawn);
        CHECK(contains(residency.getToShow(), child));
    }
    CHECK(residency.getStats().drawn == 4);

    // Zooming out: the root is wanted alone, and it is still in memory.
    step(residency, &root, 1, 1u << 20);
    CHECK(residency.find(root)->drawn);
    CHECK(residency.getToHide().size() == 4);
    CHECK(residency.getToLoad().empty());
}

TEST_CASE("an ADD parent stays drawn with its children, and a REPLACE child below it still has its stand-in")
{
    TileResidency residency({.maxLoadsPerFrame = 8});
    const TileHandle city = residency.create(tileAt(10.0, 8.0, {}, TileRefine::ADD));
    const TileHandle block = residency.create(tileAt(10.0, 4.0, city));
    const TileHandle detailA = residency.create(tileAt(10.0, 2.0, block));
    const TileHandle detailB = residency.create(tileAt(11.0, 2.0, block));
    const TileHandle wanted[2] = {detailA, detailB};

    step(residency, wanted, 2, 1u << 20);
    REQUIRE(residency.getToLoad().size() == 4);
    // Everything but detailB comes in.
    for (uint32_t index : residency.getToLoad())
        residency.setLoading(residency.handleOf(index), {});
    residency.setReady(city, bytes(TILE_BYTES));
    residency.setReady(block, bytes(TILE_BYTES));
    residency.setReady(detailA, bytes(TILE_BYTES));
    step(residency, wanted, 2, 1u << 20);
    // The city adds to its children, so it is drawn; the block stands in for its details.
    CHECK(residency.find(city)->drawn);
    CHECK(residency.find(block)->drawn);
    CHECK_FALSE(residency.find(detailA)->drawn);
    CHECK(residency.getStats().fallbacks == 1);

    residency.setReady(detailB, bytes(TILE_BYTES));
    step(residency, wanted, 2, 1u << 20);
    CHECK(residency.find(city)->drawn);
    CHECK_FALSE(residency.find(block)->drawn);
    CHECK(residency.find(detailA)->drawn);
    CHECK(residency.find(detailB)->drawn);

    // A missing ADD tile is not covered by its children: its REPLACE parent stands in for it.
    TileResidency other({.maxLoadsPerFrame = 8});
    const TileHandle ground = other.create(tileAt(10.0, 8.0));
    const TileHandle buildings = other.create(tileAt(10.0, 4.0, ground, TileRefine::ADD));
    const TileHandle detail = other.create(tileAt(10.0, 2.0, buildings));
    step(other, &detail, 1, 1u << 20);
    for (uint32_t index : other.getToLoad())
        other.setLoading(other.handleOf(index), {});
    other.setReady(ground, bytes(TILE_BYTES));
    other.setReady(detail, bytes(TILE_BYTES));
    step(other, &detail, 1, 1u << 20);
    CHECK(other.find(ground)->drawn);
    CHECK_FALSE(other.find(detail)->drawn);
}

TEST_CASE("the budget holds: unwanted tiles go first, the oldest first, then wanted ones of a lower error")
{
    TileResidency residency({.maxLoadsPerFrame = 8, .maxLoadsInFlight = 8});
    // Three roots, nearest first; room for three tiles.
    TileHandle near = residency.create(tileAt(5.0, 1.0));
    TileHandle middle = residency.create(tileAt(10.0, 1.0));
    TileHandle far = residency.create(tileAt(20.0, 1.0));
    const TileHandle all[3] = {near, middle, far};
    step(residency, all, 3, 3 * TILE_BYTES);
    giveContent(residency);
    step(residency, all, 3, 3 * TILE_BYTES);
    CHECK(residency.getStats().ready == 3);

    // The camera moves: two new tiles are wanted, far and middle are not. Middle was wanted last, so far goes first.
    const TileHandle fresh[2] = {residency.create(tileAt(6.0, 1.0)), residency.create(tileAt(7.0, 1.0))};
    const TileHandle nowWanted[3] = {near, fresh[0], fresh[1]};
    step(residency, all, 2, 3 * TILE_BYTES); // near and middle once more
    step(residency, nowWanted, 3, 3 * TILE_BYTES);
    // The estimate (the mean, 100 bytes) fits only after dropping both unwanted tiles.
    CHECK(residency.getToDrop().size() == 2);
    CHECK(residency.getToDrop()[0] == far.index);
    CHECK(residency.getToDrop()[1] == middle.index);
    CHECK(residency.getToLoad().size() == 2);
    CHECK(residency.getStats().bytes <= 3 * TILE_BYTES);
    giveContent(residency);
    step(residency, nowWanted, 3, 3 * TILE_BYTES);
    CHECK(residency.getStats().bytes == 3 * TILE_BYTES);

    // A nearer tile is wanted too, with no room: the wanted tile of the lowest error goes for it.
    const TileHandle nearest = residency.create(tileAt(2.0, 1.0));
    const TileHandle four[4] = {near, fresh[0], fresh[1], nearest};
    step(residency, four, 4, 3 * TILE_BYTES);
    CHECK(residency.getToDrop().size() == 1);
    CHECK(residency.getToDrop()[0] == fresh[1].index);
    CHECK(residency.getToLoad().size() == 1);
    giveContent(residency);
    // The farthest tile does not push a nearer one out: it waits for the budget.
    step(residency, four, 4, 3 * TILE_BYTES);
    CHECK(residency.getToDrop().empty());
    CHECK(residency.getStats().waitingForBudget == 1);
    CHECK(residency.getStats().bytes <= 3 * TILE_BYTES);
}

TEST_CASE("a load larger than its estimate, or a lower budget, drops tiles at once, within the drops per frame")
{
    TileResidency residency({.maxLoadsPerFrame = 8, .maxLoadsInFlight = 8, .maxDropsPerFrame = 1});
    TileHandle tiles[4];
    for (uint32_t i = 0; i < 4; ++i)
        tiles[i] = residency.create(tileAt(5.0 + i, 1.0));
    step(residency, tiles, 4, 4 * TILE_BYTES);
    giveContent(residency);
    step(residency, tiles, 4, 4 * TILE_BYTES);
    CHECK(residency.getStats().bytes == 4 * TILE_BYTES);

    // The budget halves: one drop per update, the farthest first.
    step(residency, tiles, 4, 2 * TILE_BYTES);
    CHECK(residency.getToDrop().size() == 1);
    CHECK(residency.getToDrop()[0] == tiles[3].index);
    step(residency, tiles, 4, 2 * TILE_BYTES);
    CHECK(residency.getToDrop()[0] == tiles[2].index);
    step(residency, tiles, 4, 2 * TILE_BYTES);
    CHECK(residency.getToDrop().empty());
    CHECK(residency.getStats().bytes == 2 * TILE_BYTES);
}

TEST_CASE("an unwanted load gives its place in flight up; a failed tile waits, then loads again")
{
    TileResidency residency({.maxLoadsPerFrame = 4, .maxLoadsInFlight = 1, .retryFailedAfter = 3});
    const TileHandle first = residency.create(tileAt(5.0, 1.0));
    const TileHandle second = residency.create(tileAt(6.0, 1.0));
    step(residency, &first, 1, 1u << 20);
    CHECK(residency.getToLoad().size() == 1);
    residency.setLoading(first, {});
    // The camera turns away from first: its load is cancelled for second.
    step(residency, &second, 1, 1u << 20);
    REQUIRE(residency.getToDrop().size() == 1);
    CHECK(residency.getToDrop()[0] == first.index);
    CHECK(residency.find(first)->state == TileState::EMPTY);
    CHECK(residency.handleOf(residency.getToLoad()[0]) == second);
    // Its content comes in late: a cancelled tile takes none.
    CHECK_FALSE(residency.setLoading(first, bytes(TILE_BYTES)));
    CHECK(residency.getUsed().units[static_cast<uint32_t>(TileUnit::BYTES)] == 0);

    residency.setLoading(second, {});
    residency.setFailed(second);
    CHECK(residency.find(second)->state == TileState::FAILED);
    for (uint32_t i = 0; i < 2; ++i)
    {
        step(residency, &second, 1, 1u << 20);
        CHECK(residency.getToLoad().empty());
    }
    step(residency, &second, 1, 1u << 20);
    CHECK(residency.getToLoad().size() == 1);
}

TEST_CASE("pools are limits too, and destroying a tile leaves its children as roots")
{
    TileResidency residency({.maxLoadsPerFrame = 8, .maxLoadsInFlight = 8});
    const TileHandle root = residency.create(tileAt(10.0, 8.0));
    const TileHandle child = residency.create(tileAt(10.0, 4.0, root));
    TileCost poolLimits = limits(1u << 20);
    poolLimits.units[static_cast<uint32_t>(TileUnit::TEXTURES)] = 1;
    TileCost oneTexture = bytes(TILE_BYTES);
    oneTexture.units[static_cast<uint32_t>(TileUnit::TEXTURES)] = 1;

    const rasm::TileView view = makeView();
    residency.update(&view, 1, &child, 1, poolLimits);
    for (uint32_t index : residency.getToLoad())
        residency.setLoading(residency.handleOf(index), {});
    residency.setReady(root, oneTexture);
    residency.update(&view, 1, &child, 1, poolLimits);
    // The child comes in over the one slot: it goes again at once, and the root stands in for it.
    residency.setReady(child, oneTexture);
    residency.update(&view, 1, &child, 1, poolLimits);
    CHECK(residency.getToDrop().size() == 1);
    CHECK(residency.getUsed().units[static_cast<uint32_t>(TileUnit::TEXTURES)] <= 1);
    CHECK(residency.find(root)->drawn);

    CHECK(residency.destroy(root));
    CHECK_FALSE(residency.isLive(root));
    CHECK_FALSE(residency.destroy(root));
    residency.update(&view, 1, &child, 1, limits(1u << 20));
    CHECK(residency.getStats().wanted == 1);
    CHECK(residency.getStats().tiles == 1);
}

TEST_CASE("a tile larger than the budget goes first and waits, without pushing others out or holding them back")
{
    TileResidency residency({.maxLoadsPerFrame = 8, .maxLoadsInFlight = 8});
    TileHandle small[3];
    for (uint32_t i = 0; i < 3; ++i)
        small[i] = residency.create(tileAt(10.0 + i, 1.0));
    // The application thinks the big one costs 50 bytes; its textures turn out to be 1000.
    TileDesc bigDesc = tileAt(2.0, 1.0);
    bigDesc.estimatedBytes = 50;
    const TileHandle big = residency.create(bigDesc);
    const TileHandle all[4] = {big, small[0], small[1], small[2]};
    step(residency, all, 4, 4 * TILE_BYTES);
    CHECK(residency.getToLoad().size() == 4);
    CHECK(residency.find(big)->cost.units[static_cast<uint32_t>(TileUnit::BYTES)] == 50);
    for (uint32_t index : residency.getToLoad())
        residency.setLoading(residency.handleOf(index), {});
    for (const TileHandle tile : small)
        residency.setReady(tile, bytes(TILE_BYTES));
    // Its textures are made before its pixels come: it holds them while it loads.
    residency.updateLoading(big, bytes(1000));
    CHECK(residency.getUsed().units[static_cast<uint32_t>(TileUnit::BYTES)] == 1300);
    // What the content holds, without the reservations: the pools' own counts are compared with it.
    CHECK(residency.getReal().units[static_cast<uint32_t>(TileUnit::BYTES)] == 1300);
    TileResidency fresh({.maxLoadsPerFrame = 8});
    TileDesc oneDesc = tileAt(5.0, 1.0);
    oneDesc.estimatedBytes = 50;
    const TileHandle one = fresh.create(oneDesc);
    step(fresh, &one, 1, 1u << 20);
    CHECK(fresh.getUsed().units[static_cast<uint32_t>(TileUnit::BYTES)] == 50);
    CHECK(fresh.getReal().units[static_cast<uint32_t>(TileUnit::BYTES)] == 0);

    step(residency, all, 4, 4 * TILE_BYTES);
    REQUIRE(residency.getToDrop().size() == 1);
    CHECK(residency.getToDrop()[0] == big.index);
    CHECK(residency.getStats().bytes == 3 * TILE_BYTES);
    // Its estimate is 1000 bytes now, more than the budget: it waits, and drops nothing.
    step(residency, all, 4, 4 * TILE_BYTES);
    CHECK(residency.getToDrop().empty());
    CHECK(residency.getToLoad().empty());
    CHECK(residency.getStats().waitingForBudget == 1);

    // A smaller wanted tile behind it still loads.
    const TileHandle more = residency.create(tileAt(20.0, 1.0));
    const TileHandle five[5] = {big, small[0], small[1], small[2], more};
    step(residency, five, 5, 4 * TILE_BYTES);
    REQUIRE(residency.getToLoad().size() == 1);
    CHECK(residency.handleOf(residency.getToLoad()[0]) == more);
}

// A streamer on a renderer that did not start (no window size, so no GPU is needed) is safe to call.
TEST_CASE("a TileStreamer on a renderer that did not start is safe")
{
    rasm::Renderer renderer(rasm::RendererConfig{.cacheDir = ""});
    const rasm::SceneHandle scene = renderer.createScene();
    rasm::TileStreamer streamer(renderer, scene);
    const TileHandle tile = streamer.createTile(tileAt(5.0, 1.0));
    CHECK(tile.isValid());
    streamer.setViews(nullptr, 0);
    streamer.update(&tile, 1);
    uint32_t count = 1;
    streamer.getTilesToLoad(count);
    CHECK(count == 0);
    CHECK(streamer.getTileState(tile) == TileState::EMPTY);
    // Content for a tile that was not handed out is refused.
    const rasm::EntityHandle stale{1, 7};
    CHECK_FALSE(streamer.setTileContent(tile, {.entities = &stale, .entityCount = 1}));
    CHECK(streamer.getTileState(tile) == TileState::EMPTY);
    streamer.destroyTile(tile);
    CHECK(streamer.getTileState(tile) == TileState::EMPTY);
    CHECK(streamer.getStats().tiles == 0);
    CHECK_FALSE(streamer.createTile({.radius = -1.0}).isValid());
}
