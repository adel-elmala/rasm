#include "c/cApi.h"

#include "rasm/assetLoader.h"
#include "rasm/engine.h"

#include <memory>

struct rasm_asset_loader
{
    rasm::AssetLoader *loader = nullptr;
    std::unique_ptr<rasm::AssetLoader> owned; // null for an engine's loader
};

// The C++ Engine with the C views of its renderer and loader, which live as long as it does.
struct rasm_engine
{
    explicit rasm_engine(const rasm::EngineConfig &config) : engine(config)
    {
        renderer.renderer = &engine;
        loader.loader = &engine.getAssetLoader();
    }

    rasm::Engine engine;
    rasm_renderer renderer;
    rasm_asset_loader loader;
};

using namespace rasm;

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h

namespace
{
    AssetLoaderConfig toCpp(const rasm_asset_loader_config &config)
    {
        return {.assetRoot = config.asset_root != nullptr ? config.asset_root : "", .loadThreads = config.load_threads, .loadMemoryBudget = config.load_memory_budget};
    }

    rasm_asset_loader_config getLoaderDefaults()
    {
        const AssetLoaderConfig defaults;
        return {.size = sizeof(rasm_asset_loader_config), .asset_root = nullptr, .load_threads = defaults.loadThreads, .load_memory_budget = defaults.loadMemoryBudget};
    }

    AssetLoader *getLoader(rasm_asset_loader *loader, const char *function)
    {
        if (loader == nullptr || loader->loader == nullptr)
        {
            c::reportError(function, "loader is NULL.");
            return nullptr;
        }
        return loader->loader;
    }

    bool checkPath(const char *path, const char *function)
    {
        if (path != nullptr)
            return true;
        c::reportError(function, "path is NULL.");
        return false;
    }
}

