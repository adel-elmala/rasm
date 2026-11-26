#include "c/cApi.h"

#include "rasm/glyphs.h"

#include <cstddef>

using namespace rasm;

namespace
{
    // The arrays are glm's layout, so the builder reads them in place: a rasm_quat is x, y, z, w, as glm::quat.
    static_assert(sizeof(rasm_vec3) == sizeof(glm::vec3));
    static_assert(sizeof(rasm_quat) == sizeof(glm::quat));
    static_assert(offsetof(glm::quat, x) == offsetof(rasm_quat, x) && offsetof(glm::quat, w) == offsetof(rasm_quat, w));
    static_assert(RASM_MAX_GLYPH_CHUNK_GLYPHS == MAX_GLYPH_CHUNK_GLYPHS);
    static_assert(RASM_GLYPH_SHAPE_ARROW == static_cast<uint32_t>(GlyphShape::ARROW));
    static_assert(RASM_GLYPH_SHAPE_SPHERE == static_cast<uint32_t>(GlyphShape::SPHERE));
    static_assert(RASM_GLYPH_SHAPE_CUBE == static_cast<uint32_t>(GlyphShape::CUBE));

    constexpr rasm_quat IDENTITY = {.x = 0.0f, .y = 0.0f, .z = 0.0f, .w = 1.0f};
}

rasm_result RASM_CALL rasm_glyph_chunk_desc_init(rasm_glyph_chunk_desc *desc, uint32_t size)
try
{
    const rasm_glyph_chunk_desc defaults = {.size = sizeof(rasm_glyph_chunk_desc)};
    return c::initSized(desc, size, c::GLYPH_CHUNK_DESC_SIZE_1_14, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_build_glyph_chunk(const rasm_glyph_chunk_desc *desc, rasm_mesh_build **build)
try
{
    if (!c::checkPointer(build, __func__, "build"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *build = nullptr;
    rasm_glyph_chunk_desc values = {};
    if (!c::readSized(desc, c::GLYPH_CHUNK_DESC_SIZE_1_14, &values, sizeof(values), __func__, "desc") || !c::checkPointer(values.positions, __func__, "desc->positions"))
        return RASM_ERROR_INVALID_ARGUMENT;
    MeshBuild made = buildGlyphChunk({.shape = c::toCpp(values.shape),
                                      .positions = reinterpret_cast<const glm::vec3 *>(values.positions),
                                      .orientations = reinterpret_cast<const glm::quat *>(values.orientations),
                                      .scales = reinterpret_cast<const glm::vec3 *>(values.scales),
                                      .colors = values.colors,
                                      .scalars = values.scalars,
                                      .count = values.count});
    if (!made.isValid())
        return RASM_ERROR_INVALID_ARGUMENT;
    *build = new rasm_mesh_build{std::move(made)};
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_build_glyph_shape(rasm_glyph_shape shape, uint32_t segments, rasm_mesh_build **build)
try
{
    if (!c::checkPointer(build, __func__, "build"))
        return RASM_ERROR_INVALID_ARGUMENT;
    *build = nullptr;
    if (!c::checkEnum(shape, RASM_GLYPH_SHAPE_CUBE + 1, __func__, "shape"))
        return RASM_ERROR_INVALID_ARGUMENT;
    MeshBuild made = buildGlyphShape(static_cast<GlyphShape>(shape), segments);
    if (!made.isValid())
        return RASM_ERROR_FAILED;
    *build = new rasm_mesh_build{std::move(made)};
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_quat RASM_CALL rasm_get_glyph_orientation(rasm_vec3 direction)
try
{
    const glm::quat q = getGlyphOrientation(glm::vec3(direction.x, direction.y, direction.z));
    return {.x = q.x, .y = q.y, .z = q.z, .w = q.w};
}
RASM_C_CATCH(IDENTITY)
