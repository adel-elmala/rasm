#include "c/cApi.h"

#include "rasm/renderer.h"
#include "rasm/vector.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace rasm;

// The geometry the vector functions append to.
struct rasm_vector_geometry
{
    VectorGeometry geometry;
};

namespace
{
    // A rasm_dvec3 array is glm::dvec3's layout, so the builders read it in place.
    static_assert(sizeof(rasm_dvec3) == sizeof(glm::dvec3));
    static_assert(RASM_LINE_JOIN_MITER == static_cast<uint32_t>(LineJoin::MITER));
    static_assert(RASM_LINE_JOIN_BEVEL == static_cast<uint32_t>(LineJoin::BEVEL));
    static_assert(RASM_LINE_JOIN_ROUND == static_cast<uint32_t>(LineJoin::ROUND));
    static_assert(RASM_LINE_CAP_BUTT == static_cast<uint32_t>(LineCap::BUTT));
    static_assert(RASM_LINE_CAP_SQUARE == static_cast<uint32_t>(LineCap::SQUARE));
    static_assert(RASM_LINE_CAP_ROUND == static_cast<uint32_t>(LineCap::ROUND));
    static_assert(RASM_MAX_VECTOR_MITER == MAX_VECTOR_MITER);
    static_assert(sizeof(rasm_vec4) == sizeof(glm::vec4));
}

namespace
{
    rasm_mesh_build_options toC(const MeshBuildOptions &options)
    {
        return {.size = sizeof(rasm_mesh_build_options), .lods = options.lods ? RASM_TRUE : RASM_FALSE, .meshlets = options.meshlets ? RASM_TRUE : RASM_FALSE};
    }

    rasm_line_shape toC(const LineShape &shape)
    {
        return {.size = sizeof(rasm_line_shape), .join = static_cast<uint32_t>(shape.join), .cap = static_cast<uint32_t>(shape.cap), .miter_limit = shape.miterLimit, .drape_width = shape.drapeWidth};
    }

    rasm_vector_placement toC(const VectorPlacement &placement)
    {
        return {.size = sizeof(rasm_vector_placement), .up = c::toC(placement.up)};
    }

    const glm::dvec3 *toCpp(const rasm_dvec3 *points)
    {
        return reinterpret_cast<const glm::dvec3 *>(points);
    }

    // The shape and placement a call reads, from its sized structs or the defaults; the drape's tile lives in tile.
    bool readShape(const rasm_line_shape *shape, LineShape &result, const char *function)
    {
        rasm_line_shape values = toC(LineShape{});
        if (shape != nullptr && (!c::readSized(shape, c::LINE_SHAPE_SIZE_1_5, &values, sizeof(values), function, "shape") ||
                                 !c::checkEnum(values.join, RASM_LINE_JOIN_ROUND + 1, function, "shape->join") || !c::checkEnum(values.cap, RASM_LINE_CAP_ROUND + 1, function, "shape->cap")))
            return false;
        if (!std::isfinite(values.drape_width) || values.drape_width < 0.0f)
        {
            c::reportError(function, "shape->drape_width must be finite and 0 or more.");
            return false;
        }
        result = {.join = static_cast<LineJoin>(values.join), .cap = static_cast<LineCap>(values.cap), .miterLimit = values.miter_limit, .drapeWidth = values.drape_width};
        return true;
    }

    bool readPlacement(const rasm_vector_placement *placement, VectorPlacement &result, TerrainTileDesc &tile, const char *function)
    {
        rasm_vector_placement values = toC(VectorPlacement{});
        if (placement != nullptr && !c::readSized(placement, c::VECTOR_PLACEMENT_SIZE_1_5, &values, sizeof(values), function, "placement"))
            return false;
        result = {.up = c::toCpp(values.up)};
        if (values.drape_tile == nullptr)
            return true;
        const rasm_terrain_heights &heights = values.drape_heights;
        if ((heights.unorm16 == nullptr) == (heights.floats == nullptr) || heights.width < 4 || heights.width > MAX_TERRAIN_CELLS + 3)
        {
            c::reportError(function, "placement->drape_heights must have one of unorm16 and floats, and a width of 4 to RASM_MAX_TERRAIN_CELLS + 3.");
            return false;
        }
        if (!c::readTerrainTileDesc(values.drape_tile, tile, function))
            return false;
        result.drapeTile = &tile;
        result.drapeHeights = {.unorm16 = heights.unorm16, .floats = heights.floats, .width = heights.width};
        return true;
    }

    bool checkArray(const void *array, uint32_t count, const char *function, const char *name)
    {
        return count == 0 || c::checkPointer(array, function, name);
    }

