#include "c/cApi.h"

#include "rasm/renderer.h"

#include <string>

using namespace rasm;

namespace
{
    rasm_inspection toC(const Inspection &inspection)
    {
        return {.size = sizeof(rasm_inspection),
                .target = nullptr,
                .kind = static_cast<rasm_inspect_kind>(inspection.kind),
                .attachment = inspection.attachment,
                .face = inspection.face,
                .level = inspection.level,
                .sample = inspection.sample,
                .channel = inspection.channel,
                .exposure = inspection.exposure,
                .depth_range = inspection.depthRange,
                .entity = c::toC(inspection.entity)};
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_set_debug_view(rasm_renderer *renderer, rasm_debug_view view)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkEnum(view, RASM_DEBUG_VIEW_MESHLETS + 1, __func__, "view"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setDebugView(static_cast<DebugView>(view));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_debug_view RASM_CALL rasm_get_debug_view(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? static_cast<rasm_debug_view>(r->getDebugView()) : RASM_DEBUG_VIEW_NONE;
}
RASM_C_CATCH(RASM_DEBUG_VIEW_NONE)

rasm_result RASM_CALL rasm_set_culling_frozen(rasm_renderer *renderer, rasm_bool frozen)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    r->setCullingFrozen(frozen != 0);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_meshlet_path(rasm_renderer *renderer, rasm_meshlet_path path)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkEnum(path, RASM_MESHLET_PATH_AUTO + 1, __func__, "path"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setMeshletPath(static_cast<MeshletPath>(path));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_meshlet_path RASM_CALL rasm_get_meshlet_path(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? static_cast<rasm_meshlet_path>(r->getMeshletPath()) : RASM_MESHLET_PATH_AUTO;
}
RASM_C_CATCH(RASM_MESHLET_PATH_AUTO)

rasm_result RASM_CALL rasm_inspection_init(rasm_inspection *inspection, uint32_t size)
try
{
    const rasm_inspection defaults = toC(Inspection{});
    return c::initSized(inspection, size, c::INSPECTION_SIZE_1_18, &defaults, sizeof(defaults), __func__, "inspection");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_inspection(rasm_renderer *renderer, const rasm_inspection *inspection)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_inspection values = toC(Inspection{});
    if (r == nullptr || !c::readSized(inspection, c::INSPECTION_SIZE_1_18, &values, sizeof(values), __func__, "inspection") ||
        !c::checkEnum(values.kind, RASM_INSPECT_KIND_LIGHT_CLUSTERS + 1, __func__, "inspection->kind"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const Inspection picked = {.target = values.target != nullptr ? values.target : "",
                               .kind = static_cast<InspectKind>(values.kind),
                               .attachment = values.attachment,
                               .face = values.face,
                               .level = values.level,
                               .sample = values.sample,
                               .channel = values.channel,
                               .exposure = values.exposure,
                               .depthRange = values.depth_range,
                               .entity = c::toCpp(values.entity)};
    const c::CallErrors errors;
    r->setInspection(picked);
    return errors.result();
}
RASM_C_CATCH_RESULT

// The struct's fields as far as its size reaches, and the target's name by the snprintf rule; target points to the
// caller's buffer.
uint32_t RASM_CALL rasm_get_inspection(rasm_renderer *renderer, rasm_inspection *inspection, char *target_buffer, uint32_t capacity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || (capacity > 0 && !c::checkPointer(target_buffer, __func__, "target_buffer")))
        return 0;
    const Inspection &current = r->getInspection();
    rasm_inspection values = toC(current);
    values.target = capacity > 0 ? target_buffer : nullptr;
    if (!c::writeSized(inspection, c::INSPECTION_SIZE_1_18, &values, sizeof(values), __func__, "inspection"))
        return 0;
    return c::copyString(current.target, target_buffer, capacity);
}
RASM_C_CATCH(0)
// NOLINTEND(readability-identifier-naming)
