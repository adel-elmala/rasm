#include "c/cApi.h"

#include "rasm/renderer.h"
#include "rasm/tileStreamer.h"

#include <memory>
#include <string>
#include <vector>

using namespace rasm;

// The streamer, and the lists a call converts handles into, kept so a steady frame allocates nothing.
struct rasm_tile_streamer
{
    std::unique_ptr<TileStreamer> streamer;
    std::vector<TileHandle> wanted;
    std::vector<rasm_tile> toLoad;
    std::vector<EntityHandle> entities;
    std::vector<MeshHandle> meshes;
    std::vector<MaterialHandle> materials;
    std::vector<TextureHandle> textures;
};

namespace
{
    static_assert(RASM_TILE_REFINE_REPLACE == static_cast<uint32_t>(TileRefine::REPLACE));
    static_assert(RASM_TILE_REFINE_ADD == static_cast<uint32_t>(TileRefine::ADD));
    static_assert(RASM_TILE_STATE_EMPTY == static_cast<uint32_t>(TileState::EMPTY));
    static_assert(RASM_TILE_STATE_REQUESTED == static_cast<uint32_t>(TileState::REQUESTED));
    static_assert(RASM_TILE_STATE_LOADING == static_cast<uint32_t>(TileState::LOADING));
    static_assert(RASM_TILE_STATE_READY == static_cast<uint32_t>(TileState::READY));
    static_assert(RASM_TILE_STATE_FAILED == static_cast<uint32_t>(TileState::FAILED));
    constexpr uint32_t TILE_REFINE_COUNT = 2;
}

namespace
{
    rasm_tile_streamer_config toC(const TileStreamerConfig &config)
    {
        return {.size = sizeof(rasm_tile_streamer_config),
                .memory_budget = config.memoryBudget,
                .device_share = config.deviceShare,
                .max_tiles = config.maxTiles,
                .max_loads_per_frame = config.maxLoadsPerFrame,
                .max_loads_in_flight = config.maxLoadsInFlight,
                .max_drops_per_frame = config.maxDropsPerFrame,
                .retry_failed_after = config.retryFailedAfter};
    }

    TileStreamerConfig toCpp(const rasm_tile_streamer_config &config)
    {
        return {.memoryBudget = config.memory_budget,
                .deviceShare = config.device_share,
                .maxTiles = config.max_tiles,
                .maxLoadsPerFrame = config.max_loads_per_frame,
                .maxLoadsInFlight = config.max_loads_in_flight,
                .maxDropsPerFrame = config.max_drops_per_frame,
                .retryFailedAfter = config.retry_failed_after};
    }

    rasm_tile_desc toC(const TileDesc &desc)
    {
        return {.size = sizeof(rasm_tile_desc),
                .parent = c::toC(desc.parent),
                .center = c::toC(desc.center),
                .radius = desc.radius,
                .geometric_error = desc.geometricError,
                .refine = static_cast<uint32_t>(desc.refine),
                .estimated_bytes = desc.estimatedBytes};
    }

    TileDesc toCpp(const rasm_tile_desc &desc)
    {
        return {.parent = c::toCpp(desc.parent),
                .center = c::toCpp(desc.center),
                .radius = desc.radius,
                .geometricError = desc.geometric_error,
                .refine = static_cast<TileRefine>(desc.refine),
                .estimatedBytes = desc.estimated_bytes};
    }

    rasm_tile_streamer_stats toC(const TileStreamerStats &stats)
    {
        return {.size = sizeof(rasm_tile_streamer_stats),
                .tiles = stats.tiles,
                .wanted = stats.wanted,
                .ready = stats.ready,
                .loading = stats.loading,
                .failed = stats.failed,
                .drawn = stats.drawn,
                .fallbacks = stats.fallbacks,
                .holes = stats.holes,
                .waiting_for_budget = stats.waitingForBudget,
                .loads_started = stats.loadsStarted,
                .dropped = stats.dropped,
                .bytes = stats.bytes,
                .peak_bytes = stats.peakBytes,
                .budget = stats.budget,
                .device_budget = stats.deviceBudget,
                .device_usage = stats.deviceUsage,
                .total_loads = stats.totalLoads,
                .total_drops = stats.totalDrops};
    }

