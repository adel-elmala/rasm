#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"
#include "exampleBench.h"

#if defined(RASM_IMGUI)
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Several views of one scene in one frame, with Engine::render(views, count). Each view is a camera's picture of the
// scene, tone mapped into a rect of the window or into a texture:
// - layout 1: the main camera in the whole window, with a top-down map as a picture-in-picture;
// - layout 2: split screen, the main camera on the left and a side camera on the right, and the map;
// - layout 3: four views, the main, side, map and security cameras.
// A security camera that pans over the scene draws into a view texture, which the TV in the scene shows as its
// emissive texture and the UI window shows with ImGui::Image. It is listed first, so the views after it show this
// frame's picture; the security camera itself sees last frame's on the TV. The map keeps no bloom and no TAA
// (View::bloom, View::antiAliasing), so it costs less. Each window view has its own camera rig, which takes the input
// of its own rect (getInputArea): drag in a view to orbit its camera, wheel to zoom; the map takes no input, so a drag
// that starts on it turns the view under it. Keys 1, 2 and 3 pick the layout.
// A second window (Engine::addWindow; not with --bench) shows an orbit camera that turns around the scene; drag in it to turn it
// yourself. Closing it is an event (InputEventType::WINDOW_CLOSE): the example removes it, and N opens it again. Closing
// the main window quits.
// --layout 1|2|3 picks the first one (2); --bench measures that layout's GPU time (exampleBench.h); --async-compute
// culls on the compute queue (EngineConfig::asyncCompute). A headless run plays a script first, so the checks of
// verify-change cover the changes too: layout 3, then 1, then --layout's from frame 40, the TV's texture at a new size
// from frame 30, and the second window removed at frame 25 while a view still names it and opened again at frame 35.
// Each change builds the frame's graph again; the views that stay keep their culling and TAA history. The second
// window is an offscreen image under --headless, and its last frame goes to <out>_window2.png.

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr uint32_t TV_WIDTH = 320;
    constexpr uint32_t TV_HEIGHT = 180;
    constexpr float MAP_HALF_SIZE = 9.0f;
    constexpr uint32_t SECOND_WIDTH = 640;
    constexpr uint32_t SECOND_HEIGHT = 360;

    // A window view's camera with its own rig and its own input.
    struct ViewCamera
    {
        rasm::CameraHandle camera;
        rasm::CameraRig rig;
        rasm::CameraInputState input;
    };
}

