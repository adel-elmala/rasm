#include "c/cApi.h"

#include "rasm/pointCloud.h"
#include "rasm/renderer.h"

using namespace rasm;

namespace
{
    // A rasm_vec3 or rasm_vec4 array is glm's layout, so the builders read it in place.
    static_assert(sizeof(rasm_vec3) == sizeof(glm::vec3));
    static_assert(sizeof(rasm_vec4) == sizeof(glm::vec4));
}

rasm_texture RASM_CALL rasm_create_colormap(rasm_renderer *renderer, const rasm_vec4 *colors, uint32_t count)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkPointer(colors, __func__, "colors"))
        return {};
    return c::toC(r->createColormap(reinterpret_cast<const glm::vec4 *>(colors), count));
}
RASM_C_CATCH({})

rasm_texture RASM_CALL rasm_create_colormap_preset(rasm_renderer *renderer, rasm_colormap_preset preset)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkEnum(preset, RASM_COLORMAP_GRAY + 1, __func__, "preset"))
        return {};
    return c::toC(r->createColormap(static_cast<ColormapPreset>(preset)));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_point_cloud_chunk_desc_init(rasm_point_cloud_chunk_desc *desc, uint32_t size)
try
{
    const rasm_point_cloud_chunk_desc defaults = {.size = sizeof(rasm_point_cloud_chunk_desc)};
    return c::initSized(desc, size, c::POINT_CLOUD_CHUNK_DESC_SIZE_1_10, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_build_point_cloud_chunk(const rasm_point_cloud_chunk_desc *desc, rasm_mesh_build **build)
try
{
    if (!c::checkPointer(build, __func__, "build"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *build = nullptr;
    rasm_point_cloud_chunk_desc values = {};
    if (!c::readSized(desc, c::POINT_CLOUD_CHUNK_DESC_SIZE_1_10, &values, sizeof(values), __func__, "desc") || !c::checkPointer(values.positions, __func__, "desc->positions"))
        return RASM_ERROR_INVALID_ARGUMENT;
    MeshBuild made = buildPointCloudChunk({.positions = reinterpret_cast<const glm::vec3 *>(values.positions), .colors = values.colors, .scalars = values.scalars, .count = values.count});
    if (!made.isValid())
        return RASM_ERROR_INVALID_ARGUMENT;
    *build = new rasm_mesh_build{std::move(made)};
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_compute_points(rasm_renderer *renderer, rasm_bool on)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setComputePoints(on != 0);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_bool RASM_CALL rasm_get_compute_points(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr && r->getComputePoints() ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)