    // A sized rasm_label_desc array, each element the size of the first, into labels.
    bool readLabels(const rasm_label_desc *labels, uint32_t labelCount, std::vector<LabelDesc> &converted, const char *function)
    {
        uint32_t stride = 0;
        if (labelCount > 0)
            std::memcpy(&stride, labels, sizeof(stride));
        converted.resize(labelCount);
        for (uint32_t i = 0; i < labelCount; ++i)
        {
            const auto *element = reinterpret_cast<const uint8_t *>(labels) + static_cast<size_t>(i) * stride;
            uint32_t size = 0;
            std::memcpy(&size, element, sizeof(size));
            if (size != stride)
            {
                c::reportError(function, ("labels[" + std::to_string(i) + "].size is " + std::to_string(size) + ", not the first label's " + std::to_string(stride) + ".").c_str());
                return false;
            }
            rasm_label_desc values = {};
            if (!c::readSized(element, c::LABEL_DESC_SIZE_1_6, &values, sizeof(values), function, "label") || !c::checkEnum(values.anchor, RASM_LABEL_ANCHOR_RIGHT + 1, function, "label->anchor") ||
                !c::checkFinite(values.position, function, "label->position"))
                return false;
            converted[i] = {.position = c::toCpp(values.position),
                            .text = values.text,
                            .priority = values.priority,
                            .anchor = static_cast<LabelAnchor>(values.anchor),
                            .offset = c::toCpp(values.offset),
                            .featureId = values.feature_id};
        }
        return true;
    }

    // The sum of a count array: the points or rings it covers.
    uint64_t sum(const uint32_t *counts, uint32_t count)
    {
        uint64_t total = 0;
        for (uint32_t i = 0; i < count; ++i)
            total += counts[i];
        return total;
    }
}

