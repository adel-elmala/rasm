#include "c/cApi.h"

#include "rasm/renderer.h"
#include "rasm/view.h"

#include <cstring>
#include <string>

using namespace rasm;

namespace
{
    // One color for the tone map to draw a view into, cleared to opaque black.
    rasm_render_target_desc getTargetDefaults()
    {
        return {.size = sizeof(rasm_render_target_desc), .format = RASM_FORMAT_R8G8B8A8_SRGB, .clear_color = {0.0f, 0.0f, 0.0f, 1.0f}};
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_view_init(rasm_view *view, uint32_t size)
try
{
    const rasm_view defaults = c::toC(View{});
    return c::initSized(view, size, c::VIEW_SIZE_1_1, &defaults, sizeof(defaults), __func__, "view");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_render_target_desc_init(rasm_render_target_desc *desc, uint32_t size)
try
{
    const rasm_render_target_desc defaults = getTargetDefaults();
    return c::initSized(desc, size, c::RENDER_TARGET_DESC_SIZE_1_1, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

// The views go to Renderer::render through an array on the stack: no allocation per frame.
rasm_result RASM_CALL rasm_render_views(rasm_renderer *renderer, const rasm_view *views, uint32_t view_count)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || (view_count > 0 && !c::checkPointer(views, __func__, "views")))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (view_count > MAX_SCENE_VIEWS)
    {
        c::reportError(__func__, ("view_count " + std::to_string(view_count) + " is more than RASM_MAX_VIEWS.").c_str());
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    View converted[MAX_SCENE_VIEWS];
    uint32_t stride = 0;
    if (view_count > 0)
        std::memcpy(&stride, views, sizeof(stride));
    for (uint32_t i = 0; i < view_count; ++i)
    {
        // Every element has the size of the first: the array steps by it.
        const auto *element = reinterpret_cast<const uint8_t *>(views) + static_cast<size_t>(i) * stride;
        uint32_t size = 0;
        std::memcpy(&size, element, sizeof(size));
        if (size != stride)
        {
            c::reportError(__func__, ("views[" + std::to_string(i) + "].size is " + std::to_string(size) + ", not the first view's " + std::to_string(stride) + ".").c_str());
            return RASM_ERROR_INVALID_ARGUMENT;
        }
        rasm_view values = c::toC(View{});
        if (!c::readSized(element, c::VIEW_SIZE_1_1, &values, sizeof(values), __func__, "view") || !c::checkView(values, __func__))
            return RASM_ERROR_INVALID_ARGUMENT;
        converted[i] = c::toCpp(values);
    }
    const c::CallErrors errors;
    r->render(converted, view_count);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_view_pixels RASM_CALL rasm_get_view_pixels(const rasm_view_rect *rect, uint32_t width, uint32_t height)
try
{
    if (!c::checkPointer(rect, __func__, "rect"))
        return {};
    const ViewPixels pixels = getViewPixels(c::toCpp(*rect), width, height);
    return {.x = pixels.x, .y = pixels.y, .width = pixels.width, .height = pixels.height};
}
RASM_C_CATCH({})

rasm_render_target RASM_CALL rasm_create_render_target(rasm_renderer *renderer, const rasm_render_target_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_render_target_desc values = getTargetDefaults();
    if (r == nullptr || !c::readSized(desc, c::RENDER_TARGET_DESC_SIZE_1_1, &values, sizeof(values), __func__, "desc"))
        return {};
    if (values.format != RASM_FORMAT_R8G8B8A8_SRGB && values.format != RASM_FORMAT_R8G8B8A8_UNORM && values.format != RASM_FORMAT_R16G16B16A16_SFLOAT)
    {
        c::reportError(__func__, "desc->format must be RASM_FORMAT_R8G8B8A8_SRGB (a picture), RASM_FORMAT_R8G8B8A8_UNORM or RASM_FORMAT_R16G16B16A16_SFLOAT.");
        return {};
    }
    // A view texture: one color, no depth, one sample.
    return c::toC(r->createRenderTarget({.name = values.name != nullptr ? values.name : "",
                                         .width = values.width,
                                         .height = values.height,
                                         .colorFormats = {static_cast<Format>(values.format)},
                                         .clearColor = c::toCpp(values.clear_color),
                                         .readable = values.readable != 0}));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_resize_render_target(rasm_renderer *renderer, rasm_render_target target, uint32_t width, uint32_t height)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return RASM_ERROR_INVALID_ARGUMENT;
    return r->resizeRenderTarget(c::toCpp(target), width, height) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_destroy_render_target(rasm_renderer *renderer, rasm_render_target target)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyRenderTarget(c::toCpp(target));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_texture RASM_CALL rasm_get_render_target_texture(rasm_renderer *renderer, rasm_render_target target)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return {};
    return c::toC(r->getRenderTargetTexture(c::toCpp(target)));
}
RASM_C_CATCH({})
// NOLINTEND(readability-identifier-naming)
