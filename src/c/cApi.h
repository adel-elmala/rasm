// clang-format off
#pragma once

#include "rasm/rasm_c.h"
#include "rasm/renderer.h"

#include <cstddef>
#include <cstdint>
#include <memory>

struct rasm_renderer
{
    rasm::Renderer*                     renderer        = nullptr;
    std::unique_ptr<rasm::Renderer>     owned;                                  // null for an engine's renderer
};

struct rasm_image
{
    rasm::ReadbackImage                 image;
};

namespace rasm::c
{

    // Every entry point calls it before it logs or creates: the log sink, and the shared library as the base folder.
    void                    initialize();
    // Logs as RASM does, which sets this thread's last error through the sink.
    void                    reportError(const char* function, const char* message);
    // In a catch (...) handler: logs the exception and gives its code.
    rasm_result             reportException(const char* function) noexcept;
    // Errors this thread logged so far, so a call can tell whether RASM refused it.
    uint64_t                getErrorCount();

    struct CallErrors
    {
        uint64_t            before          = getErrorCount();

        rasm_result         result() const  { return getErrorCount() == before ? RASM_OK : RASM_ERROR_FAILED; }
    };

    // A creation call's check of the caller's RASM_C_API_VERSION: the same major version, and a minor one no newer.
    rasm_result             checkVersion(uint32_t apiVersion, const char* function);
    Renderer*               getRenderer(rasm_renderer* renderer, const char* function);
    // The snprintf rule: writes what fits with its terminating zero, returns the full length.
    uint32_t                copyString(const std::string& text, char* buffer, uint32_t capacity);

    // A sized struct: source's first size bytes over target, which holds the defaults. Refuses null and a size under
    // minSize (the struct's size in version 1.0).
    bool                    readSized(const void* source, size_t minSize, void* target, size_t targetSize, const char* function, const char* name);
    // The other way: source's fields over target's, as far as target's size reaches; its size stays.
    bool                    writeSized(void* target, size_t minSize, const void* source, size_t sourceSize, const char* function, const char* name);
    // Fills target with defaults up to size, and sets its size.
    rasm_result             initSized(void* target, uint32_t size, size_t minSize, const void* defaults, size_t defaultsSize, const char* function, const char* name);

    // A handle as one uint64: generation << 32 | index. Generations start at 1 and stop at MAX_HANDLE_GENERATION, so a
    // valid handle is never 0 and always fits.
    inline uint64_t         packHandle(uint64_t index, uint64_t generation)
    {
        return index > UINT32_MAX || generation > UINT32_MAX || generation == 0 ? 0 : (generation << 32 | index);
    }
    inline uint64_t         unpackIndex(uint64_t id)        { return id & UINT32_MAX; }
    inline uint64_t         unpackGeneration(uint64_t id)   { return id >> 32; }

    inline SceneHandle      toCpp(rasm_scene h)             { return {unpackIndex(h.id), unpackGeneration(h.id)}; }
    inline EntityHandle     toCpp(rasm_entity h)            { return {unpackIndex(h.id), unpackGeneration(h.id)}; }
    inline MeshHandle       toCpp(rasm_mesh h)              { return {unpackIndex(h.id), unpackGeneration(h.id)}; }
    inline MaterialHandle   toCpp(rasm_material h)          { return {unpackIndex(h.id), unpackGeneration(h.id)}; }
    inline TextureHandle    toCpp(rasm_texture h)           { return {unpackIndex(h.id), unpackGeneration(h.id)}; }
    inline LightHandle      toCpp(rasm_light h)             { return {unpackIndex(h.id), unpackGeneration(h.id)}; }
    inline CameraHandle     toCpp(rasm_camera h)            { return {unpackIndex(h.id), unpackGeneration(h.id)}; }
    inline EnvironmentHandle toCpp(rasm_environment h)      { return {unpackIndex(h.id), unpackGeneration(h.id)}; }
    inline ReadbackHandle   toCpp(rasm_readback h)          { return {unpackIndex(h.id), unpackGeneration(h.id)}; }

