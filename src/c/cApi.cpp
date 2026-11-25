#include "c/cApi.h"
#include "platform/paths.h"

#include "spdlog/sinks/sink.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
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
        constexpr uint32_t asNumber(Key value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(MouseButton value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(InputEventType value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(ViewToggle value) { return static_cast<uint32_t>(value); }
        constexpr uint32_t asNumber(CameraController::Type value) { return static_cast<uint32_t>(value); }

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

        static_assert(RASM_KEY_COUNT == asNumber(Key::COUNT));
        static_assert(RASM_KEY_NONE == asNumber(Key::NONE));
        static_assert(RASM_KEY_A == asNumber(Key::A));
        static_assert(RASM_KEY_B == asNumber(Key::B));
        static_assert(RASM_KEY_C == asNumber(Key::C));
        static_assert(RASM_KEY_D == asNumber(Key::D));
        static_assert(RASM_KEY_E == asNumber(Key::E));
        static_assert(RASM_KEY_F == asNumber(Key::F));
        static_assert(RASM_KEY_G == asNumber(Key::G));
        static_assert(RASM_KEY_H == asNumber(Key::H));
        static_assert(RASM_KEY_I == asNumber(Key::I));
        static_assert(RASM_KEY_J == asNumber(Key::J));
        static_assert(RASM_KEY_K == asNumber(Key::K));
        static_assert(RASM_KEY_L == asNumber(Key::L));
        static_assert(RASM_KEY_M == asNumber(Key::M));
        static_assert(RASM_KEY_N == asNumber(Key::N));
        static_assert(RASM_KEY_O == asNumber(Key::O));
        static_assert(RASM_KEY_P == asNumber(Key::P));
        static_assert(RASM_KEY_Q == asNumber(Key::Q));
        static_assert(RASM_KEY_R == asNumber(Key::R));
        static_assert(RASM_KEY_S == asNumber(Key::S));
        static_assert(RASM_KEY_T == asNumber(Key::T));
        static_assert(RASM_KEY_U == asNumber(Key::U));
        static_assert(RASM_KEY_V == asNumber(Key::V));
        static_assert(RASM_KEY_W == asNumber(Key::W));
        static_assert(RASM_KEY_X == asNumber(Key::X));
        static_assert(RASM_KEY_Y == asNumber(Key::Y));
        static_assert(RASM_KEY_Z == asNumber(Key::Z));
        static_assert(RASM_KEY_DIGIT_0 == asNumber(Key::DIGIT_0));
        static_assert(RASM_KEY_DIGIT_1 == asNumber(Key::DIGIT_1));
        static_assert(RASM_KEY_DIGIT_2 == asNumber(Key::DIGIT_2));
        static_assert(RASM_KEY_DIGIT_3 == asNumber(Key::DIGIT_3));
        static_assert(RASM_KEY_DIGIT_4 == asNumber(Key::DIGIT_4));
        static_assert(RASM_KEY_DIGIT_5 == asNumber(Key::DIGIT_5));
        static_assert(RASM_KEY_DIGIT_6 == asNumber(Key::DIGIT_6));
        static_assert(RASM_KEY_DIGIT_7 == asNumber(Key::DIGIT_7));
        static_assert(RASM_KEY_DIGIT_8 == asNumber(Key::DIGIT_8));
        static_assert(RASM_KEY_DIGIT_9 == asNumber(Key::DIGIT_9));
        static_assert(RASM_KEY_F1 == asNumber(Key::F1));
        static_assert(RASM_KEY_F2 == asNumber(Key::F2));
        static_assert(RASM_KEY_F3 == asNumber(Key::F3));
        static_assert(RASM_KEY_F4 == asNumber(Key::F4));
        static_assert(RASM_KEY_F5 == asNumber(Key::F5));
        static_assert(RASM_KEY_F6 == asNumber(Key::F6));
        static_assert(RASM_KEY_F7 == asNumber(Key::F7));
        static_assert(RASM_KEY_F8 == asNumber(Key::F8));
        static_assert(RASM_KEY_F9 == asNumber(Key::F9));
        static_assert(RASM_KEY_F10 == asNumber(Key::F10));
        static_assert(RASM_KEY_F11 == asNumber(Key::F11));
        static_assert(RASM_KEY_F12 == asNumber(Key::F12));
        static_assert(RASM_KEY_ESCAPE == asNumber(Key::ESCAPE));
        static_assert(RASM_KEY_ENTER == asNumber(Key::ENTER));
        static_assert(RASM_KEY_TAB == asNumber(Key::TAB));
        static_assert(RASM_KEY_BACKSPACE == asNumber(Key::BACKSPACE));
        static_assert(RASM_KEY_SPACE == asNumber(Key::SPACE));
        static_assert(RASM_KEY_INSERT == asNumber(Key::INSERT));
        static_assert(RASM_KEY_DEL == asNumber(Key::DEL));
        static_assert(RASM_KEY_HOME == asNumber(Key::HOME));
        static_assert(RASM_KEY_END == asNumber(Key::END));
        static_assert(RASM_KEY_PAGE_UP == asNumber(Key::PAGE_UP));
        static_assert(RASM_KEY_PAGE_DOWN == asNumber(Key::PAGE_DOWN));
        static_assert(RASM_KEY_LEFT == asNumber(Key::LEFT));
        static_assert(RASM_KEY_RIGHT == asNumber(Key::RIGHT));
        static_assert(RASM_KEY_UP == asNumber(Key::UP));
        static_assert(RASM_KEY_DOWN == asNumber(Key::DOWN));
        static_assert(RASM_KEY_LEFT_SHIFT == asNumber(Key::LEFT_SHIFT));
        static_assert(RASM_KEY_RIGHT_SHIFT == asNumber(Key::RIGHT_SHIFT));
        static_assert(RASM_KEY_LEFT_CTRL == asNumber(Key::LEFT_CTRL));
        static_assert(RASM_KEY_RIGHT_CTRL == asNumber(Key::RIGHT_CTRL));
        static_assert(RASM_KEY_LEFT_ALT == asNumber(Key::LEFT_ALT));
        static_assert(RASM_KEY_RIGHT_ALT == asNumber(Key::RIGHT_ALT));
        static_assert(RASM_KEY_LEFT_SUPER == asNumber(Key::LEFT_SUPER));
        static_assert(RASM_KEY_RIGHT_SUPER == asNumber(Key::RIGHT_SUPER));
        static_assert(RASM_KEY_MENU == asNumber(Key::MENU));
        static_assert(RASM_KEY_APOSTROPHE == asNumber(Key::APOSTROPHE));
        static_assert(RASM_KEY_COMMA == asNumber(Key::COMMA));
        static_assert(RASM_KEY_MINUS == asNumber(Key::MINUS));
        static_assert(RASM_KEY_PERIOD == asNumber(Key::PERIOD));
        static_assert(RASM_KEY_SLASH == asNumber(Key::SLASH));
        static_assert(RASM_KEY_SEMICOLON == asNumber(Key::SEMICOLON));
        static_assert(RASM_KEY_EQUAL == asNumber(Key::EQUAL));
        static_assert(RASM_KEY_LEFT_BRACKET == asNumber(Key::LEFT_BRACKET));
        static_assert(RASM_KEY_BACKSLASH == asNumber(Key::BACKSLASH));
        static_assert(RASM_KEY_RIGHT_BRACKET == asNumber(Key::RIGHT_BRACKET));
        static_assert(RASM_KEY_GRAVE == asNumber(Key::GRAVE));
        static_assert(RASM_KEY_CAPS_LOCK == asNumber(Key::CAPS_LOCK));
        static_assert(RASM_KEY_SCROLL_LOCK == asNumber(Key::SCROLL_LOCK));
        static_assert(RASM_KEY_NUM_LOCK == asNumber(Key::NUM_LOCK));
        static_assert(RASM_KEY_PRINT_SCREEN == asNumber(Key::PRINT_SCREEN));
        static_assert(RASM_KEY_PAUSE == asNumber(Key::PAUSE));
        static_assert(RASM_KEY_KEYPAD_0 == asNumber(Key::KEYPAD_0));
        static_assert(RASM_KEY_KEYPAD_1 == asNumber(Key::KEYPAD_1));
        static_assert(RASM_KEY_KEYPAD_2 == asNumber(Key::KEYPAD_2));
        static_assert(RASM_KEY_KEYPAD_3 == asNumber(Key::KEYPAD_3));
        static_assert(RASM_KEY_KEYPAD_4 == asNumber(Key::KEYPAD_4));
        static_assert(RASM_KEY_KEYPAD_5 == asNumber(Key::KEYPAD_5));
        static_assert(RASM_KEY_KEYPAD_6 == asNumber(Key::KEYPAD_6));
        static_assert(RASM_KEY_KEYPAD_7 == asNumber(Key::KEYPAD_7));
        static_assert(RASM_KEY_KEYPAD_8 == asNumber(Key::KEYPAD_8));
        static_assert(RASM_KEY_KEYPAD_9 == asNumber(Key::KEYPAD_9));
        static_assert(RASM_KEY_KEYPAD_DECIMAL == asNumber(Key::KEYPAD_DECIMAL));
        static_assert(RASM_KEY_KEYPAD_DIVIDE == asNumber(Key::KEYPAD_DIVIDE));
        static_assert(RASM_KEY_KEYPAD_MULTIPLY == asNumber(Key::KEYPAD_MULTIPLY));
        static_assert(RASM_KEY_KEYPAD_SUBTRACT == asNumber(Key::KEYPAD_SUBTRACT));
        static_assert(RASM_KEY_KEYPAD_ADD == asNumber(Key::KEYPAD_ADD));
        static_assert(RASM_KEY_KEYPAD_ENTER == asNumber(Key::KEYPAD_ENTER));
        static_assert(RASM_KEY_KEYPAD_EQUAL == asNumber(Key::KEYPAD_EQUAL));
        static_assert(RASM_MOUSE_BUTTON_LEFT == asNumber(MouseButton::LEFT) && RASM_MOUSE_BUTTON_RIGHT == asNumber(MouseButton::RIGHT) && RASM_MOUSE_BUTTON_MIDDLE == asNumber(MouseButton::MIDDLE));
        static_assert(RASM_MOUSE_BUTTON_BACK == asNumber(MouseButton::BACK) && RASM_MOUSE_BUTTON_FORWARD == asNumber(MouseButton::FORWARD));
        static_assert(RASM_INPUT_EVENT_TYPE_NONE == asNumber(InputEventType::NONE));
        static_assert(RASM_INPUT_EVENT_TYPE_KEY == asNumber(InputEventType::KEY));
        static_assert(RASM_INPUT_EVENT_TYPE_MOUSE_BUTTON == asNumber(InputEventType::MOUSE_BUTTON));
        static_assert(RASM_INPUT_EVENT_TYPE_MOUSE_MOVE == asNumber(InputEventType::MOUSE_MOVE));
        static_assert(RASM_INPUT_EVENT_TYPE_MOUSE_WHEEL == asNumber(InputEventType::MOUSE_WHEEL));
        static_assert(RASM_INPUT_EVENT_TYPE_MOUSE_LEAVE == asNumber(InputEventType::MOUSE_LEAVE));
        static_assert(RASM_INPUT_EVENT_TYPE_TEXT == asNumber(InputEventType::TEXT));
        static_assert(RASM_INPUT_EVENT_TYPE_FOCUS == asNumber(InputEventType::FOCUS));
        static_assert(RASM_INPUT_EVENT_TYPE_WINDOW_CLOSE == asNumber(InputEventType::WINDOW_CLOSE));
        static_assert(RASM_KEY_MOD_SHIFT == KEY_MOD_SHIFT && RASM_KEY_MOD_CTRL == KEY_MOD_CTRL && RASM_KEY_MOD_ALT == KEY_MOD_ALT && RASM_KEY_MOD_SUPER == KEY_MOD_SUPER);
        static_assert(RASM_VIEW_TOGGLE_DEFAULT == asNumber(ViewToggle::DEFAULT) && RASM_VIEW_TOGGLE_OFF == asNumber(ViewToggle::OFF) && RASM_VIEW_TOGGLE_ON == asNumber(ViewToggle::ON));
        static_assert(RASM_CAMERA_CONTROLLER_TYPE_TRACK_BALL == asNumber(CameraController::Type::TRACK_BALL));
        static_assert(RASM_CAMERA_CONTROLLER_TYPE_FREE_FLY == asNumber(CameraController::Type::FREE_FLY));
        static_assert(RASM_CAMERA_CONTROLLER_TYPE_NONE == asNumber(CameraController::Type::NONE));
        static_assert(RASM_MAX_MESH_LODS == MAX_MESH_LODS && RASM_ALL_ANIMATION_CLIPS == ALL_ANIMATION_CLIPS && RASM_MAX_VIEWS == MAX_SCENE_VIEWS);

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

    bool checkPointer(const void *pointer, const char *function, const char *name)
    {
        if (pointer != nullptr)
            return true;
        reportError(function, (std::string(name) + " is NULL.").c_str());
        return false;
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
        return {.position = glm::dvec3(toCpp(transform.position)),
                .rotation = glm::quat(transform.rotation.w, transform.rotation.x, transform.rotation.y, transform.rotation.z),
                .scale = toCpp(transform.scale)};
    }

    rasm_transform toC(const Transform &transform)
    {
        return {.position = toC(glm::vec3(transform.position)),
                .rotation = {transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w},
                .scale = toC(transform.scale)};
    }

    Transform toCpp(const rasm_transform_d &transform)
    {
        return {.position = toCpp(transform.position),
                .rotation = glm::quat(transform.rotation.w, transform.rotation.x, transform.rotation.y, transform.rotation.z),
                .scale = toCpp(transform.scale)};
    }

    rasm_transform_d toTransformD(const Transform &transform)
    {
        return {.position = toC(transform.position),
                .rotation = {transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w},
                .scale = toC(transform.scale)};
    }

    // A coordinate past a float's range has a float part of 0 and is all remainder, so the sum stays the double.
    glm::vec3 getFloatPart(const glm::dvec3 &v)
    {
        glm::vec3 part = glm::vec3(v);
        for (int i = 0; i < 3; ++i)
            part[i] = std::isfinite(part[i]) ? part[i] : 0.0f;
        return part;
    }

    rasm_vec3 toFloatPart(const glm::dvec3 &v)
    {
        return toC(getFloatPart(v));
    }

    rasm_dvec3 toRemainder(const glm::dvec3 &v)
    {
        return toC(v - glm::dvec3(getFloatPart(v)));
    }

    rasm_dvec3 keepRemainder(const rasm_vec3 &given, const rasm_vec3 &current, const rasm_dvec3 &currentRemainder)
    {
        const bool unchanged = given.x == current.x && given.y == current.y && given.z == current.z;
        return unchanged ? currentRemainder : rasm_dvec3{0.0, 0.0, 0.0};
    }

    bool checkFinite(const rasm_dvec3 &v, const char *function, const char *name)
    {
        if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z))
            return true;
        reportError(function, (std::string(name) + " is not finite.").c_str());
        return false;
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
                .position = glm::dvec3(toCpp(light.position)) + toCpp(light.position_d),
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
                .position = toFloatPart(light.position),
                .direction = toC(light.direction),
                .inner_cone_angle = light.innerConeAngle,
                .outer_cone_angle = light.outerConeAngle,
                .position_d = toRemainder(light.position)};
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
        return {.environment = toCpp(environment.environment),
                .intensity = environment.intensity,
                .rotation = environment.rotation,
                .sky = environment.sky != 0,
                .orientation = glm::quat(environment.orientation.w, environment.orientation.x, environment.orientation.y, environment.orientation.z)};
    }

    rasm_scene_environment toC(const SceneEnvironment &environment)
    {
        return {.size = sizeof(rasm_scene_environment),
                .environment = toC(environment.environment),
                .intensity = environment.intensity,
                .rotation = environment.rotation,
                .sky = environment.sky ? RASM_TRUE : RASM_FALSE,
                .orientation = {environment.orientation.x, environment.orientation.y, environment.orientation.z, environment.orientation.w}};
    }
    bool checkInputEvent(const rasm_input_event &event, const char *function)
    {
        return checkEnum(event.type, RASM_INPUT_EVENT_TYPE_WINDOW_CLOSE + 1, function, "event->type") && checkEnum(event.key, RASM_KEY_COUNT, function, "event->key") &&
               checkEnum(event.physical_key, RASM_KEY_COUNT, function, "event->physical_key") && checkEnum(event.button, RASM_MOUSE_BUTTON_FORWARD + 1, function, "event->button") &&
               checkEnum(event.modifiers, (RASM_KEY_MOD_SUPER << 1), function, "event->modifiers");
    }

    bool checkCameraController(const rasm_camera_controller &controller, const char *function)
    {
        const glm::vec3 up = toCpp(controller.free_fly_up);
        if (!std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z))
        {
            reportError(function, "controller->free_fly_up is not finite.");
            return false;
        }
        return checkFinite(controller.track_ball_center_d, function, "controller->track_ball_center_d") &&
               checkEnum(controller.type, RASM_CAMERA_CONTROLLER_TYPE_NONE + 1, function, "controller->type");
    }

    bool checkView(const rasm_view &view, const char *function)
    {
        return checkEnum(view.bloom, RASM_VIEW_TOGGLE_ON + 1, function, "view->bloom") && checkEnum(view.anti_aliasing, RASM_ANTI_ALIASING_DEFAULT + 1, function, "view->anti_aliasing");
    }

    InputEvent toCpp(const rasm_input_event &event)
    {
        return {.type = static_cast<InputEventType>(event.type),
                .window = toCpp(event.window),
                .key = static_cast<Key>(event.key),
                .physicalKey = static_cast<Key>(event.physical_key),
                .down = event.down != 0,
                .repeat = event.repeat != 0,
                .modifiers = static_cast<uint8_t>(event.modifiers),
                .button = static_cast<MouseButton>(event.button),
                .x = event.x,
                .y = event.y,
                .codepoint = event.codepoint};
    }

    rasm_input_event toC(const InputEvent &event)
    {
        return {.size = sizeof(rasm_input_event),
                .type = asNumber(event.type),
                .window = toC(event.window),
                .key = asNumber(event.key),
                .physical_key = asNumber(event.physicalKey),
                .down = event.down ? RASM_TRUE : RASM_FALSE,
                .repeat = event.repeat ? RASM_TRUE : RASM_FALSE,
                .modifiers = event.modifiers,
                .button = asNumber(event.button),
                .x = event.x,
                .y = event.y,
                .codepoint = event.codepoint};
    }

    CameraInput toCpp(const rasm_camera_input &input)
    {
        return {.mouseStart = toCpp(input.mouse_start),
                .mouseEnd = toCpp(input.mouse_end),
                .wheelDelta = input.wheel_delta,
                .deltaTime = input.delta_time,
                .forward = input.forward != 0,
                .backward = input.backward != 0,
                .left = input.left != 0,
                .right = input.right != 0};
    }

    rasm_camera_input toC(const CameraInput &input)
    {
        return {.size = sizeof(rasm_camera_input),
                .mouse_start = toC(input.mouseStart),
                .mouse_end = toC(input.mouseEnd),
                .wheel_delta = input.wheelDelta,
                .delta_time = input.deltaTime,
                .forward = input.forward ? RASM_TRUE : RASM_FALSE,
                .backward = input.backward ? RASM_TRUE : RASM_FALSE,
                .left = input.left ? RASM_TRUE : RASM_FALSE,
                .right = input.right ? RASM_TRUE : RASM_FALSE};
    }

    // The C struct is flat: only the fields of its type reach the union.
    CameraController toCpp(const rasm_camera_controller &controller)
    {
        CameraController result = {.type = static_cast<CameraController::Type>(controller.type), .easingSpeed = controller.easing_speed};
        if (result.type == CameraController::Type::TRACK_BALL)
            result.trackBall = {.radius = controller.track_ball_radius,
                                .center = glm::dvec3(toCpp(controller.track_ball_center)) + toCpp(controller.track_ball_center_d),
                                .orbitSensitivity = controller.track_ball_orbit_sensitivity};
        else if (result.type == CameraController::Type::FREE_FLY)
            result.freeFly = {.yaw = controller.free_fly_yaw,
                              .pitch = controller.free_fly_pitch,
                              .speed = controller.free_fly_speed,
                              .lookSensitivity = controller.free_fly_look_sensitivity,
                              .up = toCpp(controller.free_fly_up)};
        return result;
    }

    rasm_camera_controller getControllerDefaults()
    {
        return {.size = sizeof(rasm_camera_controller),
                .type = RASM_CAMERA_CONTROLLER_TYPE_NONE,
                .easing_speed = CameraController{}.easingSpeed,
                .track_ball_radius = 5.0f,
                .track_ball_center = {0.0f, 0.0f, 0.0f},
                .track_ball_orbit_sensitivity = 1.0f,
                .free_fly_yaw = 0.0f,
                .free_fly_pitch = 0.0f,
                .free_fly_speed = 5.0f,
                .free_fly_look_sensitivity = 1.0f,
                .track_ball_center_d = {0.0, 0.0, 0.0},
                .free_fly_up = {0.0f, 1.0f, 0.0f}};
    }

    // The other type's fields keep their defaults, so a caller that switches the type gets a working controller.
    rasm_camera_controller toC(const CameraController &controller)
    {
        rasm_camera_controller result = getControllerDefaults();
        result.type = asNumber(controller.type);
        result.easing_speed = controller.easingSpeed;
        if (controller.type == CameraController::Type::TRACK_BALL)
        {
            result.track_ball_radius = controller.trackBall.radius;
            result.track_ball_center = toFloatPart(controller.trackBall.center);
            result.track_ball_center_d = toRemainder(controller.trackBall.center);
            result.track_ball_orbit_sensitivity = controller.trackBall.orbitSensitivity;
        }
        else if (controller.type == CameraController::Type::FREE_FLY)
        {
            result.free_fly_yaw = controller.freeFly.yaw;
            result.free_fly_pitch = controller.freeFly.pitch;
            result.free_fly_speed = controller.freeFly.speed;
            result.free_fly_look_sensitivity = controller.freeFly.lookSensitivity;
            if (glm::dot(controller.freeFly.up, controller.freeFly.up) > 0.0f)
                result.free_fly_up = toC(controller.freeFly.up);
        }
        return result;
    }

    ViewRect toCpp(const rasm_view_rect &rect)
    {
        return {.x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height};
    }

    InputArea toCpp(const rasm_input_area &area)
    {
        return {.x = area.x, .y = area.y, .width = area.width, .height = area.height};
    }

    rasm_input_area toC(const InputArea &area)
    {
        return {.x = area.x, .y = area.y, .width = area.width, .height = area.height};
    }

    UiInputCapture toCpp(const rasm_ui_input_capture &capture)
    {
        return {.mouse = capture.mouse != 0, .keyboard = capture.keyboard != 0, .text = capture.text != 0};
    }

    rasm_ui_input_capture toC(const UiInputCapture &capture)
    {
        return {.mouse = capture.mouse ? RASM_TRUE : RASM_FALSE, .keyboard = capture.keyboard ? RASM_TRUE : RASM_FALSE, .text = capture.text ? RASM_TRUE : RASM_FALSE};
    }

    View toCpp(const rasm_view &view)
    {
        return {.scene = toCpp(view.scene),
                .camera = toCpp(view.camera),
                .output = {.rect = toCpp(view.rect), .target = toCpp(view.target), .window = toCpp(view.window)},
                .bloom = static_cast<ViewToggle>(view.bloom),
                .antiAliasing = static_cast<AntiAliasing>(view.anti_aliasing)};
    }

    rasm_view toC(const View &view)
    {
        return {.size = sizeof(rasm_view),
                .scene = toC(view.scene),
                .camera = toC(view.camera),
                .rect = {.x = view.output.rect.x, .y = view.output.rect.y, .width = view.output.rect.width, .height = view.output.rect.height},
                .target = toC(view.output.target),
                .window = toC(view.output.window),
                .bloom = asNumber(view.bloom),
                .anti_aliasing = asNumber(view.antiAliasing)};
    }

    rasm_window_info toC(const Window &window)
    {
        return {.size = sizeof(rasm_window_info),
                .window = toC(window.handle),
                .platform_window = window.platformWindow,
                .native = toC(window.native),
                .width = window.width,
                .height = window.height,
                .pixel_width = window.pixelWidth,
                .pixel_height = window.pixelHeight,
                .content_scale = window.contentScale,
                .resized = window.resized ? RASM_TRUE : RASM_FALSE,
                .minimized = window.minimized ? RASM_TRUE : RASM_FALSE};
    }

    AnimationPlay toCpp(const rasm_animation_play &play)
    {
        return {.clip = play.clip, .startTime = play.start_time, .clipTime = play.clip_time, .speed = play.speed, .loop = play.loop != 0};
    }

    rasm_animation_play toC(const AnimationPlay &play)
    {
        return {.size = sizeof(rasm_animation_play), .clip = play.clip, .start_time = play.startTime, .clip_time = play.clipTime, .speed = play.speed, .loop = play.loop ? RASM_TRUE : RASM_FALSE};
    }

    rasm_frame_stats toC(const FrameStats &stats)
    {
        rasm_frame_stats result = {.size = sizeof(rasm_frame_stats),
                                   .cpu_frame_ms = stats.cpuFrameMs,
                                   .gpu_frame_ms = stats.gpuFrameMs,
                                   .upload_gpu_ms = stats.uploadGpuMs,
                                   .draws = stats.draws,
                                   .draws_visible = stats.drawsVisible,
                                   .triangles = stats.triangles,
                                   .triangles_visible = stats.trianglesVisible,
                                   .triangles_visible_lod0 = stats.trianglesVisibleLod0,
                                   .triangles_visible_lod = stats.trianglesVisibleLod,
                                   .meshlets_visible = stats.meshletsVisible,
                                   .meshlets_culled = stats.meshletsCulled,
                                   .meshlet_draws_whole = stats.meshletDrawsWhole,
                                   .lights_global = stats.lightsGlobal,
                                   .lights_visible = stats.lightsVisible,
                                   .lights_dropped = stats.lightsDropped};
        std::copy(std::begin(stats.drawsPerLod), std::end(stats.drawsPerLod), std::begin(result.draws_per_lod));
        return result;
    }

    rasm_pool_stats toC(const PoolStats &stats)
    {
        return {.size = sizeof(rasm_pool_stats),
                .entities = stats.entities,
                .meshes = stats.meshes,
                .materials = stats.materials,
                .lights = stats.lights,
                .textures = stats.textures,
                .bindless_textures = stats.bindlessTextures,
                .vertices = stats.vertices,
                .indices = stats.indices,
                .sub_meshes = stats.subMeshes,
                .meshlets = stats.meshlets,
                .instances = stats.instances,
                .animation_pool_bytes = stats.animationPoolBytes,
                .releases_waiting = stats.releasesWaiting};
    }

    rasm_load_stats toC(const LoadStats &stats)
    {
        return {.size = sizeof(rasm_load_stats),
                .workers = stats.workers,
                .queued = stats.queued,
                .running = stats.running,
                .decoded_bytes = stats.decodedBytes,
                .peak_decoded_bytes = stats.peakDecodedBytes};
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
