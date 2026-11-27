#include "c/cApi.h"

#include "rasm/readback.h"
#include "rasm/renderer.h"

#include <memory>

using namespace rasm;

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h

rasm_result RASM_CALL rasm_renderer_config_init(rasm_renderer_config *config, uint32_t size)
try
{
    const rasm_renderer_config defaults = c::toC(RendererConfig{});
    return c::initSized(config, size, c::RENDERER_CONFIG_SIZE_1_0, &defaults, sizeof(defaults), __func__, "config");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_renderer_create(uint32_t api_version, const rasm_renderer_config *config, rasm_renderer **renderer)
try
{
    c::initialize();
    if (renderer == nullptr)
    {
        c::reportError(__func__, "renderer is NULL.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    *renderer = nullptr;
    const rasm_result version = c::checkVersion(api_version, __func__);
    if (version != RASM_OK)
        return version;
    rasm_renderer_config values = c::toC(RendererConfig{});
    if (!c::readSized(config, c::RENDERER_CONFIG_SIZE_1_0, &values, sizeof(values), __func__, "config") || !c::checkRendererConfig(values, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;

    auto created = std::make_unique<rasm_renderer>();
    created->owned = std::make_unique<Renderer>(c::toCpp(values));
    created->renderer = created->owned.get();
    // The C++ constructor logs why it stopped; running() says whether it did.
    if (!created->renderer->running())
        return RASM_ERROR_FAILED;
    *renderer = created.release();
    return RASM_OK;
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_renderer_destroy(rasm_renderer *renderer)
try
{
    if (renderer == nullptr)
        return;
    if (!renderer->owned)
    {
        c::reportError(__func__, "this renderer belongs to an engine: destroy the engine with rasm_engine_destroy.");
        return;
    }
    if (!c::checkNotRecording(renderer, __func__))
        return;
    delete renderer;
}
RASM_C_CATCH_VOID

rasm_bool RASM_CALL rasm_running(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr && r->running() ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)

void RASM_CALL rasm_render(rasm_renderer *renderer, rasm_scene scene, rasm_camera camera)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r != nullptr && c::checkNotRecording(renderer, __func__))
        r->render(c::toCpp(scene), c::toCpp(camera));
}
RASM_C_CATCH_VOID

void RASM_CALL rasm_wait_for_loads(rasm_renderer *renderer)
try
{
    if (Renderer *r = c::getRenderer(renderer, __func__))
        r->waitForLoads();
}
RASM_C_CATCH_VOID

rasm_readback RASM_CALL rasm_read_frame(rasm_renderer *renderer, rasm_readback_format format)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkEnum(format, RASM_READBACK_FORMAT_RGBA32_FLOAT + 1, __func__, "format"))
        return {};
    return c::toC(r->readFrame(static_cast<ReadbackFormat>(format)));
}
RASM_C_CATCH({})

rasm_bool RASM_CALL rasm_is_readback_ready(rasm_renderer *renderer, rasm_readback readback)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr && r->isReadbackReady(c::toCpp(readback)) ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)

rasm_result RASM_CALL rasm_take_readback(rasm_renderer *renderer, rasm_readback readback, rasm_image *image)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    if (image == nullptr)
    {
        c::reportError(__func__, "image is NULL: make one with rasm_image_create.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    return r->takeReadback(c::toCpp(readback), image->image) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_image *RASM_CALL rasm_image_create(void)
try
{
    return new rasm_image();
}
RASM_C_CATCH(nullptr)

void RASM_CALL rasm_image_destroy(rasm_image *image)
try
{
    delete image;
}
RASM_C_CATCH_VOID

rasm_image_data RASM_CALL rasm_image_get(const rasm_image *image)
try
{
    if (image == nullptr)
        return {};
    const ReadbackImage &data = image->image;
    return {.width = data.width,
            .height = data.height,
            .format = static_cast<rasm_readback_format>(data.format),
            .pixels = data.pixels.empty() ? nullptr : data.pixels.data(),
            .byte_size = data.pixels.size()};
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_image_write_png(const rasm_image *image, const char *path)
try
{
    if (image == nullptr || path == nullptr)
    {
        c::reportError(__func__, "image or path is NULL.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    if (!writePng(path, image->image))
    {
        c::reportError(__func__, "the image is not RGBA8_SRGB, or the file can't be written.");
        return RASM_ERROR_FAILED;
    }
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_image_read_png(rasm_image *image, const char *path)
try
{
    if (image == nullptr || path == nullptr)
    {
        c::reportError(__func__, "image or path is NULL.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    if (!readPng(path, image->image))
    {
        c::reportError(__func__, "the PNG can't be read.");
        return RASM_ERROR_FAILED;
    }
    return RASM_OK;
}
RASM_C_CATCH_RESULT

// The first call turns measuring on, as in C++: the numbers fill in a few frames later.
rasm_result RASM_CALL rasm_get_frame_stats(rasm_renderer *renderer, rasm_frame_stats *stats)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const rasm_frame_stats values = c::toC(r->getFrameStats());
    return c::writeSized(stats, c::FRAME_STATS_SIZE_1_1, &values, sizeof(values), __func__, "stats") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_pool_stats(rasm_renderer *renderer, rasm_pool_stats *stats)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const rasm_pool_stats values = c::toC(r->getPoolStats());
    return c::writeSized(stats, c::POOL_STATS_SIZE_1_1, &values, sizeof(values), __func__, "stats") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_load_stats(rasm_renderer *renderer, rasm_load_stats *stats)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const rasm_load_stats values = c::toC(r->getLoadStats());
    return c::writeSized(stats, c::LOAD_STATS_SIZE_1_1, &values, sizeof(values), __func__, "stats") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT
// NOLINTEND(readability-identifier-naming)