rasm_result RASM_CALL rasm_asset_loader_config_init(rasm_asset_loader_config *config, uint32_t size)
try
{
    const rasm_asset_loader_config defaults = getLoaderDefaults();
    return c::initSized(config, size, c::ASSET_LOADER_CONFIG_SIZE_1_0, &defaults, sizeof(defaults), __func__, "config");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_engine_config_init(rasm_engine_config *config, uint32_t size)
try
{
    const rasm_engine_config defaults = {.size = sizeof(rasm_engine_config), .platform = RASM_PLATFORM_TYPE_SDL};
    return c::initSized(config, size, c::ENGINE_CONFIG_SIZE_1_0, &defaults, sizeof(defaults), __func__, "config");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_engine_create(uint32_t api_version, const rasm_engine_config *config, rasm_engine **engine)
try
{
    c::initialize();
    if (engine == nullptr)
    {
        c::reportError(__func__, "engine is NULL.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    *engine = nullptr;
    const rasm_result version = c::checkVersion(api_version, __func__);
    if (version != RASM_OK)
        return version;
    rasm_engine_config values = {.platform = RASM_PLATFORM_TYPE_SDL};
    if (!c::readSized(config, c::ENGINE_CONFIG_SIZE_1_0, &values, sizeof(values), __func__, "config") || !c::checkEnum(values.platform, RASM_PLATFORM_TYPE_NATIVE + 1, __func__, "config->platform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (values.renderer == nullptr)
    {
        c::reportError(__func__, "config->renderer is NULL: fill one with rasm_renderer_config_init.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    rasm_renderer_config rendererValues = c::toC(RendererConfig{});
    rasm_asset_loader_config loaderValues = getLoaderDefaults();
    if (!c::readSized(values.renderer, c::RENDERER_CONFIG_SIZE_1_0, &rendererValues, sizeof(rendererValues), __func__, "config->renderer") || !c::checkRendererConfig(rendererValues, __func__) ||
        (values.loader != nullptr && !c::readSized(values.loader, c::ASSET_LOADER_CONFIG_SIZE_1_0, &loaderValues, sizeof(loaderValues), __func__, "config->loader")))
        return RASM_ERROR_INVALID_ARGUMENT;

    auto created = std::make_unique<rasm_engine>(toEngineConfig(c::toCpp(rendererValues), static_cast<PlatformType>(values.platform), toCpp(loaderValues)));
    if (!created->engine.running())
        return RASM_ERROR_FAILED;
    *engine = created.release();
    return RASM_OK;
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_engine_destroy(rasm_engine *engine)
try
{
    delete engine;
}
RASM_C_CATCH_VOID

rasm_renderer *RASM_CALL rasm_engine_get_renderer(rasm_engine *engine)
try
{
    return engine != nullptr ? &engine->renderer : nullptr;
}
RASM_C_CATCH(nullptr)

rasm_asset_loader *RASM_CALL rasm_engine_get_asset_loader(rasm_engine *engine)
try
{
    return engine != nullptr ? &engine->loader : nullptr;
}
RASM_C_CATCH(nullptr)

rasm_result RASM_CALL rasm_asset_loader_create(rasm_renderer *renderer, const rasm_asset_loader_config *config, rasm_asset_loader **loader)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || loader == nullptr)
    {
        if (loader == nullptr)
            c::reportError(__func__, "loader is NULL.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    *loader = nullptr;
    rasm_asset_loader_config values = getLoaderDefaults();
    if (config != nullptr && !c::readSized(config, c::ASSET_LOADER_CONFIG_SIZE_1_0, &values, sizeof(values), __func__, "config"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    auto created = std::make_unique<rasm_asset_loader>();
    created->owned = std::make_unique<AssetLoader>(*r, toCpp(values));
    created->loader = created->owned.get();
    // The constructor refuses a second loader of one renderer by logging.
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    *loader = created.release();
    return RASM_OK;
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_asset_loader_destroy(rasm_asset_loader *loader)
try
{
    if (loader == nullptr)
        return;
    if (!loader->owned)
    {
        c::reportError(__func__, "this loader belongs to an engine: destroy the engine with rasm_engine_destroy.");
        return;
    }
    delete loader;
}
RASM_C_CATCH_VOID

rasm_mesh RASM_CALL rasm_asset_loader_load_mesh(rasm_asset_loader *loader, const char *path)
try
{
    AssetLoader *l = getLoader(loader, __func__);
    return l != nullptr && checkPath(path, __func__) ? c::toC(l->loadMesh(path)) : rasm_mesh{};
}
RASM_C_CATCH({})

rasm_mesh RASM_CALL rasm_asset_loader_load_gltf(rasm_asset_loader *loader, const char *path)
try
{
    AssetLoader *l = getLoader(loader, __func__);
    return l != nullptr && checkPath(path, __func__) ? c::toC(l->loadGLTF(path)) : rasm_mesh{};
}
RASM_C_CATCH({})

rasm_texture RASM_CALL rasm_asset_loader_load_texture(rasm_asset_loader *loader, const char *path, rasm_format format)
try
{
    AssetLoader *l = getLoader(loader, __func__);
    if (l == nullptr || !checkPath(path, __func__) || !c::checkEnum(format, RASM_FORMAT_R16G16_SFLOAT + 1, __func__, "format"))
        return {};
    return c::toC(l->loadTexture(path, static_cast<Format>(format)));
}
RASM_C_CATCH({})

rasm_environment RASM_CALL rasm_asset_loader_load_environment(rasm_asset_loader *loader, const char *path)
try
{
    AssetLoader *l = getLoader(loader, __func__);
    return l != nullptr && checkPath(path, __func__) ? c::toC(l->loadEnvironment(path)) : rasm_environment{};
}
RASM_C_CATCH({})

void RASM_CALL rasm_asset_loader_wait_for_loads(rasm_asset_loader *loader)
try
{
    if (AssetLoader *l = getLoader(loader, __func__))
        l->waitForLoads();
}
RASM_C_CATCH_VOID

uint32_t RASM_CALL rasm_asset_loader_resolve_asset_path(rasm_asset_loader *loader, const char *path, char *buffer, uint32_t capacity)
try
{
    AssetLoader *l = getLoader(loader, __func__);
    if (l == nullptr || !checkPath(path, __func__))
        return c::copyString({}, buffer, capacity);
    return c::copyString(l->resolveAssetPath(path), buffer, capacity);
}
RASM_C_CATCH(0)
// NOLINTEND(readability-identifier-naming)
