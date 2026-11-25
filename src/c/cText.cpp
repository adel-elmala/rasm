#include "c/cApi.h"

#include "rasm/renderer.h"
#include "rasm/text.h"
#include "rasm/vector.h"

using namespace rasm;

namespace
{
    static_assert(RASM_MAX_FONTS == MAX_FONTS);
    static_assert(RASM_MAX_LABELS == MAX_LABELS);
    static_assert(RASM_MAX_LABEL_REACH == MAX_LABEL_REACH);
    static_assert(RASM_LABEL_ANCHOR_CENTER == static_cast<uint32_t>(LabelAnchor::CENTER));
    static_assert(RASM_LABEL_ANCHOR_BOTTOM == static_cast<uint32_t>(LabelAnchor::BOTTOM));
    static_assert(RASM_LABEL_ANCHOR_TOP == static_cast<uint32_t>(LabelAnchor::TOP));
    static_assert(RASM_LABEL_ANCHOR_LEFT == static_cast<uint32_t>(LabelAnchor::LEFT));
    static_assert(RASM_LABEL_ANCHOR_RIGHT == static_cast<uint32_t>(LabelAnchor::RIGHT));
}

namespace
{
    rasm_label_desc toC(const LabelDesc &label)
    {
        return {.size = sizeof(rasm_label_desc),
                .position = c::toC(label.position),
                .text = label.text,
                .priority = label.priority,
                .anchor = static_cast<uint32_t>(label.anchor),
                .offset = c::toC(label.offset),
                .feature_id = label.featureId};
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h

rasm_result RASM_CALL rasm_label_desc_init(rasm_label_desc *desc, uint32_t size)
try
{
    const rasm_label_desc defaults = toC(LabelDesc{});
    return c::initSized(desc, size, c::LABEL_DESC_SIZE_1_6, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_font_atlas_destroy(rasm_font_atlas *atlas)
try
{
    delete atlas;
}
RASM_C_CATCH_VOID

rasm_font RASM_CALL rasm_create_font(rasm_renderer *renderer, const rasm_font_atlas *atlas)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkPointer(atlas, __func__, "atlas"))
        return {};
    return c::toC(r->createFont(atlas->atlas));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_destroy_font(rasm_renderer *renderer, rasm_font font)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyFont(c::toCpp(font));
    return errors.result();
}
RASM_C_CATCH_RESULT

// NOLINTEND(readability-identifier-naming)