    // A count of items needs a pointer to them.
    bool checkArray(const void *items, uint32_t count, const char *function, const char *name)
    {
        return count == 0 || c::checkPointer(items, function, name);
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_tile_streamer_config_init(rasm_tile_streamer_config *config, uint32_t size)
try
{
    const rasm_tile_streamer_config defaults = toC(TileStreamerConfig{});
    return c::initSized(config, size, c::TILE_STREAMER_CONFIG_SIZE_1_3, &defaults, sizeof(defaults), __func__, "config");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_tile_desc_init(rasm_tile_desc *desc, uint32_t size)
try
{
    const rasm_tile_desc defaults = toC(TileDesc{});
    return c::initSized(desc, size, c::TILE_DESC_SIZE_1_3, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_tile_content_init(rasm_tile_content *content, uint32_t size)
try
{
    const rasm_tile_content defaults = {.size = sizeof(rasm_tile_content)};
    return c::initSized(content, size, c::TILE_CONTENT_SIZE_1_3, &defaults, sizeof(defaults), __func__, "content");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_tile_streamer_create(rasm_renderer *renderer, rasm_scene scene, const rasm_tile_streamer_config *config, rasm_tile_streamer **streamer)
try
{
    c::initialize();
    if (streamer == nullptr)
    {
        c::reportError(__func__, "streamer is NULL.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    *streamer = nullptr;
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_tile_streamer_config values = toC(TileStreamerConfig{});
    if (r == nullptr || (config != nullptr && !c::readSized(config, c::TILE_STREAMER_CONFIG_SIZE_1_3, &values, sizeof(values), __func__, "config")))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    auto created = std::make_unique<rasm_tile_streamer>();
    created->streamer = std::make_unique<TileStreamer>(*r, c::toCpp(scene), toCpp(values));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    *streamer = created.release();
    return RASM_OK;
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_tile_streamer_destroy(rasm_tile_streamer *streamer)
try
{
    delete streamer;
}
RASM_C_CATCH_VOID

rasm_tile RASM_CALL rasm_tile_streamer_create_tile(rasm_tile_streamer *streamer, const rasm_tile_desc *desc)
try
{
    rasm_tile_desc values = toC(TileDesc{});
    if (!c::checkPointer(streamer, __func__, "streamer") || !c::readSized(desc, c::TILE_DESC_SIZE_1_3, &values, sizeof(values), __func__, "desc") ||
        !c::checkEnum(values.refine, TILE_REFINE_COUNT, __func__, "desc->refine"))
        return {};
    return c::toC(streamer->streamer->createTile(toCpp(values)));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_tile_streamer_destroy_tile(rasm_tile_streamer *streamer, rasm_tile tile)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer") || !c::checkHandle(tile.id, __func__, "tile"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    streamer->streamer->destroyTile(c::toCpp(tile));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_tile_streamer_set_views(rasm_tile_streamer *streamer, const rasm_view *views, uint32_t view_count)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer") || !checkArray(views, view_count, __func__, "views"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (view_count > MAX_SCENE_VIEWS)
    {
        c::reportError(__func__, ("view_count " + std::to_string(view_count) + " is more than RASM_MAX_VIEWS.").c_str());
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    View converted[MAX_SCENE_VIEWS];
    if (!c::readViews(views, view_count, converted, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    streamer->streamer->setViews(converted, view_count);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

double RASM_CALL rasm_tile_streamer_get_screen_error(rasm_tile_streamer *streamer, rasm_dvec3 center, double radius, double geometric_error)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer"))
        return 0.0;
    return streamer->streamer->getScreenError(c::toCpp(center), radius, geometric_error);
}
RASM_C_CATCH(0.0)

double RASM_CALL rasm_tile_streamer_get_tile_screen_error(rasm_tile_streamer *streamer, rasm_tile tile)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer"))
        return 0.0;
    return streamer->streamer->getScreenError(c::toCpp(tile));
}
RASM_C_CATCH(0.0)

rasm_result RASM_CALL rasm_tile_streamer_update(rasm_tile_streamer *streamer, const rasm_tile *wanted, uint32_t wanted_count)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer") || !checkArray(wanted, wanted_count, __func__, "wanted"))
        return RASM_ERROR_INVALID_ARGUMENT;
    streamer->wanted.clear();
    for (uint32_t i = 0; i < wanted_count; ++i)
        streamer->wanted.push_back(c::toCpp(wanted[i]));
    const c::CallErrors errors;
    streamer->streamer->update(streamer->wanted.data(), wanted_count);
    uint32_t count = 0;
    const TileHandle *toLoad = streamer->streamer->getTilesToLoad(count);
    streamer->toLoad.clear();
    for (uint32_t i = 0; i < count; ++i)
        streamer->toLoad.push_back(c::toC(toLoad[i]));
    return errors.result();
}
RASM_C_CATCH_RESULT

const rasm_tile *RASM_CALL rasm_tile_streamer_get_tiles_to_load(rasm_tile_streamer *streamer, uint32_t *count)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer") || !c::checkPointer(count, __func__, "count"))
        return nullptr;
    *count = static_cast<uint32_t>(streamer->toLoad.size());
    return streamer->toLoad.data();
}
RASM_C_CATCH(nullptr)

rasm_result RASM_CALL rasm_tile_streamer_set_tile_content(rasm_tile_streamer *streamer, rasm_tile tile, const rasm_tile_content *content)
try
{
    rasm_tile_content values = {.size = sizeof(rasm_tile_content)};
    if (!c::checkPointer(streamer, __func__, "streamer") || !c::readSized(content, c::TILE_CONTENT_SIZE_1_3, &values, sizeof(values), __func__, "content") ||
        !checkArray(values.entities, values.entity_count, __func__, "content->entities") || !checkArray(values.meshes, values.mesh_count, __func__, "content->meshes") ||
        !checkArray(values.materials, values.material_count, __func__, "content->materials") || !checkArray(values.textures, values.texture_count, __func__, "content->textures"))
        return RASM_ERROR_INVALID_ARGUMENT;
    streamer->entities.clear();
    streamer->meshes.clear();
    streamer->materials.clear();
    streamer->textures.clear();
    for (uint32_t i = 0; i < values.entity_count; ++i)
        streamer->entities.push_back(c::toCpp(values.entities[i]));
    for (uint32_t i = 0; i < values.mesh_count; ++i)
        streamer->meshes.push_back(c::toCpp(values.meshes[i]));
    for (uint32_t i = 0; i < values.material_count; ++i)
        streamer->materials.push_back(c::toCpp(values.materials[i]));
    for (uint32_t i = 0; i < values.texture_count; ++i)
        streamer->textures.push_back(c::toCpp(values.textures[i]));
    const c::CallErrors errors;
    const bool accepted = streamer->streamer->setTileContent(c::toCpp(tile), {.entities = streamer->entities.data(),
                                                                              .entityCount = values.entity_count,
                                                                              .meshes = streamer->meshes.data(),
                                                                              .meshCount = values.mesh_count,
                                                                              .materials = streamer->materials.data(),
                                                                              .materialCount = values.material_count,
                                                                              .textures = streamer->textures.data(),
                                                                              .textureCount = values.texture_count});
    return accepted ? errors.result() : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_tile_state RASM_CALL rasm_tile_streamer_get_tile_state(rasm_tile_streamer *streamer, rasm_tile tile)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer"))
        return RASM_TILE_STATE_EMPTY;
    return static_cast<uint32_t>(streamer->streamer->getTileState(c::toCpp(tile)));
}
RASM_C_CATCH(RASM_TILE_STATE_EMPTY)

rasm_bool RASM_CALL rasm_tile_streamer_is_tile_drawn(rasm_tile_streamer *streamer, rasm_tile tile)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer"))
        return RASM_FALSE;
    return streamer->streamer->isTileDrawn(c::toCpp(tile)) ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)

rasm_result RASM_CALL rasm_tile_streamer_get_stats(rasm_tile_streamer *streamer, rasm_tile_streamer_stats *stats)
try
{
    if (!c::checkPointer(streamer, __func__, "streamer"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const rasm_tile_streamer_stats values = toC(streamer->streamer->getStats());
    return c::writeSized(stats, c::TILE_STREAMER_STATS_SIZE_1_3, &values, sizeof(values), __func__, "stats") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT
// NOLINTEND(readability-identifier-naming)
