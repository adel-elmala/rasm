#include "c/cApi.h"
#include "platform/paths.h"

#include "spdlog/sinks/sink.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

namespace rasm::c
{
    namespace
    {
        // What the sink keeps for the thread that logged.
        struct ThreadErrors
        {
            std::string lastError;
            uint64_t count = 0;
        };

        // Gives every RASM error to the thread's last error, and every line to the application's callback. spdlog
        // calls it from whichever thread logged: the caller's, or a load worker's. It holds no lock while the callback
        // runs, so the callback may call RASM, and two threads may be in it at once.
        class CSink : public spdlog::sinks::sink
        {
        public:
            void log(const spdlog::details::log_msg &message) override;
            void flush() override {}
            void set_pattern(const std::string &) override {}
            void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

            void setCallback(rasm_log_callback newCallback, void *newUserData)
            {
                const std::lock_guard<std::mutex> lock(mutex);
                callback = newCallback;
                userData = newUserData;
            }

        private:
            std::mutex mutex;
            rasm_log_callback callback = nullptr;
            void *userData = nullptr;
        };
    }

    namespace
    {
        thread_local ThreadErrors threadErrors; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): the per-thread last error the C API promises

        std::shared_ptr<CSink> &getSink()
        {
            static std::shared_ptr<CSink> sink = std::make_shared<CSink>();
            return sink;
        }

        void baseModuleAnchor() {}

