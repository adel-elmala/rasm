#include "c/cApi.h"

#include "rasm/picking.h"
#include "rasm/renderer.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace rasm;

namespace
{
    static_assert(RASM_MAX_PICK_SIZE == MAX_PICK_SIZE);
    static_assert(RASM_PICK_SKIP_MESHES == PICK_SKIP_MESHES);
    static_assert(RASM_PICK_SKIP_BLEND == PICK_SKIP_BLEND);
    static_assert(RASM_PICK_SKIP_TERRAIN == PICK_SKIP_TERRAIN);
    static_assert(RASM_PICK_SKIP_VECTORS == PICK_SKIP_VECTORS);
    static_assert(RASM_PICK_SKIP_LABELS == PICK_SKIP_LABELS);
    static_assert(RASM_PICK_SKIP_OVERLAYS == PICK_SKIP_OVERLAYS);
    static_assert(RASM_PICK_SKIP_ALL == PICK_SKIP_ALL);
    static_assert(RASM_PICK_HIT_FEATURE == PICK_HIT_FEATURE);
    static_assert(RASM_PICK_HIT_LABEL == PICK_HIT_LABEL);
    static_assert(RASM_PICK_HIT_OVERLAY == PICK_HIT_OVERLAY);
    static_assert(RASM_PICK_HIT_TERRAIN == PICK_HIT_TERRAIN);
    static_assert(RASM_PICK_HIT_BACK_FACE == PICK_HIT_BACK_FACE);
}

namespace
{
    rasm_pick_desc toC(const PickDesc &desc)
    {
        return {.size = sizeof(rasm_pick_desc),
                .rect = {.x = desc.rect.x, .y = desc.rect.y, .width = desc.rect.width, .height = desc.rect.height},
                .min_alpha = desc.minAlpha,
                .skip = desc.skip};
    }

    rasm_pick_hit toC(const PickHit &hit)
    {
        return {.size = sizeof(rasm_pick_hit),
                .entity = c::toC(hit.entity),
                .sub_mesh = hit.subMesh,
                .feature = hit.feature,
                .flags = hit.flags,
                .pixels = hit.pixels,
                .x = hit.x,
                .y = hit.y,
                .position = c::toC(hit.position),
                .normal = c::toC(hit.normal),
                .distance = hit.distance};
    }

    // The desc's fields the caller's size reaches, over the defaults; NULL is the defaults.
    bool readDesc(const rasm_pick_desc *desc, PickDesc &converted, const char *function)
    {
        rasm_pick_desc values = toC(PickDesc{});
        if (desc != nullptr && !c::readSized(desc, c::PICK_DESC_SIZE_1_7, &values, sizeof(values), function, "desc"))
            return false;
        converted = {.rect = {.x = values.rect.x, .y = values.rect.y, .width = values.rect.width, .height = values.rect.height}, .minAlpha = values.min_alpha, .skip = values.skip};
        return true;
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_pick_desc_init(rasm_pick_desc *desc, uint32_t size)
try
{
    const rasm_pick_desc defaults = toC(PickDesc{});
    return c::initSized(desc, size, c::PICK_DESC_SIZE_1_7, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_pick_hit_init(rasm_pick_hit *hit, uint32_t size)
try
{
    const rasm_pick_hit defaults = toC(PickHit{});
    return c::initSized(hit, size, c::PICK_HIT_SIZE_1_7, &defaults, sizeof(defaults), __func__, "hit");
}
RASM_C_CATCH_RESULT

rasm_pick RASM_CALL rasm_pick_view(rasm_renderer *renderer, const rasm_view *view, const rasm_pick_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    View converted;
    PickDesc pickDesc;
    if (r == nullptr || !c::checkPointer(view, __func__, "view") || !c::readViews(view, 1, &converted, __func__) || !readDesc(desc, pickDesc, __func__))
        return {};
    const c::CallErrors errors;
    return c::toC(r->pick(converted, pickDesc));
}
RASM_C_CATCH({})

rasm_pick RASM_CALL rasm_pick_target(rasm_renderer *renderer, rasm_scene scene, rasm_camera camera, rasm_render_target target, const rasm_pick_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    PickDesc pickDesc;
    if (r == nullptr || !readDesc(desc, pickDesc, __func__))
        return {};
    const c::CallErrors errors;
    return c::toC(r->pick(c::toCpp(scene), c::toCpp(camera), c::toCpp(target), pickDesc));
}
RASM_C_CATCH({})

rasm_bool RASM_CALL rasm_is_pick_ready(rasm_renderer *renderer, rasm_pick pick)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr && r->isPickReady(c::toCpp(pick)) ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)

// Up to 64 hits go through an array on the stack, so a hover pick each frame allocates nothing; more take a vector.
rasm_result RASM_CALL rasm_take_pick(rasm_renderer *renderer, rasm_pick pick, rasm_pick_hit *hits, uint32_t capacity, uint32_t *hit_count)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || (capacity > 0 && !c::checkPointer(hits, __func__, "hits")))
        return RASM_ERROR_INVALID_ARGUMENT;
    uint32_t stride = 0;
    if (capacity > 0)
    {
        std::memcpy(&stride, hits, sizeof(stride));
        if (stride < c::PICK_HIT_SIZE_1_7)
        {
            c::reportError(__func__, ("hits[0].size is " + std::to_string(stride) + ", less than a rasm_pick_hit of version 1.7: set it (rasm_pick_hit_init).").c_str());
            return RASM_ERROR_INVALID_ARGUMENT;
        }
    }
    constexpr uint32_t STACK_HITS = 64;
    PickHit onStack[STACK_HITS];
    std::vector<PickHit> onHeap(capacity > STACK_HITS ? capacity : 0);
    PickHit *converted = capacity > STACK_HITS ? onHeap.data() : onStack;
    uint32_t count = 0;
    const c::CallErrors errors;
    const bool taken = r->takePick(c::toCpp(pick), converted, capacity, count);
    if (hit_count != nullptr)
        *hit_count = taken ? count : 0;
    if (!taken)
        return errors.result() == RASM_OK ? RASM_ERROR_FAILED : errors.result();
    for (uint32_t i = 0; i < std::min(capacity, count); ++i)
    {
        const rasm_pick_hit value = toC(converted[i]);
        c::writeSized(reinterpret_cast<uint8_t *>(hits) + static_cast<size_t>(i) * stride, c::PICK_HIT_SIZE_1_7, &value, sizeof(value), __func__, "hit");
    }
    return RASM_OK;
}
RASM_C_CATCH_RESULT
// NOLINTEND(readability-identifier-naming)
