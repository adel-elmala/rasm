#include "c/cApi.h"

#include "rasm/renderer.h"
#include "rasm/view.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace rasm;

namespace
{
    // A color a render target can draw into, or UNKNOWN for none.
    bool isColorFormat(rasm_format format)
    {
        switch (format)
        {
        case RASM_FORMAT_UNKNOWN:
        case RASM_FORMAT_R8G8B8A8_UNORM:
        case RASM_FORMAT_R8G8B8A8_SRGB:
        case RASM_FORMAT_B8G8R8A8_SRGB:
        case RASM_FORMAT_B8G8R8A8_UNORM:
        case RASM_FORMAT_R16G16B16A16_SFLOAT:
        case RASM_FORMAT_R32G32B32A32_SFLOAT:
        case RASM_FORMAT_R32G32_SFLOAT:
        case RASM_FORMAT_R16G16_SFLOAT:
        case RASM_FORMAT_R16_UNORM:
        case RASM_FORMAT_R32_SFLOAT:
        case RASM_FORMAT_R8_UNORM:
        case RASM_FORMAT_R32G32B32A32_UINT:
            return true;
        default:
            return false;
        }
    }

    bool isDepthFormat(rasm_format format)
    {
        return format == RASM_FORMAT_UNKNOWN || format == RASM_FORMAT_D24_UNORM_S8_UINT || format == RASM_FORMAT_D32_SFLOAT || format == RASM_FORMAT_D32_SFLOAT_S8_UINT;
    }
}

namespace rasm::c
{
    // One color for the tone map to draw a view into, cleared to opaque black.
    rasm_render_target_desc getRenderTargetDefaults()
    {
        return {.size = sizeof(rasm_render_target_desc), .format = RASM_FORMAT_R8G8B8A8_SRGB, .clear_color = {0.0f, 0.0f, 0.0f, 1.0f}, .samples = 1};
    }

    bool readRenderTargetDesc(const rasm_render_target_desc *desc, RenderTargetDesc &target, const char *function)
    {
        rasm_render_target_desc values = getRenderTargetDefaults();
        if (!readSized(desc, RENDER_TARGET_DESC_SIZE_1_1, &values, sizeof(values), function, "desc"))
            return false;
        bool formats = isColorFormat(values.format) && isDepthFormat(values.depth_format) && (values.format != RASM_FORMAT_UNKNOWN || values.depth_format != RASM_FORMAT_UNKNOWN);
        for (const rasm_format format : values.more_color_formats)
            formats = formats && isColorFormat(format);
        if (!formats)
        {
            reportError(function, "desc's colors must be formats a target can draw into (8-bit RGBA or BGRA, R8, R16, RG16F, RGBA16F, R32F, RG32F, RGBA32F, RGBA32_UINT), its "
                                  "depth D24_UNORM_S8_UINT, D32_SFLOAT or D32_SFLOAT_S8_UINT, and it needs a color or a depth.");
            return false;
        }
        if (!(values.swapchain_scale >= 0.0f) || !std::isfinite(values.swapchain_scale))
        {
            reportError(function, "desc->swapchain_scale must be 0 or more.");
            return false;
        }
        target = {.name = values.name != nullptr ? values.name : "",
                  .width = values.width,
                  .height = values.height,
                  .swapchainScale = values.swapchain_scale,
                  .colorFormats = {static_cast<Format>(values.format)},
                  .depthFormat = static_cast<Format>(values.depth_format),
                  .clearColor = toCpp(values.clear_color),
                  .history = values.history != 0,
                  .cube = values.cube != 0,
                  .storage = values.storage != 0,
                  .samples = values.samples,
                  .readable = values.readable != 0};
        // The colors after format, up to the first UNKNOWN.
        for (uint32_t i = 0; values.format != RASM_FORMAT_UNKNOWN && i + 1 < MAX_COLOR_ATTACHMENTS && values.more_color_formats[i] != RASM_FORMAT_UNKNOWN; ++i)
            target.colorFormats[i + 1] = static_cast<Format>(values.more_color_formats[i]);
        return true;
    }

    bool readViews(const rasm_view *views, uint32_t viewCount, View *converted, const char *function)
    {
        uint32_t stride = 0;
        if (viewCount > 0)
            std::memcpy(&stride, views, sizeof(stride));
        for (uint32_t i = 0; i < viewCount; ++i)
        {
            // Every element has the size of the first: the array steps by it.
            const auto *element = reinterpret_cast<const uint8_t *>(views) + static_cast<size_t>(i) * stride;
            uint32_t size = 0;
            std::memcpy(&size, element, sizeof(size));
            if (size != stride)
            {
                reportError(function, ("views[" + std::to_string(i) + "].size is " + std::to_string(size) + ", not the first view's " + std::to_string(stride) + ".").c_str());
                return false;
            }
            rasm_view values = toC(View{});
            if (!readSized(element, VIEW_SIZE_1_1, &values, sizeof(values), function, "view") || !checkView(values, function))
                return false;
            converted[i] = toCpp(values);
        }
        return true;
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
    const rasm_render_target_desc defaults = c::getRenderTargetDefaults();
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
    if (!c::checkNotRecording(renderer, __func__))
        return RASM_ERROR_FAILED;
    if (view_count > MAX_SCENE_VIEWS)
    {
        c::reportError(__func__, ("view_count " + std::to_string(view_count) + " is more than RASM_MAX_VIEWS.").c_str());
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    View converted[MAX_SCENE_VIEWS];
    if (!c::readViews(views, view_count, converted, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
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
    RenderTargetDesc target;
    if (r == nullptr || !c::readRenderTargetDesc(desc, target, __func__))
        return {};
    return c::toC(r->createRenderTarget(target));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_resize_render_target(rasm_renderer *renderer, rasm_render_target target, uint32_t width, uint32_t height)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (!c::checkNotRecording(renderer, __func__))
        return RASM_ERROR_FAILED;
    return r->resizeRenderTarget(c::toCpp(target), width, height) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_destroy_render_target(rasm_renderer *renderer, rasm_render_target target)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (!c::checkNotRecording(renderer, __func__))
        return RASM_ERROR_FAILED;
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