        // Fails the build when a C constant and its C++ enum value differ: the C values are fixed by the ABI.
        constexpr uint32_t asNumber(Format value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(PlatformType value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(NativeWindowType value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(Backend value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(AntiAliasing value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(MeshletPath value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(MaterialType value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(PbrSlot value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(PbrParam value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(AlphaMode value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(LightType value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(CameraProjection::Type value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(ReadbackFormat value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(spdlog::level::level_enum value) { return static_cast<uint32_t>(value); }

        static_assert(RASM_FORMAT_UNKNOWN == asNumber(Format::UNKNOWN));
        static_assert(RASM_FORMAT_R8G8B8A8_UNORM == asNumber(Format::R8G8B8A8_UNORM));
        static_assert(RASM_FORMAT_R8G8B8A8_SRGB == asNumber(Format::R8G8B8A8_SRGB));
        static_assert(RASM_FORMAT_B8G8R8A8_SRGB == asNumber(Format::B8G8R8A8_SRGB));
        static_assert(RASM_FORMAT_B8G8R8A8_UNORM == asNumber(Format::B8G8R8A8_UNORM));
        static_assert(RASM_FORMAT_R8G8B8_UNORM == asNumber(Format::R8G8B8_UNORM));
        static_assert(RASM_FORMAT_R16G16B16A16_SFLOAT == asNumber(Format::R16G16B16A16_SFLOAT));
        static_assert(RASM_FORMAT_R32G32B32_SFLOAT == asNumber(Format::R32G32B32_SFLOAT));
        static_assert(RASM_FORMAT_R32G32B32A32_SFLOAT == asNumber(Format::R32G32B32A32_SFLOAT));
        static_assert(RASM_FORMAT_D24_UNORM_S8_UINT == asNumber(Format::D24_UNORM_S8_UINT));
        static_assert(RASM_FORMAT_D32_SFLOAT == asNumber(Format::D32_SFLOAT));
        static_assert(RASM_FORMAT_D32_SFLOAT_S8_UINT == asNumber(Format::D32_SFLOAT_S8_UINT));
        static_assert(RASM_FORMAT_R32G32_SFLOAT == asNumber(Format::R32G32_SFLOAT));
        static_assert(RASM_FORMAT_U16_UINT == asNumber(Format::U16_UINT));
        static_assert(RASM_FORMAT_U32_UINT == asNumber(Format::U32_UINT));
        static_assert(RASM_FORMAT_BC4_UNORM == asNumber(Format::BC4_UNORM));
        static_assert(RASM_FORMAT_BC5_UNORM == asNumber(Format::BC5_UNORM));
        static_assert(RASM_FORMAT_BC7_UNORM == asNumber(Format::BC7_UNORM));
        static_assert(RASM_FORMAT_BC7_SRGB == asNumber(Format::BC7_SRGB));
        static_assert(RASM_FORMAT_ASTC_4X4_UNORM == asNumber(Format::ASTC_4x4_UNORM));
        static_assert(RASM_FORMAT_ASTC_4X4_SRGB == asNumber(Format::ASTC_4x4_SRGB));
        static_assert(RASM_FORMAT_ETC2_R8G8B8A8_UNORM == asNumber(Format::ETC2_R8G8B8A8_UNORM));
        static_assert(RASM_FORMAT_ETC2_R8G8B8A8_SRGB == asNumber(Format::ETC2_R8G8B8A8_SRGB));
        static_assert(RASM_FORMAT_EAC_R11_UNORM == asNumber(Format::EAC_R11_UNORM));
        static_assert(RASM_FORMAT_EAC_R11G11_UNORM == asNumber(Format::EAC_R11G11_UNORM));
        static_assert(RASM_FORMAT_R16G16_SFLOAT == asNumber(Format::R16G16_SFLOAT));
        static_assert(RASM_PLATFORM_TYPE_SDL == asNumber(PlatformType::SDL));
        static_assert(RASM_PLATFORM_TYPE_HEADLESS == asNumber(PlatformType::HEADLESS));
        static_assert(RASM_PLATFORM_TYPE_NATIVE == asNumber(PlatformType::NATIVE));
        static_assert(RASM_NATIVE_WINDOW_TYPE_NONE == asNumber(NativeWindowType::NONE));
        static_assert(RASM_NATIVE_WINDOW_TYPE_WINDOWS == asNumber(NativeWindowType::WINDOWS));
        static_assert(RASM_NATIVE_WINDOW_TYPE_X11 == asNumber(NativeWindowType::X11));
        static_assert(RASM_NATIVE_WINDOW_TYPE_XCB == asNumber(NativeWindowType::XCB));
        static_assert(RASM_NATIVE_WINDOW_TYPE_WAYLAND == asNumber(NativeWindowType::WAYLAND));
        static_assert(RASM_NATIVE_WINDOW_TYPE_METAL == asNumber(NativeWindowType::METAL));
        static_assert(RASM_BACKEND_VULKAN == asNumber(Backend::VULKAN));
        static_assert(RASM_BACKEND_DX12 == asNumber(Backend::DX12));
        static_assert(RASM_BACKEND_METAL == asNumber(Backend::METAL));
        static_assert(RASM_BACKEND_WEBGPU == asNumber(Backend::WEBGPU));
        static_assert(RASM_ANTI_ALIASING_NONE == asNumber(AntiAliasing::NONE));
        static_assert(RASM_ANTI_ALIASING_MSAA_2X == asNumber(AntiAliasing::MSAA_2X));
        static_assert(RASM_ANTI_ALIASING_MSAA_4X == asNumber(AntiAliasing::MSAA_4X));
        static_assert(RASM_ANTI_ALIASING_MSAA_8X == asNumber(AntiAliasing::MSAA_8X));
        static_assert(RASM_ANTI_ALIASING_TAA == asNumber(AntiAliasing::TAA));
        static_assert(RASM_ANTI_ALIASING_DEFAULT == asNumber(AntiAliasing::DEFAULT));
        static_assert(RASM_MESHLET_PATH_OFF == asNumber(MeshletPath::OFF));
        static_assert(RASM_MESHLET_PATH_VERTEX_PULLING == asNumber(MeshletPath::VERTEX_PULLING));
        static_assert(RASM_MESHLET_PATH_MESH_SHADERS == asNumber(MeshletPath::MESH_SHADERS));
        static_assert(RASM_MESHLET_PATH_AUTO == asNumber(MeshletPath::AUTO));
        static_assert(RASM_MATERIAL_TYPE_PBR == asNumber(MaterialType::PBR));
        static_assert(RASM_MATERIAL_TYPE_UNLIT == asNumber(MaterialType::UNLIT));
        static_assert(RASM_PBR_SLOT_ALBEDO == asNumber(PbrSlot::ALBEDO));
        static_assert(RASM_PBR_SLOT_METALLIC == asNumber(PbrSlot::METALLIC));
        static_assert(RASM_PBR_SLOT_ROUGHNESS == asNumber(PbrSlot::ROUGHNESS));
        static_assert(RASM_PBR_SLOT_NORMAL == asNumber(PbrSlot::NORMAL));
        static_assert(RASM_PBR_SLOT_OCCLUSION == asNumber(PbrSlot::OCCLUSION));
        static_assert(RASM_PBR_SLOT_EMISSIVE == asNumber(PbrSlot::EMISSIVE));
        static_assert(RASM_PBR_PARAM_ALBEDO == asNumber(PbrParam::ALBEDO));
        static_assert(RASM_PBR_PARAM_METALLIC == asNumber(PbrParam::METALLIC));
        static_assert(RASM_PBR_PARAM_ROUGHNESS == asNumber(PbrParam::ROUGHNESS));
        static_assert(RASM_PBR_PARAM_NORMAL_SCALE == asNumber(PbrParam::NORMAL_SCALE));
        static_assert(RASM_PBR_PARAM_OCCLUSION_STRENGTH == asNumber(PbrParam::OCCLUSION_STRENGTH));
        static_assert(RASM_PBR_PARAM_EMISSIVE_FACTOR == asNumber(PbrParam::EMISSIVE_FACTOR));
        static_assert(RASM_PBR_PARAM_EMISSIVE_STRENGTH == asNumber(PbrParam::EMISSIVE_STRENGTH));
        static_assert(RASM_ALPHA_MODE_NONE == asNumber(AlphaMode::NONE));
        static_assert(RASM_ALPHA_MODE_MASK == asNumber(AlphaMode::MASK));
        static_assert(RASM_ALPHA_MODE_BLEND == asNumber(AlphaMode::BLEND));
        static_assert(RASM_LIGHT_TYPE_DIRECTIONAL == asNumber(LightType::DIRECTIONAL));
        static_assert(RASM_LIGHT_TYPE_POINT == asNumber(LightType::POINT));
        static_assert(RASM_LIGHT_TYPE_SPOT == asNumber(LightType::SPOT));
        static_assert(RASM_PROJECTION_TYPE_PERSPECTIVE == asNumber(CameraProjection::Type::PERSPECTIVE));
        static_assert(RASM_PROJECTION_TYPE_ORTHOGRAPHIC == asNumber(CameraProjection::Type::ORTHOGRAPHIC));
        static_assert(RASM_READBACK_FORMAT_RGBA8_SRGB == asNumber(ReadbackFormat::RGBA8_SRGB));
        static_assert(RASM_READBACK_FORMAT_RGBA32_FLOAT == asNumber(ReadbackFormat::RGBA32_FLOAT));
        static_assert(RASM_LOG_LEVEL_TRACE == asNumber(spdlog::level::trace) && RASM_LOG_LEVEL_DEBUG == asNumber(spdlog::level::debug) && RASM_LOG_LEVEL_INFO == asNumber(spdlog::level::info));
        static_assert(RASM_LOG_LEVEL_WARN == asNumber(spdlog::level::warn) && RASM_LOG_LEVEL_ERROR == asNumber(spdlog::level::err) && RASM_LOG_LEVEL_CRITICAL == asNumber(spdlog::level::critical));
        static_assert(RASM_MAX_VERTEX_UV_SETS == MAX_VERTEX_UV_SETS && RASM_MAX_TEXTURE_SLOTS == MAX_TEXTURE_SLOTS && RASM_MAX_PARAM_SLOTS == MAX_PARAM_SLOTS);

        // rasm_create_mesh copies the caller's vertices as they are.
        static_assert(sizeof(rasm_vertex) == sizeof(Vertex));
        static_assert(offsetof(rasm_vertex, normal) == offsetof(Vertex, normal));
        static_assert(offsetof(rasm_vertex, uv) == offsetof(Vertex, uv));
        static_assert(offsetof(rasm_vertex, tangent) == offsetof(Vertex, tangent));
        static_assert(MAX_HANDLE_GENERATION <= UINT32_MAX);
    }

    void CSink::log(const spdlog::details::log_msg &message)
    {
        if (message.level >= spdlog::level::err)
        {
            ThreadErrors &errors = threadErrors;
            errors.lastError.assign(message.payload.data(), message.payload.size());
            ++errors.count;
        }
        rasm_log_callback current = nullptr;
        void *currentUserData = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            current = callback;
            currentUserData = userData;
        }
        if (current != nullptr)
        {
            // A copy of its own: a callback that calls RASM may log again on this thread before it returns.
            const std::string text(message.payload.data(), message.payload.size());
            current(currentUserData, static_cast<rasm_log_level>(message.level), text.c_str());
        }
    }

    void initialize()
    {
        // A function's static is made once, by the first thread that gets here.
        static const bool initialized = []
        {
            setBaseModule(reinterpret_cast<const void *>(&baseModuleAnchor));
            spdlog::default_logger()->sinks().push_back(getSink());
            return true;
        }();
        (void)initialized;
    }

    void reportError(const char *function, const char *message)
    {
        initialize();
        spdlog::error("{}: {}", function, message);
    }

    rasm_result reportException(const char *function) noexcept
    {
        try
        {
            try
            {
                throw;
            }
            catch (const std::bad_alloc &)
            {
                reportError(function, "out of memory");
                return RASM_ERROR_OUT_OF_MEMORY;
            }
            catch (const std::exception &error)
            {
                reportError(function, error.what());
                return RASM_ERROR_INTERNAL;
            }
        }
        catch (...)
        {
            // Not even the message could be logged; the code still says what happened.
            return RASM_ERROR_INTERNAL;
        }
    }

    uint64_t getErrorCount()
    {
        return threadErrors.count;
    }

    rasm_result checkVersion(uint32_t apiVersion, const char *function)
    {
        const uint32_t major = apiVersion >> 16;
        const uint32_t minor = apiVersion & 0xFFFFu;
        if (major == RASM_C_API_VERSION_MAJOR && minor <= RASM_C_API_VERSION_MINOR)
            return RASM_OK;
        reportError(function, ("the program was built against rasm_c.h " + std::to_string(major) + "." + std::to_string(minor) + ", and this library is " +
                               std::to_string(RASM_C_API_VERSION_MAJOR) + "." + std::to_string(RASM_C_API_VERSION_MINOR) + ": the major versions must match, and the library must not be older.")
                                  .c_str());
        return RASM_ERROR_VERSION;
    }

    Renderer *getRenderer(rasm_renderer *renderer, const char *function)
    {
        if (renderer == nullptr || renderer->renderer == nullptr)
        {
            reportError(function, "renderer is NULL.");
            return nullptr;
        }
        return renderer->renderer;
    }

    uint32_t copyString(const std::string &text, char *buffer, uint32_t capacity)
    {
        if (buffer != nullptr && capacity > 0)
        {
            const size_t count = std::min<size_t>(text.size(), capacity - 1);
            std::memcpy(buffer, text.data(), count);
            buffer[count] = '\0';
        }
        return static_cast<uint32_t>(std::min<size_t>(text.size(), UINT32_MAX));
    }

    bool readSized(const void *source, size_t minSize, void *target, size_t targetSize, const char *function, const char *name)
    {
        if (source == nullptr)
        {
            reportError(function, (std::string(name) + " is NULL.").c_str());
            return false;
        }
        uint32_t size = 0;
        std::memcpy(&size, source, sizeof(size));
        if (size < minSize)
        {
            reportError(function, (std::string(name) + "->size is " + std::to_string(size) + ", below its smallest size " + std::to_string(minSize) + ": set it to sizeof the struct.").c_str());
            return false;
        }
        std::memcpy(static_cast<uint8_t *>(target) + sizeof(uint32_t), static_cast<const uint8_t *>(source) + sizeof(uint32_t), std::min<size_t>(size, targetSize) - sizeof(uint32_t));
        return true;
    }

    bool writeSized(void *target, size_t minSize, const void *source, size_t sourceSize, const char *function, const char *name)
    {
        if (target == nullptr)
        {
            reportError(function, (std::string(name) + " is NULL.").c_str());
            return false;
        }
        uint32_t size = 0;
        std::memcpy(&size, target, sizeof(size));
        if (size < minSize)
        {
            reportError(function, (std::string(name) + "->size is " + std::to_string(size) + ", below its smallest size " + std::to_string(minSize) + ": set it to sizeof the struct.").c_str());
            return false;
        }
        std::memcpy(static_cast<uint8_t *>(target) + sizeof(uint32_t), static_cast<const uint8_t *>(source) + sizeof(uint32_t), std::min<size_t>(size, sourceSize) - sizeof(uint32_t));
        return true;
    }

    rasm_result initSized(void *target, uint32_t size, size_t minSize, const void *defaults, size_t defaultsSize, const char *function, const char *name)
    {
        if (target == nullptr || size < minSize)
        {
            reportError(function, (std::string(name) + " is NULL, or size " + std::to_string(size) + " is below its smallest size " + std::to_string(minSize) + ".").c_str());
            return RASM_ERROR_INVALID_ARGUMENT;
        }
        // A newer caller's fields past this library's struct are zero.
        std::memset(target, 0, size);
        std::memcpy(target, defaults, std::min<size_t>(size, defaultsSize));
        std::memcpy(target, &size, sizeof(size));
        return RASM_OK;
    }

    bool checkEnum(uint32_t value, uint32_t count, const char *function, const char *name)
    {
        if (value < count)
            return true;
        reportError(function, (std::string(name) + " " + std::to_string(value) + " is not one of its constants.").c_str());
        return false;
    }

    bool checkRendererConfig(const rasm_renderer_config &config, const char *function)
    {
        return checkEnum(config.native_window.type, RASM_NATIVE_WINDOW_TYPE_METAL + 1, function, "config->native_window.type") &&
               checkEnum(config.preferred_backend, RASM_BACKEND_WEBGPU + 1, function, "config->preferred_backend") &&
               checkEnum(config.meshlet_path, RASM_MESHLET_PATH_AUTO + 1, function, "config->meshlet_path") &&
               checkEnum(config.anti_aliasing, RASM_ANTI_ALIASING_DEFAULT + 1, function, "config->anti_aliasing");
    }

    bool checkHandle(uint64_t id, const char *function, const char *name)
    {
        if (id != 0)
            return true;
        reportError(function, (std::string(name) + " is 0, which names no object.").c_str());
        return false;
    }

    Transform toCpp(const rasm_transform &transform)
    {
        return {.position = toCpp(transform.position),
                .rotation = glm::quat(transform.rotation.w, transform.rotation.x, transform.rotation.y, transform.rotation.z),
                .scale = toCpp(transform.scale)};
    }

    rasm_transform toC(const Transform &transform)
    {
        return {.position = toC(transform.position),
                .rotation = {transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w},
                .scale = toC(transform.scale)};
    }

    rasm_mat4 toC(const glm::mat4 &matrix)
    {
        rasm_mat4 result = {};
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
                result.m[column * 4 + row] = matrix[column][row];
        }
        return result;
    }

    NativeWindow toCpp(const rasm_native_window &window)
    {
        return {.type = static_cast<NativeWindowType>(window.type),
                .display = window.display,
                .window = window.window,
                .pixelWidth = window.pixel_width,
                .pixelHeight = window.pixel_height,
                .contentScale = window.content_scale};
    }

    rasm_native_window toC(const NativeWindow &window)
    {
        return {.type = asNumber(window.type),
                .display = window.display,
                .window = window.window,
                .pixel_width = window.pixelWidth,
                .pixel_height = window.pixelHeight,
                .content_scale = window.contentScale};
    }

    RendererConfig toCpp(const rasm_renderer_config &config)
    {
        return {
            .appName = config.app_name != nullptr ? config.app_name : "",
            .windowWidth = config.window_width,
            .windowHeight = config.window_height,
            .nativeWindow = toCpp(config.native_window),
            .enableValidation = config.enable_validation != 0,
            .enableSyncValidation = config.enable_sync_validation != 0,
            .enableGpuValidation = config.enable_gpu_validation != 0,
            .preferredBackend = static_cast<Backend>(config.preferred_backend),
            .shaderDir = config.shader_dir != nullptr ? config.shader_dir : "",
            .cacheDir = config.cache_dir != nullptr ? config.cache_dir : "",
            .geometryPoolVertices = config.geometry_pool_vertices,
            .geometryPoolColors = config.geometry_pool_colors,
            .geometryPoolSkins = config.geometry_pool_skins,
            .geometryPoolMorphs = config.geometry_pool_morphs,
            .geometryPoolIndices = config.geometry_pool_indices,
            .geometryPoolSubMeshes = config.geometry_pool_sub_meshes,
            .geometryPoolMeshlets = config.geometry_pool_meshlets,
            .geometryPoolMeshletData = config.geometry_pool_meshlet_data,
            .maxInstances = config.max_instances,
            .maxLights = config.max_lights,
            .maxMaterials = config.max_materials,
            .maxAnimatedEntities = config.max_animated_entities,
            .animationPoolBytes = config.animation_pool_bytes,
            .maxMorphWeights = config.max_morph_weights,
            .showStats = config.show_stats != 0,
            .fixedDeltaTime = config.fixed_delta_time,
            .occlusionCulling = config.occlusion_culling != 0,
            .meshLods = config.mesh_lods != 0,
            .lodErrorPixels = config.lod_error_pixels,
            .forcedLod = config.forced_lod,
            .meshletPath = static_cast<MeshletPath>(config.meshlet_path),
            .maxViewMeshlets = config.max_view_meshlets,
            .asyncCompute = config.async_compute != 0,
            .transferQueue = config.transfer_queue != 0,
            .clusteredLighting = config.clustered_lighting != 0,
            .hostImageCopy = config.host_image_copy != 0,
            .bloom = config.bloom != 0,
            .ui = config.ui != 0,
            .antiAliasing = static_cast<AntiAliasing>(config.anti_aliasing),
        };
    }

    // The strings are the defaults' only: NULL for empty, and the library's own "cache".
    rasm_renderer_config toC(const RendererConfig &config)
    {
        return {
            .size = sizeof(rasm_renderer_config),
            .app_name = nullptr,
            .window_width = config.windowWidth,
            .window_height = config.windowHeight,
            .native_window = toC(config.nativeWindow),
            .enable_validation = config.enableValidation ? RASM_TRUE : RASM_FALSE,
            .enable_sync_validation = config.enableSyncValidation ? RASM_TRUE : RASM_FALSE,
            .enable_gpu_validation = config.enableGpuValidation ? RASM_TRUE : RASM_FALSE,
            .preferred_backend = asNumber(config.preferredBackend),
            .shader_dir = nullptr,
            .cache_dir = config.cacheDir == "cache" ? "cache" : nullptr,
            .geometry_pool_vertices = config.geometryPoolVertices,
            .geometry_pool_colors = config.geometryPoolColors,
            .geometry_pool_skins = config.geometryPoolSkins,
            .geometry_pool_morphs = config.geometryPoolMorphs,
            .geometry_pool_indices = config.geometryPoolIndices,
            .geometry_pool_sub_meshes = config.geometryPoolSubMeshes,
            .geometry_pool_meshlets = config.geometryPoolMeshlets,
            .geometry_pool_meshlet_data = config.geometryPoolMeshletData,
            .max_instances = config.maxInstances,
            .max_lights = config.maxLights,
            .max_materials = config.maxMaterials,
            .max_animated_entities = config.maxAnimatedEntities,
            .animation_pool_bytes = config.animationPoolBytes,
            .max_morph_weights = config.maxMorphWeights,
            .show_stats = config.showStats ? RASM_TRUE : RASM_FALSE,
            .fixed_delta_time = config.fixedDeltaTime,
            .occlusion_culling = config.occlusionCulling ? RASM_TRUE : RASM_FALSE,
            .mesh_lods = config.meshLods ? RASM_TRUE : RASM_FALSE,
            .lod_error_pixels = config.lodErrorPixels,
            .forced_lod = config.forcedLod,
            .meshlet_path = asNumber(config.meshletPath),
            .max_view_meshlets = config.maxViewMeshlets,
            .async_compute = config.asyncCompute ? RASM_TRUE : RASM_FALSE,
            .transfer_queue = config.transferQueue ? RASM_TRUE : RASM_FALSE,
            .clustered_lighting = config.clusteredLighting ? RASM_TRUE : RASM_FALSE,
            .host_image_copy = config.hostImageCopy ? RASM_TRUE : RASM_FALSE,
            .bloom = config.bloom ? RASM_TRUE : RASM_FALSE,
            .ui = config.ui ? RASM_TRUE : RASM_FALSE,
            .anti_aliasing = asNumber(config.antiAliasing),
        };
    }

    CameraProjection toCpp(const rasm_camera_projection &projection)
    {
        if (projection.type == RASM_PROJECTION_TYPE_ORTHOGRAPHIC)
            return {.type = CameraProjection::Type::ORTHOGRAPHIC,
                    .orthographic = {.left = projection.left, .right = projection.right, .bottom = projection.bottom, .top = projection.top, .nearZ = projection.near_z, .farZ = projection.far_z}};
        return {.type = CameraProjection::Type::PERSPECTIVE,
                .perspective = {.fovY = projection.fov_y, .aspect = projection.aspect, .nearZ = projection.near_z, .farZ = projection.far_z}};
    }

    rasm_camera_projection toC(const CameraProjection &projection)
    {
        if (projection.type == CameraProjection::Type::ORTHOGRAPHIC)
        {
            const CameraProjection::Orthographic &o = projection.orthographic;
            return {.size = sizeof(rasm_camera_projection), .type = RASM_PROJECTION_TYPE_ORTHOGRAPHIC, .near_z = o.nearZ, .far_z = o.farZ, .left = o.left, .right = o.right, .bottom = o.bottom, .top = o.top};
        }
        const CameraProjection::Perspective &p = projection.perspective;
        return {.size = sizeof(rasm_camera_projection), .type = RASM_PROJECTION_TYPE_PERSPECTIVE, .fov_y = p.fovY, .aspect = p.aspect, .near_z = p.nearZ, .far_z = p.farZ};
    }

    Light toCpp(const rasm_light_desc &light)
    {
        return {.type = static_cast<LightType>(light.type),
                .intensity = light.intensity,
                .range = light.range,
                .color = toCpp(light.color),
                .position = toCpp(light.position),
                .direction = toCpp(light.direction),
                .innerConeAngle = light.inner_cone_angle,
                .outerConeAngle = light.outer_cone_angle};
    }

    rasm_light_desc toC(const Light &light)
    {
        return {.size = sizeof(rasm_light_desc),
                .type = asNumber(light.type),
                .intensity = light.intensity,
                .range = light.range,
                .color = toC(light.color),
                .position = toC(light.position),
                .direction = toC(light.direction),
                .inner_cone_angle = light.innerConeAngle,
                .outer_cone_angle = light.outerConeAngle};
    }

    // The name and the handle stay the current material's.
    Material toCpp(const rasm_material_desc &material, const Material &current)
    {
        Material result = current;
        result.type = static_cast<MaterialType>(material.type);
        for (uint32_t i = 0; i < MAX_TEXTURE_SLOTS; ++i)
        {
            result.textures[i] = toCpp(material.textures[i]);
            result.texCoords[i] = material.tex_coords[i];
            result.textureTransforms[i] = {.offset = toCpp(material.texture_transforms[i].offset),
                                           .rotation = material.texture_transforms[i].rotation,
                                           .scale = toCpp(material.texture_transforms[i].scale)};
        }
        std::copy(std::begin(material.params), std::end(material.params), std::begin(result.params));
        result.doubleSided = material.double_sided != 0;
        result.alphaMode = static_cast<AlphaMode>(material.alpha_mode);
        result.alphaCutoff = material.alpha_cutoff;
        return result;
    }

    rasm_material_desc toC(const Material &material)
    {
        rasm_material_desc result = {.size = sizeof(rasm_material_desc), .type = asNumber(material.type)};
        for (uint32_t i = 0; i < MAX_TEXTURE_SLOTS; ++i)
        {
            result.textures[i] = toC(material.textures[i]);
            result.tex_coords[i] = material.texCoords[i];
            result.texture_transforms[i] = {.offset = toC(material.textureTransforms[i].offset),
                                            .rotation = material.textureTransforms[i].rotation,
                                            .scale = toC(material.textureTransforms[i].scale)};
        }
        std::copy(std::begin(material.params), std::end(material.params), std::begin(result.params));
        result.double_sided = material.doubleSided ? RASM_TRUE : RASM_FALSE;
        result.alpha_mode = asNumber(material.alphaMode);
        result.alpha_cutoff = material.alphaCutoff;
        return result;
    }

    SceneEnvironment toCpp(const rasm_scene_environment &environment)
    {
        return {.environment = toCpp(environment.environment), .intensity = environment.intensity, .rotation = environment.rotation, .sky = environment.sky != 0};
    }

    rasm_scene_environment toC(const SceneEnvironment &environment)
    {
        return {.size = sizeof(rasm_scene_environment),
                .environment = toC(environment.environment),
                .intensity = environment.intensity,
                .rotation = environment.rotation,
                .sky = environment.sky ? RASM_TRUE : RASM_FALSE};
    }
}

using namespace rasm;

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
uint32_t RASM_CALL rasm_get_version(void)
try
{
    return RASM_C_API_VERSION;
}
RASM_C_CATCH(0)

const char *RASM_CALL rasm_get_last_error(void)
try
{
    return c::threadErrors.lastError.c_str();
}
RASM_C_CATCH("")

void RASM_CALL rasm_clear_last_error(void)
try
{
    c::threadErrors.lastError.clear();
}
RASM_C_CATCH_VOID

void RASM_CALL rasm_set_log_callback(rasm_log_callback callback, void *user_data)
try
{
    c::initialize();
    c::getSink()->setCallback(callback, user_data);
}
RASM_C_CATCH_VOID

uint32_t RASM_CALL rasm_get_base_path(char *buffer, uint32_t capacity)
try
{
    c::initialize();
    return c::copyString(getBaseDir(), buffer, capacity);
}
RASM_C_CATCH(0)
// NOLINTEND(readability-identifier-naming)
