#include "rasm/rasm_c.h"
#include "rasm/assetLoader.h"
#include "rasm/vector.h"
#include "rasm/picking.h"
#include "rasm/pointCloud.h"
#include "rasm/pointCloudOctree.h"
#include "rasm/glyphs.h"
#include "rasm/grid.h"
#include "rasm/cameraInput.h"
#include "rasm/light.h"
#include "rasm/scene.h"
#include "rasm/input.h"
#include "rasm/types.h"
#include "rasm/terrain.h"
#include "rasm/tileStreamer.h"

#include "doctest/doctest.h"
#include "glm/gtc/quaternion.hpp"

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// The C API as a binding sees it: these tests link only the rasm_c shared library, and need no GPU.

namespace
{
    struct LogCapture
    {
        std::vector<std::string> errors;
    };

    void RASM_CALL captureLog(void *userData, rasm_log_level level, const char *message)
    {
        if (level >= RASM_LOG_LEVEL_ERROR)
            static_cast<LogCapture *>(userData)->errors.emplace_back(message);
    }

    bool contains(const char *text, const char *part)
    {
        return std::strstr(text, part) != nullptr;
    }
}

TEST_CASE("the library's version is the header's")
{
    CHECK(rasm_get_version() == RASM_C_API_VERSION);
    CHECK(RASM_C_API_VERSION == ((RASM_C_API_VERSION_MAJOR << 16) | RASM_C_API_VERSION_MINOR));
}

TEST_CASE("the renderer config defaults are the C++ RendererConfig's")
{
    rasm_renderer_config config;
    REQUIRE(rasm_renderer_config_init(&config, sizeof(config)) == RASM_OK);
    const rasm::RendererConfig defaults;
    CHECK(config.size == sizeof(config));
    CHECK(config.app_name == nullptr);
    CHECK(config.window_width == defaults.windowWidth);
    CHECK(config.native_window.type == RASM_NATIVE_WINDOW_TYPE_NONE);
    CHECK(config.native_window.content_scale == defaults.nativeWindow.contentScale);
    CHECK(config.enable_validation == RASM_FALSE);
    CHECK(config.shader_dir == nullptr);
    CHECK(std::string(config.cache_dir) == defaults.cacheDir);
    CHECK(config.geometry_pool_vertices == defaults.geometryPoolVertices);
    CHECK(config.geometry_pool_meshlet_data == defaults.geometryPoolMeshletData);
    CHECK(config.geometry_pool_points == defaults.geometryPoolPoints);
    CHECK(config.max_instances == defaults.maxInstances);
    CHECK(config.max_morph_weights == defaults.maxMorphWeights);
    CHECK(config.occlusion_culling == RASM_TRUE);
    CHECK(config.mesh_lods == RASM_TRUE);
    CHECK(config.lod_error_pixels == defaults.lodErrorPixels);
    CHECK(config.forced_lod == defaults.forcedLod);
    CHECK(config.meshlet_path == RASM_MESHLET_PATH_AUTO);
    CHECK(config.max_view_meshlets == defaults.maxViewMeshlets);
    CHECK(config.transfer_queue == RASM_TRUE);
    CHECK(config.clustered_lighting == RASM_TRUE);
    CHECK(config.host_image_copy == RASM_TRUE);
    CHECK(config.ui == RASM_FALSE);
    CHECK(config.anti_aliasing == RASM_ANTI_ALIASING_NONE);
}

TEST_CASE("the other sized structs start from the C++ defaults")
{
    rasm_asset_loader_config loader;
    REQUIRE(rasm_asset_loader_config_init(&loader, sizeof(loader)) == RASM_OK);
    CHECK(loader.asset_root == nullptr);
    CHECK(loader.load_threads == rasm::AssetLoaderConfig{}.loadThreads);
    CHECK(loader.load_memory_budget == rasm::AssetLoaderConfig{}.loadMemoryBudget);

    rasm_engine_config engine;
    REQUIRE(rasm_engine_config_init(&engine, sizeof(engine)) == RASM_OK);
    CHECK(engine.platform == RASM_PLATFORM_TYPE_SDL);
    CHECK(engine.renderer == nullptr);
    CHECK(engine.loader == nullptr);

    rasm_light_desc light;
    REQUIRE(rasm_light_desc_init(&light, sizeof(light)) == RASM_OK);
    const rasm::Light lightDefaults{.type = rasm::LightType::DIRECTIONAL};
    CHECK(light.type == RASM_LIGHT_TYPE_DIRECTIONAL);
    CHECK(light.intensity == lightDefaults.intensity);
    CHECK(light.range == lightDefaults.range);
    CHECK(light.direction.y == lightDefaults.direction.y);
    CHECK(light.outer_cone_angle == lightDefaults.outerConeAngle);

    rasm_scene_environment environment;
    REQUIRE(rasm_scene_environment_init(&environment, sizeof(environment)) == RASM_OK);
    CHECK(environment.environment.id == 0);
    CHECK(environment.intensity == rasm::SceneEnvironment{}.intensity);
    CHECK(environment.sky == RASM_TRUE);

    rasm_texture_desc texture;
    REQUIRE(rasm_texture_desc_init(&texture, sizeof(texture)) == RASM_OK);
    CHECK(texture.format == RASM_FORMAT_R8G8B8A8_SRGB);
}

TEST_CASE("an init writes only the size it is given, and a newer caller's extra fields are zero")
{
    // A caller built against a header with more fields passes a bigger struct.
    struct Bigger
    {
        rasm_light_desc light;
        float newField;
    };
    Bigger bigger;
    std::memset(&bigger, 0xAB, sizeof(bigger));
    REQUIRE(rasm_light_desc_init(&bigger.light, sizeof(bigger)) == RASM_OK);
    CHECK(bigger.light.size == sizeof(bigger));
    CHECK(bigger.newField == 0.0f);

    rasm_light_desc light;
    CHECK(rasm_light_desc_init(&light, 8) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "rasm_light_desc_init"));
    CHECK(rasm_light_desc_init(nullptr, sizeof(light)) == RASM_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("creation checks the caller's version before anything else")
{
    rasm_renderer_config config;
    rasm_renderer_config_init(&config, sizeof(config));
    rasm_renderer *renderer = reinterpret_cast<rasm_renderer *>(&config);

    const uint32_t nextMajor = static_cast<uint32_t>(RASM_C_API_VERSION_MAJOR + 1) << 16;
    CHECK(rasm_renderer_create(nextMajor, &config, &renderer) == RASM_ERROR_VERSION);
    CHECK(renderer == nullptr);
    CHECK(contains(rasm_get_last_error(), "major versions must match"));

    const uint32_t newerMinor = RASM_C_API_VERSION + 1;
    CHECK(rasm_renderer_create(newerMinor, &config, &renderer) == RASM_ERROR_VERSION);

    rasm_engine *engine = nullptr;
    rasm_engine_config engineConfig;
    rasm_engine_config_init(&engineConfig, sizeof(engineConfig));
    engineConfig.renderer = &config;
    CHECK(rasm_engine_create(0, &engineConfig, &engine) == RASM_ERROR_VERSION);
    CHECK(engine == nullptr);
}

TEST_CASE("bad arguments give a code and a last error, and never crash")
{
    rasm_clear_last_error();
    CHECK(std::string(rasm_get_last_error()).empty());

    rasm_renderer *renderer = nullptr;
    CHECK(rasm_renderer_create(RASM_C_API_VERSION, nullptr, &renderer) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "config is NULL"));
    CHECK(rasm_renderer_create(RASM_C_API_VERSION, nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);

    rasm_renderer_config config;
    rasm_renderer_config_init(&config, sizeof(config));
    config.size = 4;
    CHECK(rasm_renderer_create(RASM_C_API_VERSION, &config, &renderer) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "config->size is 4"));

    rasm_engine_config engineConfig;
    rasm_engine_config_init(&engineConfig, sizeof(engineConfig));
    rasm_engine *engine = nullptr;
    CHECK(rasm_engine_create(RASM_C_API_VERSION, &engineConfig, &engine) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "config->renderer is NULL"));

    // Every renderer call takes a NULL renderer as an error, with the function's name in the message.
    CHECK(rasm_create_scene(nullptr).id == 0);
    CHECK(contains(rasm_get_last_error(), "rasm_create_scene: renderer is NULL"));
    CHECK(rasm_destroy_entity(nullptr, rasm_entity{}) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_running(nullptr) == RASM_FALSE);
    rasm_render(nullptr, rasm_scene{}, rasm_camera{});
    CHECK(rasm_take_readback(nullptr, rasm_readback{}, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_asset_loader_load_mesh(nullptr, "a.obj").id == 0);
    CHECK(contains(rasm_get_last_error(), "loader is NULL"));
    rasm_renderer_destroy(nullptr);
    rasm_engine_destroy(nullptr);
    rasm_asset_loader_destroy(nullptr);
    rasm_image_destroy(nullptr);
}

