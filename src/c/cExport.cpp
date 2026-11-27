#include "c/cApi.h"

#include "rasm/export.h"
#include "rasm/renderer.h"

#include <string>
#include <utility>

using namespace rasm;

namespace
{
    static_assert(RASM_DEFAULT_EXPORT_TILE_SIZE == DEFAULT_EXPORT_TILE_SIZE);
    static_assert(RASM_MAX_EXPORT_SAMPLES == MAX_EXPORT_SAMPLES);
    static_assert(RASM_EXPORT_FORMAT_PNG8 == static_cast<uint32_t>(ExportFormat::PNG8));
    static_assert(RASM_EXPORT_FORMAT_PNG16 == static_cast<uint32_t>(ExportFormat::PNG16));
    static_assert(RASM_EXPORT_FORMAT_EXR_HALF == static_cast<uint32_t>(ExportFormat::EXR_HALF));
    static_assert(RASM_EXPORT_FORMAT_EXR_FLOAT == static_cast<uint32_t>(ExportFormat::EXR_FLOAT));
    static_assert(RASM_EXPORT_STATE_RUNNING == static_cast<uint32_t>(ExportState::RUNNING));
    static_assert(RASM_EXPORT_STATE_DONE == static_cast<uint32_t>(ExportState::DONE));
    static_assert(RASM_EXPORT_STATE_FAILED == static_cast<uint32_t>(ExportState::FAILED));
}

namespace
{
    // The path is not part of the defaults: NULL.
    rasm_export_desc toC(const ExportDesc &desc)
    {
        return {.size = sizeof(rasm_export_desc),
                .width = desc.width,
                .height = desc.height,
                .path = nullptr,
                .format = static_cast<uint32_t>(desc.format),
                .samples = desc.samples,
                .pixel_scale = desc.pixelScale,
                .tone_map = desc.toneMap ? RASM_TRUE : RASM_FALSE,
                .transparent = desc.transparent ? RASM_TRUE : RASM_FALSE,
                .msaa = desc.msaa ? RASM_TRUE : RASM_FALSE,
                .tile_size = desc.tileSize};
    }

    rasm_export_progress toC(const ExportProgress &progress)
    {
        return {.size = sizeof(rasm_export_progress),
                .state = static_cast<uint32_t>(progress.state),
                .tiles_done = progress.tilesDone,
                .tile_count = progress.tileCount,
                .fraction = progress.fraction};
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_export_desc_init(rasm_export_desc *desc, uint32_t size)
try
{
    const rasm_export_desc defaults = toC(ExportDesc{});
    return c::initSized(desc, size, c::EXPORT_DESC_SIZE_1_16, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_export_progress_init(rasm_export_progress *progress, uint32_t size)
try
{
    const rasm_export_progress defaults = toC(ExportProgress{});
    return c::initSized(progress, size, c::EXPORT_PROGRESS_SIZE_1_16, &defaults, sizeof(defaults), __func__, "progress");
}
RASM_C_CATCH_RESULT

rasm_export RASM_CALL rasm_export_view(rasm_renderer *renderer, const rasm_view *view, const rasm_export_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    View converted;
    rasm_export_desc values = toC(ExportDesc{});
    if (r == nullptr || !c::checkPointer(view, __func__, "view") || !c::readViews(view, 1, &converted, __func__) ||
        !c::readSized(desc, c::EXPORT_DESC_SIZE_1_16, &values, sizeof(values), __func__, "desc") ||
        !c::checkEnum(values.format, RASM_EXPORT_FORMAT_EXR_FLOAT + 1, __func__, "desc->format"))
        return {};
    const ExportDesc exportDesc{.width = values.width,
                                .height = values.height,
                                .path = values.path != nullptr ? std::string(values.path) : std::string(),
                                .format = static_cast<ExportFormat>(values.format),
                                .samples = values.samples,
                                .pixelScale = values.pixel_scale,
                                .toneMap = values.tone_map != RASM_FALSE,
                                .transparent = values.transparent != RASM_FALSE,
                                .msaa = values.msaa != RASM_FALSE,
                                .tileSize = values.tile_size};
    const c::CallErrors errors;
    return c::toC(r->exportView(converted, exportDesc));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_get_export_progress(rasm_renderer *renderer, rasm_export export_handle, rasm_export_progress *progress)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const rasm_export_progress values = toC(r->getExportProgress(c::toCpp(export_handle)));
    return c::writeSized(progress, c::EXPORT_PROGRESS_SIZE_1_16, &values, sizeof(values), __func__, "progress") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_cancel_export(rasm_renderer *renderer, rasm_export export_handle)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    return r->cancelExport(c::toCpp(export_handle)) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_take_export(rasm_renderer *renderer, rasm_export export_handle, rasm_image *image)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    ReadbackImage taken;
    if (!r->takeExport(c::toCpp(export_handle), taken))
        return RASM_ERROR_FAILED;
    if (image != nullptr)
        image->image = std::move(taken);
    return RASM_OK;
}
RASM_C_CATCH_RESULT
// NOLINTEND(readability-identifier-naming)