rasm_result RASM_CALL rasm_mesh_build_options_init(rasm_mesh_build_options *options, uint32_t size)
try
{
    const rasm_mesh_build_options defaults = toC(MeshBuildOptions{});
    return c::initSized(options, size, c::MESH_BUILD_OPTIONS_SIZE_1_5, &defaults, sizeof(defaults), __func__, "options");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_build_mesh(const rasm_vertex *vertices, uint32_t vertex_count, const uint32_t *indices, uint32_t index_count, const uint32_t *colors, const rasm_mesh_build_options *options,
                                      rasm_mesh_build **build)
try
{
    if (!c::checkPointer(build, __func__, "build"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *build = nullptr;
    rasm_mesh_build_options values = toC(MeshBuildOptions{});
    if (!checkArray(vertices, vertex_count, __func__, "vertices") || !checkArray(indices, index_count, __func__, "indices") ||
        (options != nullptr && !c::readSized(options, c::MESH_BUILD_OPTIONS_SIZE_1_5, &values, sizeof(values), __func__, "options")))
        return RASM_ERROR_INVALID_ARGUMENT;
    std::vector<Vertex> meshVertices(vertex_count);
    if (vertex_count > 0)
        std::memcpy(meshVertices.data(), vertices, sizeof(Vertex) * vertex_count);
    std::vector<uint32_t> meshColors;
    if (colors != nullptr)
        meshColors.assign(colors, colors + vertex_count);
    MeshBuild made = buildMesh(std::move(meshVertices), std::vector<uint32_t>(indices, indices + index_count), std::move(meshColors), {.lods = values.lods != 0, .meshlets = values.meshlets != 0});
    if (!made.isValid())
        return RASM_ERROR_FAILED;
    *build = new rasm_mesh_build{std::move(made)};
    return RASM_OK;
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_mesh_build_destroy(rasm_mesh_build *build)
try
{
    delete build;
}
RASM_C_CATCH_VOID

rasm_mesh RASM_CALL rasm_create_mesh_from_build(rasm_renderer *renderer, rasm_mesh_build *build)
try
{
    // The build is the call's from here on, whatever happens.
    const std::unique_ptr<rasm_mesh_build> owned(build);
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkPointer(build, __func__, "build"))
        return {};
    return c::toC(r->createMesh(std::move(owned->build)));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_line_shape_init(rasm_line_shape *shape, uint32_t size)
try
{
    const rasm_line_shape defaults = toC(LineShape{});
    return c::initSized(shape, size, c::LINE_SHAPE_SIZE_1_5, &defaults, sizeof(defaults), __func__, "shape");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_vector_placement_init(rasm_vector_placement *placement, uint32_t size)
try
{
    const rasm_vector_placement defaults = toC(VectorPlacement{});
    return c::initSized(placement, size, c::VECTOR_PLACEMENT_SIZE_1_5, &defaults, sizeof(defaults), __func__, "placement");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_vector_geometry_create(rasm_vector_geometry **geometry)
try
{
    if (!c::checkPointer(geometry, __func__, "geometry"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *geometry = new rasm_vector_geometry{};
    return RASM_OK;
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_vector_geometry_destroy(rasm_vector_geometry *geometry)
try
{
    delete geometry;
}
RASM_C_CATCH_VOID

rasm_result RASM_CALL rasm_vector_geometry_get_size(const rasm_vector_geometry *geometry, uint32_t *vertex_count, uint32_t *index_count)
try
{
    if (!c::checkPointer(geometry, __func__, "geometry"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (vertex_count != nullptr)
        *vertex_count = static_cast<uint32_t>(geometry->geometry.vertices.size());
    if (index_count != nullptr)
        *index_count = static_cast<uint32_t>(geometry->geometry.indices.size());
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_vector_add_lines(rasm_vector_geometry *geometry, rasm_dvec3 origin, const rasm_dvec3 *points, const uint32_t *line_sizes, uint32_t line_count, const rasm_line_shape *shape,
                                            const rasm_vector_placement *placement, const uint32_t *feature_ids)
try
{
    LineShape lineShape;
    VectorPlacement place;
    TerrainTileDesc tile;
    if (!c::checkPointer(geometry, __func__, "geometry") || !checkArray(line_sizes, line_count, __func__, "line_sizes") || !c::checkFinite(origin, __func__, "origin") ||
        !readShape(shape, lineShape, __func__) || !readPlacement(placement, place, tile, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (line_count > 0 && !checkArray(points, sum(line_sizes, line_count) > 0 ? 1 : 0, __func__, "points"))
        return RASM_ERROR_INVALID_ARGUMENT;
    addVectorLines(geometry->geometry, c::toCpp(origin), toCpp(points), line_sizes, line_count, lineShape, place, feature_ids);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_vector_add_polygons(rasm_vector_geometry *geometry, rasm_dvec3 origin, const rasm_dvec3 *points, const uint32_t *ring_sizes, const uint32_t *ring_counts, uint32_t polygon_count,
                                               const rasm_vector_placement *placement, const uint32_t *feature_ids)
try
{
    VectorPlacement place;
    TerrainTileDesc tile;
    if (!c::checkPointer(geometry, __func__, "geometry") || !checkArray(ring_counts, polygon_count, __func__, "ring_counts") || !c::checkFinite(origin, __func__, "origin") ||
        !readPlacement(placement, place, tile, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    const uint64_t rings = polygon_count > 0 ? sum(ring_counts, polygon_count) : 0;
    if (rings > 0 && (!c::checkPointer(ring_sizes, __func__, "ring_sizes") || (sum(ring_sizes, static_cast<uint32_t>(rings)) > 0 && !c::checkPointer(points, __func__, "points"))))
        return RASM_ERROR_INVALID_ARGUMENT;
    addVectorPolygons(geometry->geometry, c::toCpp(origin), toCpp(points), ring_sizes, ring_counts, polygon_count, place, feature_ids);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_vector_add_points(rasm_vector_geometry *geometry, rasm_dvec3 origin, const rasm_dvec3 *points, uint32_t point_count, const rasm_vector_placement *placement,
                                             const uint32_t *feature_ids, const rasm_vec4 *icon_rects)
try
{
    VectorPlacement place;
    TerrainTileDesc tile;
    if (!c::checkPointer(geometry, __func__, "geometry") || !checkArray(points, point_count, __func__, "points") || !c::checkFinite(origin, __func__, "origin") ||
        !readPlacement(placement, place, tile, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    addVectorPoints(geometry->geometry, c::toCpp(origin), toCpp(points), point_count, place, feature_ids, reinterpret_cast<const glm::vec4 *>(icon_rects));
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_vector_add_labels(rasm_vector_geometry *geometry, rasm_dvec3 origin, const rasm_font_atlas *atlas, const rasm_label_desc *labels, uint32_t label_count,
                                             const rasm_vector_placement *placement)
try
{
    VectorPlacement place;
    TerrainTileDesc tile;
    std::vector<LabelDesc> converted;
    if (!c::checkPointer(geometry, __func__, "geometry") || !c::checkPointer(atlas, __func__, "atlas") || !checkArray(labels, label_count, __func__, "labels") ||
        !c::checkFinite(origin, __func__, "origin") || !readPlacement(placement, place, tile, __func__) || !readLabels(labels, label_count, converted, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    addVectorLabels(geometry->geometry, c::toCpp(origin), atlas->atlas, converted.data(), label_count, place);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_build_vector_mesh(rasm_vector_geometry *geometry, rasm_mesh_build **build)
try
{
    if (!c::checkPointer(geometry, __func__, "geometry") || !c::checkPointer(build, __func__, "build"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *build = nullptr;
    MeshBuild made = buildVectorMesh(std::move(geometry->geometry));
    geometry->geometry = {};
    if (!made.isValid())
        return RASM_ERROR_FAILED;
    *build = new rasm_mesh_build{std::move(made)};
    return RASM_OK;
}
RASM_C_CATCH_RESULT