TEST_CASE("the last error belongs to the thread that made it")
{
    CHECK(rasm_create_scene(nullptr).id == 0);
    CHECK(contains(rasm_get_last_error(), "rasm_create_scene"));
    std::string otherThread = "unset";
    std::thread thread([&otherThread]
                       { otherThread = rasm_get_last_error(); });
    thread.join();
    CHECK(otherThread.empty());
    CHECK(contains(rasm_get_last_error(), "rasm_create_scene"));
}

TEST_CASE("the log callback gets RASM's lines")
{
    LogCapture capture;
    rasm_set_log_callback(captureLog, &capture);
    rasm_destroy_scene(nullptr, rasm_scene{});
    rasm_set_log_callback(nullptr, nullptr);
    rasm_destroy_scene(nullptr, rasm_scene{});
    REQUIRE(capture.errors.size() == 1);
    CHECK(contains(capture.errors[0].c_str(), "rasm_destroy_scene: renderer is NULL"));
}

TEST_CASE("strings come back the snprintf way")
{
    const uint32_t length = rasm_get_base_path(nullptr, 0);
    REQUIRE(length > 0);
    std::vector<char> full(length + 1, 'x');
    CHECK(rasm_get_base_path(full.data(), length + 1) == length);
    CHECK(full[length] == '\0');
    char small[4] = {'x', 'x', 'x', 'x'};
    CHECK(rasm_get_base_path(small, 4) == length);
    CHECK(small[3] == '\0');
    CHECK(std::strncmp(small, full.data(), 3) == 0);
}

TEST_CASE("the base path is the shared library's folder")
{
    std::vector<char> path(rasm_get_base_path(nullptr, 0) + 1);
    rasm_get_base_path(path.data(), static_cast<uint32_t>(path.size()));
    const std::filesystem::path folder(reinterpret_cast<const char8_t *>(path.data()));
    CHECK(std::filesystem::exists(folder / RASM_TEST_C_LIBRARY_NAME));
}

TEST_CASE("an image reads and writes PNGs")
{
    rasm_image *image = rasm_image_create();
    REQUIRE(image != nullptr);
    CHECK(rasm_image_get(image).pixels == nullptr);
    CHECK(rasm_image_read_png(image, "no such file.png") == RASM_ERROR_FAILED);
    REQUIRE(rasm_image_read_png(image, RASM_TEST_GOLDEN_DIR "/bloom.png") == RASM_OK);
    const rasm_image_data data = rasm_image_get(image);
    CHECK(data.width > 0);
    CHECK(data.format == RASM_READBACK_FORMAT_RGBA8_SRGB);
    CHECK(data.byte_size == static_cast<uint64_t>(data.width) * data.height * 4);
    const std::vector<uint8_t> pixels(data.pixels, data.pixels + data.byte_size);

    const std::filesystem::path out = std::filesystem::temp_directory_path() / "rasm_c_api_test.png";
    REQUIRE(rasm_image_write_png(image, out.string().c_str()) == RASM_OK);
    rasm_image *copy = rasm_image_create();
    REQUIRE(rasm_image_read_png(copy, out.string().c_str()) == RASM_OK);
    const rasm_image_data copied = rasm_image_get(copy);
    CHECK(copied.width == data.width);
    CHECK(copied.height == data.height);
    CHECK(std::vector<uint8_t>(copied.pixels, copied.pixels + copied.byte_size) == pixels);
    rasm_image_destroy(copy);
    rasm_image_destroy(image);
    std::filesystem::remove(out);
}

TEST_CASE("vertex colors pack as the C++ API packs them")
{
    CHECK(rasm_pack_vertex_color(rasm_vec4{1.0f, 0.0f, 0.0f, 1.0f}) == 0xFF0000FFu);
    CHECK(rasm_pack_vertex_color(rasm_vec4{0.0f, 0.0f, 0.0f, 0.0f}) == 0u);
}

TEST_CASE("a config enum past its last constant is refused before anything starts")
{
    rasm_renderer_config config;
    rasm_renderer_config_init(&config, sizeof(config));
    config.anti_aliasing = 9;
    rasm_renderer *renderer = nullptr;
    CHECK(rasm_renderer_create(RASM_C_API_VERSION, &config, &renderer) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "config->anti_aliasing 9"));
    CHECK(renderer == nullptr);
}

TEST_CASE("a camera projection starts as a 45 degree perspective")
{
    rasm_camera_projection projection;
    REQUIRE(rasm_camera_projection_init(&projection, sizeof(projection)) == RASM_OK);
    CHECK(projection.size == sizeof(projection));
    CHECK(projection.type == RASM_PROJECTION_TYPE_PERSPECTIVE);
    CHECK(projection.fov_y == 45.0f);
    CHECK(projection.near_z == 0.1f);
    CHECK(projection.far_z == 0.0f);
}

namespace
{
    struct ReentrantLog
    {
        int depth = 0;
        int calls = 0;
    };

    // Calls RASM from inside the callback, which logs again on this thread.
    void RASM_CALL logAndCallBack(void *userData, rasm_log_level level, const char *)
    {
        auto *state = static_cast<ReentrantLog *>(userData);
        ++state->calls;
        if (level < RASM_LOG_LEVEL_ERROR || state->depth > 0)
            return;
        ++state->depth;
        rasm_create_scene(nullptr);
        --state->depth;
    }
}

TEST_CASE("the log callback may call RASM")
{
    ReentrantLog state;
    rasm_set_log_callback(logAndCallBack, &state);
    rasm_create_scene(nullptr);
    rasm_set_log_callback(nullptr, nullptr);
    CHECK(state.calls == 2);
}

TEST_CASE("the base path is absolute")
{
    std::vector<char> path(rasm_get_base_path(nullptr, 0) + 1);
    rasm_get_base_path(path.data(), static_cast<uint32_t>(path.size()));
    CHECK(std::filesystem::path(reinterpret_cast<const char8_t *>(path.data())).is_absolute());
}

TEST_CASE("the 1.1 structs start from the C++ defaults")
{
    rasm_view view;
    REQUIRE(rasm_view_init(&view, sizeof(view)) == RASM_OK);
    CHECK(view.rect.width == 1.0f);
    CHECK(view.rect.height == 1.0f);
    CHECK(view.target.id == 0);
    CHECK(view.window.id == 0);
    CHECK(view.bloom == RASM_VIEW_TOGGLE_DEFAULT);
    CHECK(view.anti_aliasing == RASM_ANTI_ALIASING_DEFAULT);

    rasm_input_event event;
    REQUIRE(rasm_input_event_init(&event, sizeof(event)) == RASM_OK);
    CHECK(event.type == RASM_INPUT_EVENT_TYPE_NONE);
    CHECK(event.key == RASM_KEY_NONE);

    rasm_camera_controller controller;
    REQUIRE(rasm_camera_controller_init(&controller, sizeof(controller)) == RASM_OK);
    CHECK(controller.type == RASM_CAMERA_CONTROLLER_TYPE_NONE);
    CHECK(controller.easing_speed == 12.0f);

    rasm_render_target_desc target;
    REQUIRE(rasm_render_target_desc_init(&target, sizeof(target)) == RASM_OK);
    CHECK(target.format == RASM_FORMAT_R8G8B8A8_SRGB);
    CHECK(target.clear_color.w == 1.0f);

    rasm_animation_play play;
    REQUIRE(rasm_animation_play_init(&play, sizeof(play)) == RASM_OK);
    CHECK(play.speed == 1.0f);
    CHECK(play.loop == RASM_TRUE);

    rasm_window_desc window;
    REQUIRE(rasm_window_desc_init(&window, sizeof(window)) == RASM_OK);
    CHECK(window.title == nullptr);
    CHECK(window.native.content_scale == 1.0f);

    rasm_camera_input input;
    REQUIRE(rasm_camera_input_init(&input, sizeof(input)) == RASM_OK);
    CHECK(input.forward == RASM_FALSE);
}