namespace
{
    rasm::MeshHandle createCube(rasm::Engine &engine)
    {
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const auto &n : normals)
        {
            const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::cross(glm::vec3(0, 1, 0), n);
            const glm::vec3 v = std::abs(n.y) > 0.5f ? glm::cross(n, u) : glm::vec3(0, 1, 0);
            const glm::vec3 corners[4] = {-u - v, u - v, u + v, -u + v};
            const auto first = static_cast<uint32_t>(vertices.size());
            for (int i = 0; i < 4; ++i)
                vertices.push_back({.pos = (n + corners[i]) * 0.5f, .normal = n, .uv = {uvs[i], uvs[i], uvs[i]}, .tangent = glm::vec4(u, 1.0f)});
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(first + index);
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec3 &color, float roughness)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::PBR);
        rasm::Material values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = 0.0f;
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        engine.updateMaterial(material, values);
        return material;
    }

    // A screen that glows with the view texture: its emissive texture is the texture's color, which the views listed
    // after the security camera's sample in the same frame.
    rasm::MaterialHandle createScreenMaterial(rasm::Engine &engine, rasm::TextureHandle picture)
    {
        const rasm::MaterialHandle material = createColorMaterial(engine, glm::vec3(0.02f), 0.3f);
        rasm::Material values = engine.getMaterial(material);
        values.textures[static_cast<uint32_t>(rasm::PbrSlot::EMISSIVE)] = picture;
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::EMISSIVE_FACTOR) + i] = 1.0f;
        values.params[static_cast<uint32_t>(rasm::PbrParam::EMISSIVE_STRENGTH)] = 1.0f;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::Transform lookAt(const glm::vec3 &eye, const glm::vec3 &center)
    {
        return rasm::Transform{eye, glm::quatLookAt(glm::normalize(center - eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)};
    }

    rasm::CameraProjection perspective(float aspect)
    {
        return rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 45.0f, .aspect = aspect, .nearZ = 0.1f}};
    }

    // An orbit around center from eye, which the application drives: the engine's controller is NONE.
    ViewCamera createViewCamera(rasm::Engine &engine, const glm::vec3 &eye, const glm::vec3 &center)
    {
        ViewCamera view;
        const rasm::Transform pose = lookAt(eye, center);
        view.camera = engine.createCamera(perspective(static_cast<float>(WIDTH) / static_cast<float>(HEIGHT)), pose);
        engine.setCameraController(view.camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        view.rig = rasm::createCameraRig(rasm::CameraController{.type = rasm::CameraController::Type::TRACK_BALL,
                                                                .trackBall = {.radius = glm::length(eye - center), .center = center, .orbitSensitivity = 1.0f},
                                                                .easingSpeed = 8.0f},
                                         pose);
        return view;
    }

    // The rects of each layout, in the order of the window views: main, side, map, security.
    struct Layout
    {
        rasm::ViewRect rects[4];
        bool shown[4];
    };

    Layout getLayout(int layout)
    {
        const rasm::ViewRect pip = {.x = 0.74f, .y = 0.02f, .width = 0.24f, .height = 0.24f * 16.0f / 9.0f};
        if (layout == 1)
            return {.rects = {{}, {}, pip, {}}, .shown = {true, false, true, false}};
        if (layout == 3)
            return {.rects = {{.x = 0.0f, .y = 0.0f, .width = 0.5f, .height = 0.5f},
                              {.x = 0.5f, .y = 0.0f, .width = 0.5f, .height = 0.5f},
                              {.x = 0.0f, .y = 0.5f, .width = 0.5f, .height = 0.5f},
                              {.x = 0.5f, .y = 0.5f, .width = 0.5f, .height = 0.5f}},
                    .shown = {true, true, true, true}};
        return {.rects = {{.x = 0.0f, .y = 0.0f, .width = 0.5f, .height = 1.0f}, {.x = 0.5f, .y = 0.0f, .width = 0.5f, .height = 1.0f}, pip, {}},
                .shown = {true, true, true, false}};
    }

    // An invalid handle and the main window's are one window.
    bool isSameWindow(const rasm::Engine &engine, rasm::WindowHandle a, rasm::WindowHandle b)
    {
        const rasm::WindowHandle main = engine.getMainWindow().handle;
        return (a.isValid() ? a : main) == (b.isValid() ? b : main);
    }

    // One frame of a view's camera: its rig takes the frame's events of its window, in its own rect, so a drag that
    // starts in one view turns only that view's camera. The UI lives in the main window, so only there it takes input.
    void updateViewCamera(rasm::Engine &engine, ViewCamera &view, const rasm::ViewRect &rect, rasm::WindowHandle window = {})
    {
        const rasm::InputArea area = rasm::getInputArea(engine.getWindow(window), rect);
        const bool main = isSameWindow(engine, window, {});
        const rasm::UiInputCapture capture = main ? engine.getUiInputCapture() : rasm::UiInputCapture{};
        uint32_t count = 0;
        const rasm::InputEvent *events = engine.getInputEvents(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (isSameWindow(engine, events[i].window, window))
                rasm::addCameraInputEvent(view.input, events[i], area, capture);
        }
        rasm::updateCameraRig(view.rig, rasm::takeCameraInput(view.input, capture, engine.getDeltaTime()));
        engine.setCameraTransform(view.camera, view.rig.transform);
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    int layout = 2;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--layout") == 0)
            layout = std::clamp(std::atoi(argv[i + 1]), 1, 3);
    }
    bool asyncCompute = false;
    for (int i = 1; i < argc; ++i)
        asyncCompute = asyncCompute || std::strcmp(argv[i], "--async-compute") == 0;

    rasm::EngineConfig config = {
        .appName = "example_20",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .asyncCompute = asyncCompute,
        .bloom = true,
        .ui = true,
        .antiAliasing = rasm::AntiAliasing::TAA};
    applyExampleArgs(args, config);
    // --bench measures the GPU time of one layout (exampleBench.h): no script, no validation, no UI, 1920 x 1080.
    ExampleBench bench{.on = hasBenchFlag(argc, argv)};
    if (bench.on)
    {
        config.enableValidation = false;
        config.ui = false;
        config.showStats = false;
        config.windowWidth = 1920;
        config.windowHeight = 1080;
    }
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    const rasm::SceneHandle scene = engine.createScene();
    const rasm::EnvironmentHandle sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    const rasm::MeshHandle helmet = engine.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
    const rasm::MeshHandle cube = createCube(engine);
    if (!sunset.isValid() || !helmet.isValid() || !cube.isValid())
        return 1;
    engine.setSceneEnvironment(scene, {.environment = sunset, .intensity = 1.0f, .rotation = 1.15f});

    // The view texture: one sRGB color and no depth, which the tone-map pass draws the security camera into.
    const rasm::RenderTargetHandle tv = engine.createRenderTarget({.name = "tv",
                                                                   .width = TV_WIDTH,
                                                                   .height = TV_HEIGHT,
                                                                   .colorFormats = {rasm::Format::R8G8B8A8_SRGB},
                                                                   .clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)});
    if (!tv.isValid())
        return 1;
    // A target without history keeps one texture, and its handle stays when the target is resized.
    const rasm::TextureHandle tvPicture = engine.getRenderTargetTexture(tv);

    auto add = [&](const char *name, rasm::MaterialHandle material, const glm::vec3 &position, const glm::vec3 &scale)
    { engine.addEntityToScene(scene, engine.createEntity(name, cube, material, rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), scale})); };
    add("ground", createColorMaterial(engine, glm::vec3(0.55f), 0.9f), glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(2.0f * MAP_HALF_SIZE, 0.5f, 2.0f * MAP_HALF_SIZE));
    const glm::vec3 colors[4] = {{0.8f, 0.2f, 0.15f}, {0.2f, 0.6f, 0.25f}, {0.2f, 0.35f, 0.8f}, {0.85f, 0.7f, 0.2f}};
    for (uint32_t i = 0; i < 4; ++i)
    {
        const float angle = glm::radians(45.0f + 90.0f * static_cast<float>(i));
        add("box", createColorMaterial(engine, colors[i], 0.5f), glm::vec3(4.5f * std::cos(angle), 0.6f, 4.5f * std::sin(angle)), glm::vec3(1.2f));
    }
    // The TV behind the helmet, facing the main camera: a dark frame and the glowing screen in front of it.
    add("tv frame", createColorMaterial(engine, glm::vec3(0.05f), 0.6f), glm::vec3(0.0f, 1.9f, -3.0f), glm::vec3(3.4f, 2.0f, 0.15f));
    add("tv screen", createScreenMaterial(engine, tvPicture), glm::vec3(0.0f, 1.9f, -2.92f), glm::vec3(3.2f, 1.8f, 0.02f));
    engine.addEntityToScene(scene, engine.createEntity("helmet", helmet, {}, rasm::Transform{glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)}));

    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.85f, 0.7f), glm::vec3(0.0f), glm::normalize(glm::vec3(0.5f, -0.8f, -0.4f)), 2.5f, 0.0f);
    engine.addLightToScene(scene, sun);

    ViewCamera main = createViewCamera(engine, glm::vec3(0.0f, 2.2f, 7.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    ViewCamera side = createViewCamera(engine, glm::vec3(7.0f, 2.5f, 2.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    ViewCamera orbit = createViewCamera(engine, glm::vec3(-6.0f, 4.0f, -6.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    // The map looks straight down; its width is the ground's, and a view of another shape shows more or less of it.
    const rasm::CameraHandle map = engine.createCamera(
        rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                               .orthographic{.left = -MAP_HALF_SIZE, .right = MAP_HALF_SIZE, .bottom = -MAP_HALF_SIZE, .top = MAP_HALF_SIZE, .nearZ = 0.1f, .farZ = 40.0f}},
        rasm::Transform{glm::vec3(0.0f, 20.0f, 0.0f), glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(1.0f)});
    const rasm::CameraHandle security = engine.createCamera(perspective(static_cast<float>(TV_WIDTH) / static_cast<float>(TV_HEIGHT)), lookAt(glm::vec3(5.0f, 3.5f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    if (!main.camera.isValid() || !side.camera.isValid() || !orbit.camera.isValid() || !map.isValid() || !security.isValid() || !sun.isValid())
        return 1;
    // The second window: an SDL window the engine opens, or an offscreen image of the same size under --headless.
    const rasm::WindowDesc secondDesc = {.title = "RASM: a second window", .width = SECOND_WIDTH, .height = SECOND_HEIGHT};
    rasm::WindowHandle second = bench.on ? rasm::WindowHandle{} : engine.addWindow(secondDesc);
    if (!bench.on && !second.isValid())
        return 1;
    engine.setCameraController(map, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
    engine.setCameraController(security, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
    for (const rasm::CameraHandle camera : {main.camera, side.camera, orbit.camera, map, security})
        engine.addCameraToScene(scene, camera);

#if defined(RASM_IMGUI)
    engine.setUiCallback(
        [&]()
        {
            const float scale = ImGui::GetStyle().FontScaleDpi;
            ImGui::SetNextWindowPos(ImVec2(10.0f * scale, static_cast<float>(engine.getMainWindow().height) - 10.0f * scale), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::Begin("Views");
            ImGui::RadioButton("1: one view", &layout, 1);
            ImGui::RadioButton("2: split screen", &layout, 2);
            ImGui::RadioButton("3: four views", &layout, 3);
            ImGui::Text("The security camera's view texture:");
            ImGui::Image(ImTextureRef(engine.getUiTexture(tvPicture)), ImVec2(static_cast<float>(TV_WIDTH) * 0.75f * scale, static_cast<float>(TV_HEIGHT) * 0.75f * scale));
            ImGui::End();
        });
#endif

    if (args.waitForLoads)
        engine.waitForLoads();

    // The list keeps its capacity, so building it every frame allocates nothing.
    std::vector<rasm::View> views;
    views.reserve(6);
    rasm::ReadbackHandle secondReadback;
    float time = 0.0f;
    const int chosenLayout = layout;
    uint32_t frame = 0;
    while (exampleRunning(engine, args))
    {
        ++frame;
        if (args.headless && !bench.on)
        {
            layout = frame < 20 ? 3 : (frame < 40 ? 1 : chosenLayout);
            // Between frames: a new size waits for the frames in flight once, and the TV's material keeps its texture.
            if (frame == 30 && !engine.resizeRenderTarget(tv, TV_WIDTH * 3 / 2, TV_HEIGHT * 3 / 2))
                return 1;
            // The second window goes while its view is still in the list (that view is left out), then comes back.
            if (frame == 25)
                engine.removeWindow(second);
            if (frame == 35)
                second = engine.addWindow(secondDesc);
            // The same frame as the main window's copy (exampleRunning asks for it before this frame too).
            if (frame == args.frames && engine.getWindow(second).handle.isValid())
                secondReadback = engine.readFrame(second);
        }
        engine.pollEvents();
        uint32_t count = 0;
        const rasm::InputEvent *events = engine.getInputEvents(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const rasm::InputEvent &event = events[i];
            if (event.type == rasm::InputEventType::KEY && event.down && !event.repeat && !engine.getUiInputCapture().keyboard &&
                event.key >= rasm::Key::DIGIT_1 && event.key <= rasm::Key::DIGIT_3)
                layout = 1 + static_cast<int>(event.key) - static_cast<int>(rasm::Key::DIGIT_1);
            // Closing the second window is the application's to handle; N opens it again.
            if (event.type == rasm::InputEventType::WINDOW_CLOSE && event.window == second)
            {
                engine.removeWindow(second);
                second = {};
            }
            if (event.type == rasm::InputEventType::KEY && event.down && !event.repeat && event.key == rasm::Key::N && !second.isValid() && !engine.getUiInputCapture().keyboard)
                second = engine.addWindow(secondDesc);
        }
        const Layout rects = getLayout(layout);
        updateViewCamera(engine, main, rects.rects[0]);
        if (rects.shown[1])
            updateViewCamera(engine, side, rects.rects[1]);
        if (second.isValid())
            updateViewCamera(engine, orbit, {}, second);
        // Untouched, the orbit camera turns slowly around the scene.
        if (!orbit.input.dragActive)
        {
            const rasm::Transform pose = orbit.rig.transform;
            const glm::quat turn = glm::angleAxis(0.2f * engine.getDeltaTime(), glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 center(0.0f, 1.0f, 0.0f);
            orbit.rig = rasm::createCameraRig(orbit.rig.controller, rasm::Transform{center + turn * (pose.position - center), turn * pose.rotation, pose.scale});
            engine.setCameraTransform(orbit.camera, orbit.rig.transform);
        }

        // The security camera pans to and fro over the scene.
        time += engine.getDeltaTime();
        const float pan = 0.6f * std::sin(0.5f * time);
        engine.setCameraTransform(security, lookAt(glm::vec3(5.0f, 3.5f, 5.0f), glm::vec3(4.0f * pan, 1.0f, -4.0f * pan)));

        // The view texture first, so the views after it show this frame's picture; then the window's views, a later
        // rect over an earlier one, the map last.
        views.clear();
        views.push_back({.scene = scene, .camera = security, .output = {.target = tv}, .bloom = rasm::ViewToggle::OFF, .antiAliasing = rasm::AntiAliasing::NONE});
        views.push_back({.scene = scene, .camera = main.camera, .output = {.rect = rects.rects[0]}});
        if (rects.shown[1])
            views.push_back({.scene = scene, .camera = side.camera, .output = {.rect = rects.rects[1]}});
        if (rects.shown[3])
            views.push_back({.scene = scene, .camera = security, .output = {.rect = rects.rects[3]}});
        views.push_back({.scene = scene, .camera = map, .output = {.rect = rects.rects[2]}, .bloom = rasm::ViewToggle::OFF, .antiAliasing = rasm::AntiAliasing::NONE});
        // The second window's one view, which sees the TV too. A removed window's view may stay listed: it draws nothing.
        if (second.isValid())
            views.push_back({.scene = scene, .camera = orbit.camera, .output = {.window = second}});
        engine.render(views.data(), static_cast<uint32_t>(views.size()));
        addBenchFrame(bench, engine.getFrameStats());
    }
    printBench(bench, config, engine.getRenderContext().hasMeshShaders());
    if (secondReadback.isValid())
    {
        // <out> with _window2 before its extension.
        const std::string out = args.out;
        const size_t dot = out.find_last_of('.');
        const std::string path = (dot == std::string::npos ? out : out.substr(0, dot)) + "_window2.png";
        rasm::ReadbackImage image;
        if (!engine.takeReadback(secondReadback, image) || !rasm::writePng(path, image))
        {
            std::fprintf(stderr, "error: the second window's frame was not written to %s.\n", path.c_str());
            return 1;
        }
        std::printf("Wrote the second window's frame %u to %s.\n", args.frames, path.c_str());
    }
    return exampleExit(args);
}
