#include "c/cApi.h"

#include "rasm/pointCloud.h"
#include "rasm/pointCloudOctree.h"
#include "rasm/renderer.h"

#include <memory>
#include <string>

using namespace rasm;

namespace
{
    // A rasm_vec3 or rasm_vec4 array is glm's layout, so the builders read it in place.
    static_assert(sizeof(rasm_vec3) == sizeof(glm::vec3));
    static_assert(sizeof(rasm_vec4) == sizeof(glm::vec4));
    static_assert(sizeof(rasm_dvec3) == sizeof(glm::dvec3));
}

namespace
{
    bool checkNode(const rasm_point_cloud_octree *octree, uint32_t node, const char *function)
    {
        if (!c::checkPointer(octree, function, "octree"))
            return false;
        if (node < octree->octree.nodes.size())
            return true;
        c::reportError(function, ("node " + std::to_string(node) + " is past the octree's " + std::to_string(octree->octree.nodes.size()) + " nodes.").c_str());
        return false;
    }
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
    MeshBuild made = buildPointCloudChunk(
        {.positions = reinterpret_cast<const glm::vec3 *>(values.positions), .colors = values.colors, .scalars = values.scalars, .count = values.count, .spacing = values.spacing});
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

rasm_result RASM_CALL rasm_point_chunk_lod_init(rasm_point_chunk_lod *lod, uint32_t size)
try
{
    const rasm_point_chunk_lod defaults = {.size = sizeof(rasm_point_chunk_lod), .drawn = PointChunkLod{}.drawn};
    return c::initSized(lod, size, c::POINT_CHUNK_LOD_SIZE_1_12, &defaults, sizeof(defaults), __func__, "lod");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_point_chunk_lod(rasm_renderer *renderer, rasm_entity entity, const rasm_point_chunk_lod *lod)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_point_chunk_lod values = {};
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity") || !c::readSized(lod, c::POINT_CHUNK_LOD_SIZE_1_12, &values, sizeof(values), __func__, "lod"))
        return RASM_ERROR_INVALID_ARGUMENT;
    PointChunkLod made{.drawn = values.drawn};
    for (uint32_t octant = 0; octant < 8; ++octant)
        made.finer[octant] = values.finer[octant];
    const c::CallErrors errors;
    r->setPointChunkLod(c::toCpp(entity), made);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_point_cloud_octree_desc_init(rasm_point_cloud_octree_desc *desc, uint32_t size)
try
{
    const PointCloudOctreeDesc cpp;
    const rasm_point_cloud_octree_desc defaults = {.size = sizeof(rasm_point_cloud_octree_desc), .cells = cpp.cells, .max_node_points = cpp.maxNodePoints};
    return c::initSized(desc, size, c::POINT_CLOUD_OCTREE_DESC_SIZE_1_12, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_build_point_cloud_octree(const rasm_point_cloud_octree_desc *desc, rasm_point_cloud_octree **octree)
try
{
    if (!c::checkPointer(octree, __func__, "octree"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *octree = nullptr;
    rasm_point_cloud_octree_desc values = {};
    if (!c::readSized(desc, c::POINT_CLOUD_OCTREE_DESC_SIZE_1_12, &values, sizeof(values), __func__, "desc") || !c::checkPointer(values.positions, __func__, "desc->positions"))
        return RASM_ERROR_INVALID_ARGUMENT;
    auto made = std::make_unique<rasm_point_cloud_octree>();
    const c::CallErrors errors;
    if (!buildPointCloudOctree({.positions = reinterpret_cast<const glm::dvec3 *>(values.positions),
                                .colors = values.colors,
                                .scalars = values.scalars,
                                .count = values.count,
                                .cells = values.cells,
                                .maxNodePoints = values.max_node_points},
                               made->octree))
        return RASM_ERROR_INVALID_ARGUMENT;
    *octree = made.release();
    return RASM_OK;
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_point_cloud_octree_destroy(rasm_point_cloud_octree *octree)
try
{
    delete octree;
}
RASM_C_CATCH_VOID

uint32_t RASM_CALL rasm_get_point_cloud_node_count(const rasm_point_cloud_octree *octree)
try
{
    if (!c::checkPointer(octree, __func__, "octree"))
        return 0;
    return static_cast<uint32_t>(octree->octree.nodes.size());
}
RASM_C_CATCH(0)

rasm_result RASM_CALL rasm_get_point_cloud_node(const rasm_point_cloud_octree *octree, uint32_t node, rasm_point_cloud_node *record)
try
{
    if (!checkNode(octree, node, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    const PointCloudNode &cpp = octree->octree.nodes[node];
    rasm_point_cloud_node values = {.size = sizeof(rasm_point_cloud_node),
                                    .center = c::toC(cpp.center),
                                    .half_size = cpp.halfSize,
                                    .bounds_center = c::toC(cpp.boundsCenter),
                                    .bounds_radius = cpp.boundsRadius,
                                    .spacing = cpp.spacing,
                                    .geometric_error = cpp.geometricError,
                                    .parent = cpp.parent,
                                    .first_point = cpp.firstPoint,
                                    .point_count = cpp.pointCount,
                                    .level = cpp.level};
    for (uint32_t octant = 0; octant < 8; ++octant)
        values.children[octant] = cpp.children[octant];
    return c::writeSized(record, c::POINT_CLOUD_NODE_SIZE_1_12, &values, sizeof(values), __func__, "record") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_point_cloud_node_chunk(const rasm_point_cloud_octree *octree, uint32_t node, rasm_point_cloud_chunk_desc *desc)
try
{
    if (!checkNode(octree, node, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    const PointCloudChunkDesc chunk = getPointCloudNodeChunk(octree->octree, node);
    const rasm_point_cloud_chunk_desc values = {.size = sizeof(rasm_point_cloud_chunk_desc),
                                                .positions = reinterpret_cast<const rasm_vec3 *>(chunk.positions),
                                                .colors = chunk.colors,
                                                .scalars = chunk.scalars,
                                                .count = chunk.count,
                                                .spacing = chunk.spacing};
    return c::writeSized(desc, c::POINT_CLOUD_CHUNK_DESC_SIZE_1_10, &values, sizeof(values), __func__, "desc") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

const uint32_t *RASM_CALL rasm_get_point_cloud_octree_indices(const rasm_point_cloud_octree *octree, uint32_t *count)
try
{
    if (count != nullptr)
        *count = 0;
    if (!c::checkPointer(octree, __func__, "octree") || !c::checkPointer(count, __func__, "count"))
        return nullptr;
    *count = static_cast<uint32_t>(octree->octree.indices.size());
    return octree->octree.indices.data();
}
RASM_C_CATCH(nullptr)