TEST_CASE("the key constants are the C++ keys")
{
    CHECK(RASM_KEY_A == static_cast<uint32_t>(rasm::Key::A));
    CHECK(RASM_KEY_W == static_cast<uint32_t>(rasm::Key::W));
    CHECK(RASM_KEY_KEYPAD_EQUAL == static_cast<uint32_t>(rasm::Key::KEYPAD_EQUAL));
    CHECK(RASM_KEY_COUNT == static_cast<uint32_t>(rasm::Key::COUNT));
}

TEST_CASE("a free-fly rig moves forward while W is held")
{
    rasm_camera_controller controller;
    rasm_camera_controller_init(&controller, sizeof(controller));
    controller.type = RASM_CAMERA_CONTROLLER_TYPE_FREE_FLY;
    controller.easing_speed = 0.0f;
    rasm_camera_rig *rig = rasm_camera_rig_create(&controller, nullptr);
    REQUIRE(rig != nullptr);

    rasm_camera_input_state *state = rasm_camera_input_state_create();
    REQUIRE(state != nullptr);
    const rasm_input_area area = {0.0f, 0.0f, 640.0f, 480.0f};
    rasm_input_event press;
    rasm_input_event_init(&press, sizeof(press));
    press.type = RASM_INPUT_EVENT_TYPE_KEY;
    press.key = RASM_KEY_W;
    press.down = RASM_TRUE;
    REQUIRE(rasm_camera_input_state_add_event(state, &press, &area, nullptr) == RASM_OK);
    rasm_camera_input input;
    input.size = sizeof(input);
    REQUIRE(rasm_camera_input_state_take(state, nullptr, 0.5f, &input) == RASM_OK);
    CHECK(input.forward == RASM_TRUE);
    CHECK(input.delta_time == 0.5f);

    REQUIRE(rasm_camera_rig_update(rig, &input) == RASM_OK);
    rasm_transform transform;
    rasm_transform target;
    REQUIRE(rasm_camera_rig_get_transform(rig, &transform, &target) == RASM_OK);
    // The default speed, 5 units a second, for half a second down -Z.
    CHECK(target.position.z == doctest::Approx(-2.5f));
    CHECK(transform.position.z == doctest::Approx(-2.5f));

    press.key = RASM_KEY_COUNT;
    CHECK(rasm_camera_input_state_add_event(state, &press, &area, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    rasm_camera_input_state_destroy(state);
    rasm_camera_rig_destroy(rig);
}

TEST_CASE("view rects and input areas map as in C++")
{
    const rasm_view_rect half = {0.5f, 0.0f, 0.5f, 1.0f};
    const rasm_view_pixels pixels = rasm_get_view_pixels(&half, 1280, 720);
    CHECK(pixels.x == 640);
    CHECK(pixels.width == 640);
    CHECK(pixels.height == 720);

    const rasm_input_area area = {0.0f, 0.0f, 200.0f, 100.0f};
    const rasm_vec2 center = rasm_to_input_ndc(100.0f, 50.0f, &area);
    CHECK(center.x == doctest::Approx(0.0f));
    CHECK(center.y == doctest::Approx(0.0f));
    CHECK(rasm_render_views(nullptr, nullptr, 0) == RASM_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("a 1.1 program is still accepted")
{
    // The version check passes and the call goes on to check the config, which a bad enum fails.
    rasm_renderer_config config;
    rasm_renderer_config_init(&config, sizeof(config));
    config.anti_aliasing = 9;
    rasm_renderer *renderer = nullptr;
    CHECK(rasm_renderer_create((1u << 16) | 1u, &config, &renderer) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "config->anti_aliasing"));

    // A 1.1 caller's controller ends at free_fly_look_sensitivity: the 1.2 fields keep their defaults.
    rasm_camera_controller controller;
    std::memset(&controller, 0xAB, sizeof(controller));
    const uint32_t size11 = static_cast<uint32_t>(offsetof(rasm_camera_controller, free_fly_look_sensitivity) + sizeof(float));
    REQUIRE(rasm_camera_controller_init(&controller, size11) == RASM_OK);
    CHECK(controller.free_fly_look_sensitivity == 1.0f);
    CHECK(reinterpret_cast<const uint8_t *>(&controller.free_fly_up)[0] == 0xAB);
    controller.type = RASM_CAMERA_CONTROLLER_TYPE_FREE_FLY;
    rasm_camera_rig *rig = rasm_camera_rig_create(&controller, nullptr);
    REQUIRE(rig != nullptr);
    rasm_camera_rig_destroy(rig);
}

TEST_CASE("the 1.2 fields start from the C++ defaults")
{
    rasm_camera_controller controller;
    REQUIRE(rasm_camera_controller_init(&controller, sizeof(controller)) == RASM_OK);
    CHECK(controller.free_fly_up.y == 1.0f);
    CHECK(controller.track_ball_center_d.x == 0.0);

    rasm_scene_environment environment;
    REQUIRE(rasm_scene_environment_init(&environment, sizeof(environment)) == RASM_OK);
    CHECK(environment.orientation.w == 1.0f);
    CHECK(environment.orientation.x == 0.0f);

    rasm_light_desc light;
    REQUIRE(rasm_light_desc_init(&light, sizeof(light)) == RASM_OK);
    CHECK(light.position.x == 1.0f);
    CHECK(light.position_d.x == 0.0);
}

// Earth-centred coordinates of Paris, with centimetres and less that a float cannot hold.
TEST_CASE("a position in doubles goes in and comes out bit for bit")
{
    const rasm_dvec3 paris = {4197000.123456789, 168000.987654321, 4786000.000001};
    rasm_camera_controller controller;
    rasm_camera_controller_init(&controller, sizeof(controller));
    rasm_transform_d pose = {paris, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}};
    rasm_camera_rig *rig = rasm_camera_rig_create_d(&controller, &pose);
    REQUIRE(rig != nullptr);
    rasm_camera_input input;
    rasm_camera_input_init(&input, sizeof(input));
    input.delta_time = 1.0f / 60.0f;
    REQUIRE(rasm_camera_rig_update(rig, &input) == RASM_OK);
    rasm_transform_d transform;
    rasm_transform_d target;
    REQUIRE(rasm_camera_rig_get_transform_d(rig, &transform, &target) == RASM_OK);
    CHECK((transform.position.x == paris.x && transform.position.y == paris.y && transform.position.z == paris.z));
    CHECK((target.position.x == paris.x && target.position.y == paris.y && target.position.z == paris.z));
    rasm_transform rounded;
    REQUIRE(rasm_camera_rig_get_transform(rig, &rounded, nullptr) == RASM_OK);
    CHECK(rounded.position.x == static_cast<float>(paris.x));
    rasm_camera_rig_destroy(rig);

    // Render space: a point 2.5 m east of the camera is 2.5 m east of the origin there, and back to the same double.
    const rasm_dvec3 point = {paris.x + 2.5, paris.y, paris.z};
    const rasm_vec3 render = rasm_to_render_position(point, paris);
    CHECK(render.x == 2.5f);
    CHECK(render.y == 0.0f);
    const rasm_dvec3 back = rasm_to_world_position(render, paris);
    CHECK(back.x == paris.x + 2.5);
    CHECK(back.z == paris.z);
}

// The free-fly controller turns around its up axis: after a drag across the screen, the camera's up is still the
// axis (its pitch is 0), here +Z as at the North Pole.
TEST_CASE("a free-fly rig turns around its up axis")
{
    rasm_camera_controller controller;
    rasm_camera_controller_init(&controller, sizeof(controller));
    controller.type = RASM_CAMERA_CONTROLLER_TYPE_FREE_FLY;
    controller.easing_speed = 0.0f;
    controller.free_fly_up = {0.0f, 0.0f, 1.0f};
    // Level with the +Z horizon: the turn of 120 degrees around (1, 1, 1) takes the camera's +Y to +Z and its -Z to -X.
    const rasm_transform_d pose = {{0.0, 0.0, 6356752.0}, {0.5f, 0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}};
    rasm_camera_rig *rig = rasm_camera_rig_create_d(&controller, &pose);
    REQUIRE(rig != nullptr);
    rasm_camera_input input;
    rasm_camera_input_init(&input, sizeof(input));
    input.mouse_start = {0.0f, 0.0f};
    input.mouse_end = {0.6f, 0.0f};
    input.delta_time = 1.0f / 60.0f;
    REQUIRE(rasm_camera_rig_update(rig, &input) == RASM_OK);
    rasm_transform_d transform;
    REQUIRE(rasm_camera_rig_get_transform_d(rig, &transform, nullptr) == RASM_OK);
    const glm::quat rotation(transform.rotation.w, transform.rotation.x, transform.rotation.y, transform.rotation.z);
    const glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    CHECK(up.z == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(forward.z == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(forward.x > -0.99f);
    // A new up for the next frames, as a globe's normal changes along a flight.
    CHECK(rasm_camera_rig_set_free_fly_up(rig, rasm_vec3{0.0f, 1.0f, 0.0f}) == RASM_OK);
    CHECK(rasm_camera_rig_set_free_fly_up(rig, rasm_vec3{NAN, 0.0f, 0.0f}) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_camera_rig_set_free_fly_up(nullptr, rasm_vec3{0.0f, 1.0f, 0.0f}) == RASM_ERROR_INVALID_ARGUMENT);
    rasm_camera_rig_destroy(rig);
}

TEST_CASE("the 1.3 structs start from the C++ defaults")
{
    rasm_renderer_config config;
    REQUIRE(rasm_renderer_config_init(&config, sizeof(config)) == RASM_OK);
    CHECK(config.max_bindless_textures == rasm::RendererConfig{}.maxBindlessTextures);

    const rasm::TileStreamerConfig streamerDefaults;
    rasm_tile_streamer_config streamer;
    REQUIRE(rasm_tile_streamer_config_init(&streamer, sizeof(streamer)) == RASM_OK);
    CHECK(streamer.size == sizeof(streamer));
    CHECK(streamer.memory_budget == streamerDefaults.memoryBudget);
    CHECK(streamer.device_share == streamerDefaults.deviceShare);
    CHECK(streamer.max_tiles == streamerDefaults.maxTiles);
    CHECK(streamer.max_loads_per_frame == streamerDefaults.maxLoadsPerFrame);
    CHECK(streamer.max_loads_in_flight == streamerDefaults.maxLoadsInFlight);
    CHECK(streamer.max_drops_per_frame == streamerDefaults.maxDropsPerFrame);
    CHECK(streamer.retry_failed_after == streamerDefaults.retryFailedAfter);

    rasm_tile_desc desc;
    REQUIRE(rasm_tile_desc_init(&desc, sizeof(desc)) == RASM_OK);
    CHECK(desc.parent.id == 0);
    CHECK(desc.radius == 0.0);
    CHECK(desc.refine == RASM_TILE_REFINE_REPLACE);
    CHECK(desc.estimated_bytes == 0);

    rasm_tile_content content;
    std::memset(&content, 0xAB, sizeof(content));
    REQUIRE(rasm_tile_content_init(&content, sizeof(content)) == RASM_OK);
    CHECK(content.entities == nullptr);
    CHECK(content.entity_count == 0);
    CHECK(content.texture_count == 0);
}

TEST_CASE("the 1.4 terrain structs start from the C++ defaults, and the helpers follow the C++ ones")
{
    const rasm::TerrainTileDesc defaults;
    rasm_terrain_tile_desc desc;
    REQUIRE(rasm_terrain_tile_desc_init(&desc, sizeof(desc)) == RASM_OK);
    CHECK(desc.size == sizeof(desc));
    CHECK(desc.tile_size == defaults.size);
    CHECK(desc.orientation.w == 1.0f);
    CHECK(desc.terrain.height_scale == defaults.terrain.heightScale);
    CHECK(desc.terrain.sphere_radius == 0.0);
    CHECK(desc.imagery_rect.scale.x == 1.0f);
    CHECK(desc.roughness == defaults.roughness);

    rasm_material_desc material;
    material.size = sizeof(material);
    CHECK(rasm_get_material(nullptr, rasm_material{}, &material) == RASM_ERROR_INVALID_ARGUMENT);

    // A 4 x 4 tile of one cell, one point raised: the C helpers give the C++ ones' numbers.
    std::vector<uint16_t> samples(16, 0);
    samples[1 * 4 + 1] = 40;
    const rasm_terrain_heights heights{.unorm16 = samples.data(), .width = 4};
    desc.tile_size = 100.0;
    desc.center = {10.0, 20.0, 30.0};
    desc.terrain.height_scale = 0.25f;
    desc.terrain.height_offset = 5.0f;
    desc.terrain.min_height = 5.0f;
    desc.terrain.max_height = 15.0f;
    desc.terrain.sphere_radius = 6.371e6;
    // The point (0, 0) is 5 + 0.25 * 40 = 15 m, the others 5 m; (0.3, 0.2) is on the first triangle of the cell.
    const double radius = 6.371e6;
    CHECK(rasm_sample_terrain_height(&heights, &desc.terrain, 0.0, 0.0) == 15.0);
    CHECK(rasm_sample_terrain_height(&heights, &desc.terrain, 0.3, 0.2) == doctest::Approx(15.0 + 0.3 * (5.0 - 15.0) + 0.2 * (5.0 - 15.0)));
    rasm_dvec3 position;
    REQUIRE(rasm_get_terrain_position(&desc, &heights, 0.3, 0.2, &position) == RASM_OK);
    CHECK(position.x == doctest::Approx(10.0 - 20.0));
    CHECK(position.y == doctest::Approx(20.0 + 10.0 - (20.0 * 20.0 + 30.0 * 30.0) / (2.0 * radius)).epsilon(1e-12));
    CHECK(position.z == doctest::Approx(30.0 - 30.0));
    rasm_transform_d transform;
    REQUIRE(rasm_get_terrain_transform(&desc, &transform) == RASM_OK);
    const double floor = 5.0 - 2.0 * 50.0 * 50.0 / (2.0 * radius);
    CHECK(transform.position.y == doctest::Approx(20.0 + floor).epsilon(1e-12));
    CHECK(transform.scale.x == 100.0f);
    CHECK(transform.scale.y == doctest::Approx(15.0 - floor));

    // Bad arguments.
    const rasm_terrain_heights both{.unorm16 = samples.data(), .floats = reinterpret_cast<const float *>(samples.data()), .width = 4};
    CHECK(rasm_sample_terrain_height(&both, &desc.terrain, 0.0, 0.0) == 0.0);
    CHECK(contains(rasm_get_last_error(), "one of unorm16 and floats"));
    const rasm_terrain_heights narrow{.unorm16 = samples.data(), .width = 3};
    CHECK(rasm_get_terrain_position(&desc, &narrow, 0.0, 0.0, &position) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_get_terrain_transform(nullptr, &transform) == RASM_ERROR_INVALID_ARGUMENT);
    rasm_entity entity;
    rasm_material made;
    CHECK(rasm_create_terrain_tile(nullptr, &desc, &entity, &made) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_get_terrain_mesh(nullptr, 32).id == 0);
}

TEST_CASE("the 1.5 structs start from the C++ defaults")
{
    rasm_mesh_build_options options;
    REQUIRE(rasm_mesh_build_options_init(&options, sizeof(options)) == RASM_OK);
    CHECK(options.lods == RASM_TRUE);
    CHECK(options.meshlets == RASM_TRUE);
    rasm_line_shape shape;
    REQUIRE(rasm_line_shape_init(&shape, sizeof(shape)) == RASM_OK);
    CHECK(shape.join == RASM_LINE_JOIN_ROUND);
    CHECK(shape.cap == RASM_LINE_CAP_BUTT);
    CHECK(shape.miter_limit == RASM_MAX_VECTOR_MITER);
    CHECK(shape.drape_width == 0.0f);
    rasm_vector_placement placement;
    REQUIRE(rasm_vector_placement_init(&placement, sizeof(placement)) == RASM_OK);
    CHECK(placement.up.y == 1.0f);
    CHECK(placement.drape_tile == nullptr);
    const rasm::VectorMaterial defaults;
    rasm_material_desc material;
    std::memset(&material, 0, sizeof(material));
    CHECK(sizeof(material.vector) == sizeof(rasm_vector_material));
    CHECK(defaults.width == 1.0f);
}

TEST_CASE("meshes and vector features build on any thread, with no renderer")
{
    rasm_vector_geometry *geometry = nullptr;
    REQUIRE(rasm_vector_geometry_create(&geometry) == RASM_OK);
    const rasm_dvec3 origin = {1000.0, 0.0, 1000.0};
    const rasm_dvec3 points[3] = {{1000.0, 0.0, 1000.0}, {1010.0, 0.0, 1000.0}, {1010.0, 0.0, 1010.0}};
    const uint32_t size = 3;
    rasm_line_shape shape;
    rasm_line_shape_init(&shape, sizeof(shape));
    shape.join = RASM_LINE_JOIN_BEVEL;
    std::thread worker([&]()
                       { CHECK(rasm_vector_add_lines(geometry, origin, points, &size, 1, &shape, nullptr, nullptr) == RASM_OK); });
    worker.join();
    uint32_t vertices = 0;
    uint32_t indices = 0;
    REQUIRE(rasm_vector_geometry_get_size(geometry, &vertices, &indices) == RASM_OK);
    CHECK(vertices == 4 + 3 + 4);
    CHECK(indices == 6 + 3 + 6);
    // 1.9: a draped width that is not finite, or below 0, is refused.
    shape.drape_width = std::numeric_limits<float>::infinity();
    CHECK(rasm_vector_add_lines(geometry, origin, points, &size, 1, &shape, nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    shape.drape_width = -1.0f;
    CHECK(rasm_vector_add_lines(geometry, origin, points, &size, 1, &shape, nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    shape.drape_width = 0.0f;
    const uint32_t ringSizes[1] = {3};
    const uint32_t ringCounts[1] = {1};
    CHECK(rasm_vector_add_polygons(geometry, origin, points, ringSizes, ringCounts, 1, nullptr, nullptr) == RASM_OK);
    CHECK(rasm_vector_add_points(geometry, origin, points, 3, nullptr, nullptr, nullptr) == RASM_OK);
    rasm_mesh_build *build = nullptr;
    REQUIRE(rasm_build_vector_mesh(geometry, &build) == RASM_OK);
    CHECK(build != nullptr);
    REQUIRE(rasm_vector_geometry_get_size(geometry, &vertices, &indices) == RASM_OK);
    CHECK(vertices == 0);
    // Without a renderer the build is still taken and freed.
    CHECK(rasm_create_mesh_from_build(nullptr, build).id == 0);
    rasm_vector_geometry_destroy(geometry);

    rasm_vertex triangle[3] = {};
    const uint32_t triangleIndices[3] = {0, 1, 2};
    rasm_mesh_build *mesh = nullptr;
    CHECK(rasm_build_mesh(triangle, 3, triangleIndices, 3, nullptr, nullptr, &mesh) == RASM_OK);
    rasm_mesh_build_destroy(mesh);
    const uint32_t bad[3] = {0, 1, 5};
    CHECK(rasm_build_mesh(triangle, 3, bad, 3, nullptr, nullptr, &mesh) == RASM_ERROR_FAILED);
    CHECK(mesh == nullptr);
    CHECK(rasm_build_mesh(triangle, 3, triangleIndices, 3, nullptr, nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    shape.join = 7;
    CHECK(rasm_vector_add_lines(nullptr, origin, points, &size, 1, &shape, nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    rasm_vector_geometry *other = nullptr;
    rasm_vector_geometry_create(&other);
    CHECK(rasm_vector_add_lines(other, origin, points, &size, 1, &shape, nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "shape->join"));
    rasm_vector_placement placement;
    rasm_vector_placement_init(&placement, sizeof(placement));
    rasm_terrain_tile_desc tile;
    rasm_terrain_tile_desc_init(&tile, sizeof(tile));
    placement.drape_tile = &tile;
    CHECK(rasm_vector_add_points(other, origin, points, 3, &placement, nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "drape_heights"));
    rasm_vector_geometry_destroy(other);
}

TEST_CASE("the 1.6 structs start from the C++ defaults")
{
    const rasm::FontLoadOptions fontDefaults;
    rasm_font_load_options options;
    REQUIRE(rasm_font_load_options_init(&options, sizeof(options)) == RASM_OK);
    CHECK(options.em_size == fontDefaults.emSize);
    CHECK(options.distance_range == fontDefaults.distanceRange);
    CHECK(options.codepoints == nullptr);
    const rasm::LabelDesc labelDefaults;
    rasm_label_desc label;
    REQUIRE(rasm_label_desc_init(&label, sizeof(label)) == RASM_OK);
    CHECK(label.size == sizeof(label));
    CHECK(label.anchor == RASM_LABEL_ANCHOR_CENTER);
    CHECK(label.priority == labelDefaults.priority);
    CHECK(label.text == nullptr);
    CHECK(offsetof(rasm_material_desc, font) > offsetof(rasm_material_desc, vector));
    CHECK(RASM_MAX_LABEL_REACH == rasm::MAX_LABEL_REACH);
}

TEST_CASE("the 1.7 structs start from the C++ defaults, and a pick call with a bad argument gives a last error")
{
    const rasm::PickDesc defaults;
    rasm_pick_desc desc;
    REQUIRE(rasm_pick_desc_init(&desc, sizeof(desc)) == RASM_OK);
    CHECK(desc.size == sizeof(desc));
    CHECK(desc.rect.width == defaults.rect.width);
    CHECK(desc.rect.height == defaults.rect.height);
    CHECK(desc.min_alpha == defaults.minAlpha);
    CHECK(desc.skip == 0);
    rasm_pick_hit hit;
    REQUIRE(rasm_pick_hit_init(&hit, sizeof(hit)) == RASM_OK);
    CHECK(hit.size == sizeof(hit));
    CHECK(hit.entity.id == 0);
    CHECK(hit.normal.y == 1.0f);
    CHECK(RASM_MAX_PICK_SIZE == rasm::MAX_PICK_SIZE);
    CHECK(RASM_FORMAT_R32G32B32A32_UINT == static_cast<uint32_t>(rasm::Format::R32G32B32A32_UINT));

    rasm_view view;
    rasm_view_init(&view, sizeof(view));
    CHECK(rasm_pick_view(nullptr, &view, &desc).id == 0);
    CHECK(contains(rasm_get_last_error(), "renderer is NULL"));
    CHECK(rasm_pick_target(nullptr, rasm_scene{}, rasm_camera{}, rasm_render_target{}, nullptr).id == 0);
    CHECK(rasm_is_pick_ready(nullptr, rasm_pick{}) == RASM_FALSE);
    uint32_t count = 7;
    CHECK(rasm_take_pick(nullptr, rasm_pick{}, &hit, 1, &count) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_is_entity_alive(nullptr, rasm_entity{}) == RASM_FALSE);
    uint32_t x = 0;
    uint32_t y = 0;
    CHECK(rasm_get_view_pixel(nullptr, rasm_window{}, nullptr, 1.0f, 1.0f, &x, &y) == RASM_FALSE);
}

TEST_CASE("the 1.8 structs start from the C++ defaults, and a grid call with a bad argument gives a last error")
{
    const rasm::Grid defaults;
    rasm_grid grid;
    REQUIRE(rasm_grid_init(&grid, sizeof(grid)) == RASM_OK);
    CHECK(grid.size == sizeof(grid));
    CHECK(grid.enabled == RASM_FALSE);
    CHECK(grid.orientation.w == 1.0f);
    CHECK(grid.cell_size == defaults.cellSize);
    CHECK(grid.subdivisions == defaults.subdivisions);
    CHECK(grid.minor_color.w == defaults.minorColor.w);
    CHECK(grid.major_line_width == defaults.majorLineWidth);
    CHECK(grid.axes == RASM_TRUE);
    CHECK(grid.z_axis_color.z == defaults.zAxisColor.z);
    CHECK(grid.fade_distance == 0.0f);
    rasm_camera_buttons buttons;
    REQUIRE(rasm_camera_buttons_init(&buttons, sizeof(buttons)) == RASM_OK);
    CHECK(buttons.left == RASM_CAMERA_DRAG_ORBIT);
    CHECK(buttons.right == RASM_CAMERA_DRAG_PAN);
    CHECK(buttons.middle == RASM_CAMERA_DRAG_PAN);
    CHECK(buttons.back == RASM_CAMERA_DRAG_NONE);
    CHECK(buttons.shift_left == RASM_CAMERA_DRAG_PAN);
    rasm_camera_input input;
    REQUIRE(rasm_camera_input_init(&input, sizeof(input)) == RASM_OK);
    CHECK(input.pointer.x == 0.0f);
    CHECK(input.pan_end.y == 0.0f);
    rasm_camera_controller controller;
    REQUIRE(rasm_camera_controller_init(&controller, sizeof(controller)) == RASM_OK);
    CHECK(controller.track_ball_style == RASM_ORBIT_STYLE_TURNTABLE);
    CHECK(controller.track_ball_up.y == 1.0f);
    CHECK(RASM_ORBIT_STYLE_ARCBALL == static_cast<uint32_t>(rasm::CameraController::OrbitStyle::ARCBALL));
    rasm_input_event event;
    REQUIRE(rasm_input_event_init(&event, sizeof(event)) == RASM_OK);
    CHECK(event.dx == 0.0f);
    CHECK(rasm_set_mouse_grab(nullptr, rasm_window{}, RASM_TRUE) == RASM_ERROR_INVALID_ARGUMENT);
    controller.track_ball_style = 9;
    CHECK(rasm_camera_rig_create(&controller, nullptr) == nullptr);

    CHECK(rasm_set_camera_grid(nullptr, rasm_camera{}, &grid) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "renderer is NULL"));
    CHECK(rasm_get_camera_grid(nullptr, rasm_camera{}, &grid) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_set_camera_buttons(nullptr, &buttons) == RASM_ERROR_INVALID_ARGUMENT);
    buttons.middle = 7;
    CHECK(rasm_camera_input_state_set_buttons(nullptr, &buttons) == RASM_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("the 1.10 structs start from the C++ defaults, and a point cloud chunk builds on any thread")
{
    const rasm::PointMaterial defaults;
    rasm_material_desc desc = {};
    CHECK(sizeof(desc.points) == sizeof(rasm_point_material));
    CHECK(RASM_MAX_POINT_CHUNK_POINTS == rasm::MAX_POINT_CHUNK_POINTS);
    CHECK(RASM_COLORMAP_SIZE == rasm::COLORMAP_SIZE);
    CHECK(RASM_PICK_SKIP_ALL == rasm::PICK_SKIP_ALL);
    CHECK(defaults.density == 2.0f);
    rasm_point_cloud_chunk_desc chunk;
    REQUIRE(rasm_point_cloud_chunk_desc_init(&chunk, sizeof(chunk)) == RASM_OK);
    CHECK(chunk.size == sizeof(chunk));
    CHECK(chunk.positions == nullptr);
    CHECK(chunk.count == 0);

    const rasm_vec3 positions[3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f}, {-4.0f, 0.5f, 2.0f}};
    const float scalars[3] = {1.0f, 2.0f, 3.0f};
    chunk.positions = positions;
    chunk.scalars = scalars;
    chunk.count = 3;
    rasm_mesh_build *build = nullptr;
    std::thread worker([&]()
                       { CHECK(rasm_build_point_cloud_chunk(&chunk, &build) == RASM_OK); });
    worker.join();
    CHECK(build != nullptr);
    // Without a renderer the build is still taken and freed.
    CHECK(rasm_create_mesh_from_build(nullptr, build).id == 0);

    chunk.count = 0;
    CHECK(rasm_build_point_cloud_chunk(&chunk, &build) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(build == nullptr);
    chunk.count = 3;
    chunk.positions = nullptr;
    CHECK(rasm_build_point_cloud_chunk(&chunk, &build) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "positions"));
    CHECK(rasm_build_point_cloud_chunk(nullptr, &build) == RASM_ERROR_INVALID_ARGUMENT);
    const rasm_vec4 colors[2] = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};
    CHECK(rasm_create_colormap(nullptr, colors, 2).id == 0);
    CHECK(contains(rasm_get_last_error(), "renderer is NULL"));
    CHECK(rasm_create_colormap_preset(nullptr, RASM_COLORMAP_TURBO).id == 0);
}

TEST_CASE("the 1.11 config field starts from the C++ default, and the compute points calls check their renderer")
{
    rasm_renderer_config config;
    REQUIRE(rasm_renderer_config_init(&config, sizeof(config)) == RASM_OK);
    CHECK(config.compute_points == (rasm::RendererConfig{}.computePoints ? RASM_TRUE : RASM_FALSE));
    CHECK(config.shader_threads == rasm::RendererConfig{}.shaderThreads);
    CHECK(rasm_set_compute_points(nullptr, RASM_FALSE) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "renderer is NULL"));
    CHECK(rasm_get_compute_points(nullptr) == RASM_FALSE);
}

TEST_CASE("a C rig zooms an orthographic box at the pointer, and a mapped button pans")
{
    rasm_camera_controller controller;
    rasm_camera_controller_init(&controller, sizeof(controller));
    controller.type = RASM_CAMERA_CONTROLLER_TYPE_TRACK_BALL;
    controller.track_ball_radius = 10.0f;
    controller.easing_speed = 0.0f;
    rasm_transform transform = {{0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}};
    rasm_camera_rig *rig = rasm_camera_rig_create(&controller, &transform);
    REQUIRE(rig != nullptr);
    rasm_camera_projection projection;
    rasm_camera_projection_init(&projection, sizeof(projection));
    projection.type = RASM_PROJECTION_TYPE_ORTHOGRAPHIC;
    projection.left = -8.0f;
    projection.right = 8.0f;
    projection.bottom = -4.0f;
    projection.top = 4.0f;
    projection.near_z = 0.1f;
    projection.far_z = 50.0f;
    rasm_camera_input input;
    rasm_camera_input_init(&input, sizeof(input));
    input.wheel_delta = 1.0f;
    input.pointer = {0.5f, 0.0f};
    REQUIRE(rasm_camera_rig_update_projection(rig, &projection, &input) == RASM_OK);
    CHECK(projection.right == doctest::Approx(8.0f * RASM_CAMERA_WHEEL_ZOOM));
    rasm_transform pose;
    rasm_transform target;
    REQUIRE(rasm_camera_rig_get_transform(rig, &pose, &target) == RASM_OK);
    // The pointer is 2 units right of the axis; that point stays: the camera moves 2 x (1 - 0.9).
    CHECK(pose.position.x == doctest::Approx(0.2f));

    // The right button maps to ORBIT here, so a right drag gives the orbit fields and no pan.
    rasm_camera_buttons buttons;
    rasm_camera_buttons_init(&buttons, sizeof(buttons));
    buttons.right = RASM_CAMERA_DRAG_ORBIT;
    rasm_camera_input_state *state = rasm_camera_input_state_create();
    REQUIRE(rasm_camera_input_state_set_buttons(state, &buttons) == RASM_OK);
    const rasm_input_area area = {0.0f, 0.0f, 400.0f, 400.0f};
    rasm_input_event event;
    rasm_input_event_init(&event, sizeof(event));
    event.type = RASM_INPUT_EVENT_TYPE_MOUSE_BUTTON;
    event.button = RASM_MOUSE_BUTTON_RIGHT;
    event.down = RASM_TRUE;
    event.x = 200.0f;
    event.y = 200.0f;
    REQUIRE(rasm_camera_input_state_add_event(state, &event, &area, nullptr) == RASM_OK);
    event.type = RASM_INPUT_EVENT_TYPE_MOUSE_MOVE;
    event.x = 300.0f;
    REQUIRE(rasm_camera_input_state_add_event(state, &event, &area, nullptr) == RASM_OK);
    REQUIRE(rasm_camera_input_state_take(state, nullptr, 0.016f, &input) == RASM_OK);
    CHECK(input.mouse_end.x == doctest::Approx(0.5f));
    CHECK(input.pan_end.x == input.pan_start.x);
    CHECK(input.pointer.x == doctest::Approx(0.5f));
    rasm_camera_input_state_destroy(state);
    rasm_camera_rig_destroy(rig);
}

TEST_CASE("a font atlas and labels build on any thread, with no renderer")
{
    std::ifstream file(RASM_TEST_FONT, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    REQUIRE(!bytes.empty());

    rasm_font_atlas *atlas = nullptr;
    const uint32_t letters[3] = {'A', 'B', 'C'};
    rasm_font_load_options options;
    rasm_font_load_options_init(&options, sizeof(options));
    options.codepoints = letters;
    options.codepoint_count = 3;
    std::thread worker([&]()
                       { CHECK(rasm_build_font_atlas(bytes.data(), bytes.size(), &options, &atlas) == RASM_OK); });
    worker.join();
    REQUIRE(atlas != nullptr);

    rasm_vector_geometry *geometry = nullptr;
    REQUIRE(rasm_vector_geometry_create(&geometry) == RASM_OK);
    rasm_label_desc labels[2];
    rasm_label_desc_init(&labels[0], sizeof(labels[0]));
    rasm_label_desc_init(&labels[1], sizeof(labels[1]));
    labels[0].text = "AB";
    labels[1].text = "C";
    labels[1].anchor = RASM_LABEL_ANCHOR_LEFT;
    const rasm_dvec3 origin = {1000.0, 0.0, 1000.0};
    CHECK(rasm_vector_add_labels(geometry, origin, atlas, labels, 2, nullptr) == RASM_OK);
    uint32_t vertices = 0;
    uint32_t indices = 0;
    REQUIRE(rasm_vector_geometry_get_size(geometry, &vertices, &indices) == RASM_OK);
    // Three glyphs, each a halo's quad and a fill's.
    CHECK(vertices == 3 * 2 * 4);
    CHECK(indices == 3 * 2 * 6);

    labels[1].anchor = 9;
    CHECK(rasm_vector_add_labels(geometry, origin, atlas, labels, 2, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "anchor"));
    labels[1].anchor = RASM_LABEL_ANCHOR_CENTER;
    labels[1].size = 8;
    CHECK(rasm_vector_add_labels(geometry, origin, atlas, labels, 2, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "labels[1].size"));
    CHECK(rasm_vector_add_labels(geometry, origin, nullptr, labels, 1, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    rasm_mesh_build *build = nullptr;
    REQUIRE(rasm_build_vector_mesh(geometry, &build) == RASM_OK);
    rasm_mesh_build_destroy(build);
    rasm_vector_geometry_destroy(geometry);

    // Without a renderer nothing is uploaded; junk is no font.
    CHECK(rasm_create_font(nullptr, atlas).id == 0);
    rasm_font_atlas_destroy(atlas);
    const uint8_t junk[32] = {};
    atlas = reinterpret_cast<rasm_font_atlas *>(&bytes);
    CHECK(rasm_build_font_atlas(junk, sizeof(junk), nullptr, &atlas) == RASM_ERROR_FAILED);
    CHECK(atlas == nullptr);
    CHECK(rasm_asset_loader_load_font(nullptr, "fonts/Roboto-Medium.ttf", nullptr, &atlas) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_destroy_font(nullptr, rasm_font{}) == RASM_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("a tile streamer call with a bad argument gives a code and a last error")
{
    rasm_tile_streamer *streamer = reinterpret_cast<rasm_tile_streamer *>(&streamer);
    CHECK(rasm_tile_streamer_create(nullptr, rasm_scene{}, nullptr, &streamer) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(streamer == nullptr);
    CHECK(contains(rasm_get_last_error(), "renderer is NULL"));
    CHECK(rasm_tile_streamer_create(nullptr, rasm_scene{}, nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);

    rasm_tile_desc desc;
    rasm_tile_desc_init(&desc, sizeof(desc));
    CHECK(rasm_tile_streamer_create_tile(nullptr, &desc).id == 0);
    CHECK(contains(rasm_get_last_error(), "rasm_tile_streamer_create_tile: streamer is NULL"));
    CHECK(rasm_tile_streamer_update(nullptr, nullptr, 0) == RASM_ERROR_INVALID_ARGUMENT);
    uint32_t count = 7;
    CHECK(rasm_tile_streamer_get_tiles_to_load(nullptr, &count) == nullptr);
    CHECK(rasm_tile_streamer_get_tile_state(nullptr, rasm_tile{}) == RASM_TILE_STATE_EMPTY);
    CHECK(rasm_tile_streamer_is_tile_drawn(nullptr, rasm_tile{}) == RASM_FALSE);
    CHECK(rasm_tile_streamer_get_screen_error(nullptr, rasm_dvec3{}, 1.0, 1.0) == 0.0);
    rasm_tile_streamer_stats stats;
    stats.size = sizeof(stats);
    CHECK(rasm_tile_streamer_get_stats(nullptr, &stats) == RASM_ERROR_INVALID_ARGUMENT);
    rasm_tile_streamer_destroy(nullptr);
}

TEST_CASE("the 1.12 structs start from the C++ defaults, an octree builds on any thread, and its nodes are its chunks")
{
    const rasm::PointMaterial defaults;
    rasm_material_desc desc = {};
    CHECK(sizeof(desc.points) == sizeof(rasm_point_material));
    CHECK(RASM_POINT_NODE_NONE == rasm::POINT_NODE_NONE);
    rasm_point_chunk_lod lod;
    REQUIRE(rasm_point_chunk_lod_init(&lod, sizeof(lod)) == RASM_OK);
    CHECK(lod.drawn == rasm::PointChunkLod{}.drawn);
    CHECK(lod.finer[3] == 0.0f);
    CHECK(rasm_set_point_chunk_lod(nullptr, rasm_entity{}, &lod) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "renderer is NULL"));
    rasm_point_cloud_load_options options;
    REQUIRE(rasm_point_cloud_load_options_init(&options, sizeof(options)) == RASM_OK);
    CHECK(options.scalar == static_cast<uint32_t>(rasm::PointCloudLoadOptions{}.scalar));
    CHECK(options.z_up_to_y_up == RASM_FALSE);
    rasm_point_cloud *cloud = nullptr;
    CHECK(rasm_asset_loader_load_point_cloud(nullptr, "scan.las", &options, &cloud) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(cloud == nullptr);
    CHECK(rasm_get_point_cloud_info(nullptr, nullptr) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(defaults.sizeMode == rasm::PointSizeMode::FIXED);

    rasm_point_cloud_octree_desc octreeDesc;
    REQUIRE(rasm_point_cloud_octree_desc_init(&octreeDesc, sizeof(octreeDesc)) == RASM_OK);
    CHECK(octreeDesc.cells == rasm::PointCloudOctreeDesc{}.cells);
    CHECK(octreeDesc.max_node_points == rasm::PointCloudOctreeDesc{}.maxNodePoints);
    std::vector<rasm_dvec3> positions;
    for (uint32_t i = 0; i < 30000; ++i)
        positions.push_back({4197000.0 + static_cast<double>(i % 200) * 0.25, 168000.0 + static_cast<double>(i / 200) * 0.25, 4786000.0 + 0.01 * static_cast<double>(i % 7)});
    octreeDesc.positions = positions.data();
    octreeDesc.count = static_cast<uint32_t>(positions.size());
    octreeDesc.max_node_points = 4096;
    rasm_point_cloud_octree *octree = nullptr;
    std::thread worker([&]()
                       { CHECK(rasm_build_point_cloud_octree(&octreeDesc, &octree) == RASM_OK); });
    worker.join();
    REQUIRE(octree != nullptr);
    const uint32_t nodes = rasm_get_point_cloud_node_count(octree);
    CHECK(nodes > 1);
    uint32_t points = 0;
    for (uint32_t n = 0; n < nodes; ++n)
    {
        rasm_point_cloud_node node;
        node.size = sizeof(node);
        REQUIRE(rasm_get_point_cloud_node(octree, n, &node) == RASM_OK);
        CHECK((n == 0) == (node.parent == RASM_POINT_NODE_NONE));
        rasm_point_cloud_chunk_desc chunk;
        REQUIRE(rasm_point_cloud_chunk_desc_init(&chunk, sizeof(chunk)) == RASM_OK);
        REQUIRE(rasm_get_point_cloud_node_chunk(octree, n, &chunk) == RASM_OK);
        CHECK(chunk.count == node.point_count);
        CHECK(chunk.spacing == node.spacing);
        points += chunk.count;
        rasm_mesh_build *build = nullptr;
        CHECK(rasm_build_point_cloud_chunk(&chunk, &build) == RASM_OK);
        rasm_mesh_build_destroy(build);
    }
    uint32_t indexCount = 0;
    const uint32_t *indices = rasm_get_point_cloud_octree_indices(octree, &indexCount);
    CHECK(indices != nullptr);
    CHECK(indexCount == positions.size());
    CHECK(points == positions.size());
    rasm_point_cloud_node node;
    node.size = sizeof(node);
    CHECK(rasm_get_point_cloud_node(octree, nodes, &node) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "past the octree"));
    rasm_point_cloud_octree_destroy(octree);
    octreeDesc.cells = 100;
    CHECK(rasm_build_point_cloud_octree(&octreeDesc, &octree) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(octree == nullptr);
    desc.points.size_mode = 7;
    CHECK(rasm_update_material(nullptr, rasm_material{1}, &desc) == RASM_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("the 1.14 structs start from the C++ defaults, a glyph chunk builds on any thread, and data lines take values")
{
    CHECK(RASM_MAX_GLYPH_CHUNK_GLYPHS == rasm::MAX_GLYPH_CHUNK_GLYPHS);
    CHECK(RASM_PICK_HIT_GLYPH == rasm::PICK_HIT_GLYPH);
    CHECK(RASM_PICK_SKIP_GLYPHS == rasm::PICK_SKIP_GLYPHS);
    CHECK((RASM_PICK_SKIP_ALL & RASM_PICK_SKIP_GLYPHS) != 0);
    CHECK(RASM_C_API_VERSION_MINOR >= 14);
    rasm_glyph_chunk_desc chunk;
    REQUIRE(rasm_glyph_chunk_desc_init(&chunk, sizeof(chunk)) == RASM_OK);
    CHECK(chunk.size == sizeof(chunk));
    CHECK(chunk.positions == nullptr);
    CHECK(chunk.orientations == nullptr);
    CHECK(chunk.count == 0);

    const rasm_vec3 positions[3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f}, {-4.0f, 0.5f, 2.0f}};
    const rasm_quat orientations[3] = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.70710678f, 0.70710678f}, {1.0f, 0.0f, 0.0f, 0.0f}};
    const float scalars[3] = {1.0f, 2.0f, 3.0f};
    chunk.shape = rasm_mesh{.id = 1ull << 32 | 1u};
    chunk.positions = positions;
    chunk.orientations = orientations;
    chunk.scalars = scalars;
    chunk.count = 3;
    rasm_mesh_build *build = nullptr;
    std::thread worker([&]()
                       { CHECK(rasm_build_glyph_chunk(&chunk, &build) == RASM_OK); });
    worker.join();
    CHECK(build != nullptr);
    CHECK(rasm_create_mesh_from_build(nullptr, build).id == 0);
    const uint32_t colors[3] = {};
    chunk.colors = colors;
    CHECK(rasm_build_glyph_chunk(&chunk, &build) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(build == nullptr);
    chunk.colors = nullptr;
    chunk.shape = rasm_mesh{};
    CHECK(rasm_build_glyph_chunk(&chunk, &build) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(rasm_build_glyph_chunk(nullptr, &build) == RASM_ERROR_INVALID_ARGUMENT);

    CHECK(rasm_build_glyph_shape(RASM_GLYPH_SHAPE_ARROW, 16, &build) == RASM_OK);
    CHECK(build != nullptr);
    rasm_mesh_build_destroy(build);
    CHECK(rasm_build_glyph_shape(RASM_GLYPH_SHAPE_CUBE + 1, 16, &build) == RASM_ERROR_INVALID_ARGUMENT);
    CHECK(contains(rasm_get_last_error(), "shape"));
    const rasm_quat down = rasm_get_glyph_orientation(rasm_vec3{0.0f, -2.0f, 0.0f});
    const glm::vec3 turned = glm::quat(down.w, down.x, down.y, down.z) * glm::vec3(0.0f, 1.0f, 0.0f);
    CHECK(turned.y == doctest::Approx(-1.0f));

    rasm_vector_geometry *geometry = nullptr;
    REQUIRE(rasm_vector_geometry_create(&geometry) == RASM_OK);
    const rasm_dvec3 line[2] = {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}};
    const uint32_t lineSize = 2;
    const float lineScalars[2] = {0.0f, 1.0f};
    CHECK(rasm_vector_add_data_lines(geometry, line[0], line, &lineSize, 1, nullptr, nullptr, nullptr, nullptr, lineScalars) == RASM_OK);
    CHECK(rasm_vector_add_data_lines(geometry, line[0], line, &lineSize, 1, nullptr, nullptr, nullptr, colors, lineScalars) == RASM_ERROR_INVALID_ARGUMENT);
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    CHECK(rasm_vector_geometry_get_size(geometry, &vertexCount, &indexCount) == RASM_OK);
    CHECK(vertexCount == 4);
    rasm_vector_geometry_destroy(geometry);

    // The new fields come after the 1.10 ones, so a caller of an older version passes a smaller desc.
    CHECK(offsetof(rasm_material_desc, glyphs) > offsetof(rasm_material_desc, points));
    CHECK(offsetof(rasm_material_desc, vector_colors) == offsetof(rasm_material_desc, glyphs) + sizeof(rasm_scalar_colors));
    CHECK(offsetof(rasm_frame_stats, glyphs) > offsetof(rasm_frame_stats, lights_dropped));
    CHECK(offsetof(rasm_frame_stats, glyphs_visible) == offsetof(rasm_frame_stats, glyphs) + sizeof(uint32_t));
    rasm_frame_stats stats = {.size = sizeof(rasm_frame_stats)};
    CHECK(rasm_get_frame_stats(nullptr, &stats) == RASM_ERROR_INVALID_ARGUMENT);
}
