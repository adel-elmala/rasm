#include "c/cApi.h"

#include "rasm/renderer.h"
#include "rasm/terrain.h"

#include <cmath>

using namespace rasm;

namespace
{
    rasm_terrain_tile_desc toC(const TerrainTileDesc &desc)
    {
        return {.size = sizeof(rasm_terrain_tile_desc),
                .center = c::toC(desc.center),
                .orientation = {desc.orientation.x, desc.orientation.y, desc.orientation.z, desc.orientation.w},
                .tile_size = desc.size,
                .terrain = c::toC(desc.terrain),
                .imagery = c::toC(desc.imagery),
                .imagery_rect = {.offset = c::toC(desc.imageryRect.offset), .rotation = desc.imageryRect.rotation, .scale = c::toC(desc.imageryRect.scale)},
                .roughness = desc.roughness};
    }

    TerrainTileDesc toCpp(const rasm_terrain_tile_desc &desc)
    {
        return {.center = c::toCpp(desc.center),
                .orientation = glm::quat(desc.orientation.w, desc.orientation.x, desc.orientation.y, desc.orientation.z),
                .size = desc.tile_size,
                .terrain = c::toCpp(desc.terrain),
                .imagery = c::toCpp(desc.imagery),
                .imageryRect = {.offset = c::toCpp(desc.imagery_rect.offset), .rotation = desc.imagery_rect.rotation, .scale = c::toCpp(desc.imagery_rect.scale)},
                .roughness = desc.roughness};
    }

    TerrainHeights toCpp(const rasm_terrain_heights &heights)
    {
        return {.unorm16 = heights.unorm16, .floats = heights.floats, .width = heights.width};
    }

    // One of the two sample arrays, and a width of a grid of 1 to MAX_TERRAIN_CELLS cells.
    bool checkHeights(const rasm_terrain_heights *heights, const char *function)
    {
        if (!c::checkPointer(heights, function, "heights"))
            return false;
        if ((heights->unorm16 == nullptr) == (heights->floats == nullptr) || heights->width < 4 || heights->width > MAX_TERRAIN_CELLS + 3)
        {
            c::reportError(function, "heights must have one of unorm16 and floats, and a width of 4 to RASM_MAX_TERRAIN_CELLS + 3.");
            return false;
        }
        return true;
    }

}

bool rasm::c::readTerrainTileDesc(const rasm_terrain_tile_desc *desc, TerrainTileDesc &tile, const char *function)
{
    rasm_terrain_tile_desc values = ::toC(TerrainTileDesc{});
    if (!readSized(desc, TERRAIN_TILE_DESC_SIZE_1_4, &values, sizeof(values), function, "desc") || !checkFinite(values.center, function, "desc->center"))
        return false;
    tile = ::toCpp(values);
    return true;
}

rasm_result RASM_CALL rasm_terrain_tile_desc_init(rasm_terrain_tile_desc *desc, uint32_t size)
try
{
    const rasm_terrain_tile_desc defaults = toC(TerrainTileDesc{});
    return c::initSized(desc, size, c::TERRAIN_TILE_DESC_SIZE_1_4, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_mesh RASM_CALL rasm_get_terrain_mesh(rasm_renderer *renderer, uint32_t cells)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return {};
    return c::toC(r->getTerrainMesh(cells));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_create_terrain_tile(rasm_renderer *renderer, const rasm_terrain_tile_desc *desc, rasm_entity *entity, rasm_material *material)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    TerrainTileDesc tile;
    if (r == nullptr || !c::readTerrainTileDesc(desc, tile, __func__) || !c::checkPointer(entity, __func__, "entity") || !c::checkPointer(material, __func__, "material"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    const TerrainTile made = r->createTerrainTile(tile);
    *entity = c::toC(made.entity);
    *material = c::toC(made.material);
    return made.entity.isValid() ? errors.result() : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_terrain_transform(const rasm_terrain_tile_desc *desc, rasm_transform_d *transform)
try
{
    TerrainTileDesc tile;
    if (!c::readTerrainTileDesc(desc, tile, __func__) || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *transform = c::toTransformD(getTerrainTransform(tile));
    return RASM_OK;
}
RASM_C_CATCH_RESULT

double RASM_CALL rasm_sample_terrain_height(const rasm_terrain_heights *heights, const rasm_terrain_material *terrain, double u, double v)
try
{
    if (!checkHeights(heights, __func__) || !c::checkPointer(terrain, __func__, "terrain"))
        return 0.0;
    return sampleTerrainHeight(toCpp(*heights), c::toCpp(*terrain), u, v);
}
RASM_C_CATCH(0.0)

rasm_result RASM_CALL rasm_get_terrain_position(const rasm_terrain_tile_desc *desc, const rasm_terrain_heights *heights, double u, double v, rasm_dvec3 *position)
try
{
    TerrainTileDesc tile;
    if (!c::readTerrainTileDesc(desc, tile, __func__) || !checkHeights(heights, __func__) || !c::checkPointer(position, __func__, "position"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *position = c::toC(getTerrainPosition(tile, toCpp(*heights), u, v));
    return RASM_OK;
}
RASM_C_CATCH_RESULT