    inline rasm_scene       toC(SceneHandle h)              { return {packHandle(h.index, h.generation)}; }
    inline rasm_entity      toC(EntityHandle h)             { return {packHandle(h.index, h.generation)}; }
    inline rasm_mesh        toC(MeshHandle h)               { return {packHandle(h.index, h.generation)}; }
    inline rasm_material    toC(MaterialHandle h)           { return {packHandle(h.index, h.generation)}; }
    inline rasm_texture     toC(TextureHandle h)            { return {packHandle(h.index, h.generation)}; }
    inline rasm_light       toC(LightHandle h)              { return {packHandle(h.index, h.generation)}; }
    inline rasm_camera      toC(CameraHandle h)             { return {packHandle(h.index, h.generation)}; }
    inline rasm_environment toC(EnvironmentHandle h)        { return {packHandle(h.index, h.generation)}; }
    inline rasm_readback    toC(ReadbackHandle h)           { return {packHandle(h.index, h.generation)}; }

    inline glm::vec2        toCpp(const rasm_vec2& v)       { return {v.x, v.y}; }
    inline glm::vec3        toCpp(const rasm_vec3& v)       { return {v.x, v.y, v.z}; }
    inline glm::vec4        toCpp(const rasm_vec4& v)       { return {v.x, v.y, v.z, v.w}; }
    inline rasm_vec2        toC(const glm::vec2& v)         { return {v.x, v.y}; }
    inline rasm_vec3        toC(const glm::vec3& v)         { return {v.x, v.y, v.z}; }
    inline rasm_vec4        toC(const glm::vec4& v)         { return {v.x, v.y, v.z, v.w}; }

    Transform               toCpp(const rasm_transform& transform);
    rasm_transform          toC(const Transform& transform);
    rasm_mat4               toC(const glm::mat4& matrix);
    NativeWindow            toCpp(const rasm_native_window& window);
    rasm_native_window      toC(const NativeWindow& window);
    RendererConfig          toCpp(const rasm_renderer_config& config);
    rasm_renderer_config    toC(const RendererConfig& config);
    CameraProjection        toCpp(const rasm_camera_projection& projection);
    rasm_camera_projection  toC(const CameraProjection& projection);
    Light                   toCpp(const rasm_light_desc& light);
    rasm_light_desc         toC(const Light& light);
    Material                toCpp(const rasm_material_desc& material, const Material& current);
    rasm_material_desc      toC(const Material& material);
    SceneEnvironment        toCpp(const rasm_scene_environment& environment);
    rasm_scene_environment  toC(const SceneEnvironment& environment);

    // An enum value from the caller: false, with an error, when it is past the last constant.
    bool                    checkEnum(uint32_t value, uint32_t count, const char* function, const char* name);
    // The config's enum fields.
    bool                    checkRendererConfig(const rasm_renderer_config& config, const char* function);
    // A handle the call acts on: false, with an error, for 0, which names no object.
    bool                    checkHandle(uint64_t id, const char* function, const char* name);

    // The struct sizes of version 1.0: the smallest a caller may pass. They stay when fields are added.
    constexpr size_t        RENDERER_CONFIG_SIZE_1_0        = offsetof(rasm_renderer_config, anti_aliasing) + sizeof(rasm_anti_aliasing);
    constexpr size_t        ASSET_LOADER_CONFIG_SIZE_1_0    = offsetof(rasm_asset_loader_config, load_memory_budget) + sizeof(uint64_t);
    constexpr size_t        ENGINE_CONFIG_SIZE_1_0          = offsetof(rasm_engine_config, loader) + sizeof(void*);
    constexpr size_t        MATERIAL_DESC_SIZE_1_0          = offsetof(rasm_material_desc, alpha_cutoff) + sizeof(float);
    constexpr size_t        TEXTURE_DESC_SIZE_1_0           = offsetof(rasm_texture_desc, format) + sizeof(rasm_format);
    constexpr size_t        LIGHT_DESC_SIZE_1_0             = offsetof(rasm_light_desc, outer_cone_angle) + sizeof(float);
    constexpr size_t        SCENE_ENVIRONMENT_SIZE_1_0      = offsetof(rasm_scene_environment, sky) + sizeof(rasm_bool);
    constexpr size_t        CAMERA_PROJECTION_SIZE_1_0      = offsetof(rasm_camera_projection, top) + sizeof(float);

}

// Each entry point is a function try block that ends with one of these, so no exception crosses the C boundary.
#define RASM_C_CATCH(failValue)     catch (...) { rasm::c::reportException(__func__); return failValue; }
#define RASM_C_CATCH_VOID           catch (...) { rasm::c::reportException(__func__); }
#define RASM_C_CATCH_RESULT         catch (...) { return rasm::c::reportException(__func__); }
// clang-format on
