#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include "SDL3/SDL.h"
#if defined(__APPLE__)
#include "SDL3/SDL_metal.h"
#endif
#if defined(RASM_IMGUI)
#include "imgui.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// RASM in a window the application owns, with a rasm::Renderer and a rasm::AssetLoader for its files, no Engine. The
// application opens an SDL3 window and runs its event loop, as a game with its own window or an editor would; the
// renderer gets the window's native handle (RendererConfig::nativeWindow: an HWND, a CAMetalLayer, an X11 window or a Wayland surface)
// and draws into it, and the application passes on the input (Renderer::addInputEvent), the size (setWindowSize) and
// the display's scale (setWindowContentScale). Escape and closing the window end the loop: the renderer never quits
// by itself. The application also drives the camera: the renderer's camera has no controller
// (CameraController::Type::NONE), and the example's own CameraRig takes each frame's events through a
// CameraInputState, then gives the renderer the pose. Drag to orbit the helmet, F to fly instead (W/A/S/D, drag to
// look) and back; the UI window has a checkbox and a text field.
// --headless opens the window hidden and plays a scripted input instead of the user's: a drag that orbits the camera,
// a click on the checkbox and the word RASM typed into the text field, then writes frame --frames (60) to --out, as
// every example does, so the native surface, the swapchain and the input path run with nothing on screen. The script
// needs about 40 frames. --no-window plays the same script on a renderer without a window (offscreen images), for a
// machine that cannot present to a hidden window.
// A second window of the application's shows the helmet from above (Renderer::addWindow with its native handle, a view
// whose ViewOutput names it). The application passes its size on with setWindowSize(window, ...), and removes it when
// the user closes it (removeWindow). Under --headless it is hidden too, and its last frame goes to <out>_window2.png;
// with --no-window it is an offscreen image of the headless renderer. One submit draws both windows and presents both.

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr uint32_t SECOND_WIDTH = 480;
    constexpr uint32_t SECOND_HEIGHT = 360;

    struct UiTargets
    {
        float checkbox[2] = {};
        float textField[2] = {};
    };
}

namespace
{
    rasm::MeshHandle createCube(rasm::Renderer &renderer)
    {
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const auto &n : normals)
        {
            const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 v = glm::cross(n, u);
            const glm::vec3 corners[4] = {n - u - v, n + u - v, n + u + v, n - u + v};
            const auto first = static_cast<uint32_t>(vertices.size());
            for (int i = 0; i < 4; ++i)
                vertices.push_back({.pos = corners[i] * 0.5f, .normal = n, .uv = {uvs[i], uvs[i], uvs[i]}, .tangent = glm::vec4(0.0f)});
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(first + index);
        }
        return renderer.createMesh(std::move(vertices), std::move(indices));
    }

    // The window's handles, as the device makes its surface from them. On macOS a Metal view gives the window a
    // CAMetalLayer, which must outlive the renderer.
    bool getNativeWindow(SDL_Window *window, rasm::NativeWindow &native, SDL_MetalView &metalView)
    {
        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
        (void)properties;
        (void)metalView;
#if defined(_WIN32)
        native.type = rasm::NativeWindowType::WINDOWS;
        native.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
        native.window = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
        metalView = SDL_Metal_CreateView(window);
        native.type = rasm::NativeWindowType::METAL;
        native.window = metalView != nullptr ? SDL_Metal_GetLayer(metalView) : nullptr;
#else
        if (std::strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0)
        {
            native.type = rasm::NativeWindowType::WAYLAND;
            native.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
            native.window = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        }
        else
        {
            native.type = rasm::NativeWindowType::X11;
            native.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            native.window = reinterpret_cast<void *>(static_cast<uintptr_t>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
        }
#endif
        int pixelWidth = 0;
        int pixelHeight = 0;
        SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
        native.pixelWidth = static_cast<uint32_t>(pixelWidth);
        native.pixelHeight = static_cast<uint32_t>(pixelHeight);
        const float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
        native.contentScale = scale > 0.0f ? scale : 1.0f;
        return native.window != nullptr;
    }

    // The key's meaning in the layout. RASM's SDL platform maps every key; this maps what the camera, the UI's text
    // field and its shortcuts use.
    rasm::Key toKey(SDL_Keycode key)
    {
        if (key >= SDLK_A && key <= SDLK_Z)
            return static_cast<rasm::Key>(static_cast<uint32_t>(rasm::Key::A) + (key - SDLK_A));
        if (key >= SDLK_0 && key <= SDLK_9)
            return static_cast<rasm::Key>(static_cast<uint32_t>(rasm::Key::DIGIT_0) + (key - SDLK_0));
        switch (key)
        {
        case SDLK_F1:
            return rasm::Key::F1;
        case SDLK_F2:
            return rasm::Key::F2;
        case SDLK_F3:
            return rasm::Key::F3;
        case SDLK_ESCAPE:
            return rasm::Key::ESCAPE;
        case SDLK_RETURN:
            return rasm::Key::ENTER;
        case SDLK_TAB:
            return rasm::Key::TAB;
        case SDLK_BACKSPACE:
            return rasm::Key::BACKSPACE;
        case SDLK_DELETE:
            return rasm::Key::DEL;
        case SDLK_HOME:
            return rasm::Key::HOME;
        case SDLK_END:
            return rasm::Key::END;
        case SDLK_LEFT:
            return rasm::Key::LEFT;
        case SDLK_RIGHT:
            return rasm::Key::RIGHT;
        case SDLK_UP:
            return rasm::Key::UP;
        case SDLK_DOWN:
            return rasm::Key::DOWN;
        case SDLK_LSHIFT:
            return rasm::Key::LEFT_SHIFT;
        case SDLK_RSHIFT:
            return rasm::Key::RIGHT_SHIFT;
        case SDLK_LCTRL:
            return rasm::Key::LEFT_CTRL;
        case SDLK_RCTRL:
            return rasm::Key::RIGHT_CTRL;
        default:
            return rasm::Key::NONE;
        }
    }

    // Where the key is: W/A/S/D move the camera by position, also on AZERTY. Other keys are left NONE, which RASM reads
    // as "the same as key".
    rasm::Key toPhysicalKey(SDL_Scancode scancode)
    {
        if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
            return static_cast<rasm::Key>(static_cast<uint32_t>(rasm::Key::A) + (scancode - SDL_SCANCODE_A));
        return rasm::Key::NONE;
    }

    uint8_t toModifiers(SDL_Keymod mod)
    {
        return static_cast<uint8_t>(((mod & SDL_KMOD_SHIFT) != 0 ? rasm::KEY_MOD_SHIFT : 0) | ((mod & SDL_KMOD_CTRL) != 0 ? rasm::KEY_MOD_CTRL : 0) |
                                    ((mod & SDL_KMOD_ALT) != 0 ? rasm::KEY_MOD_ALT : 0) | ((mod & SDL_KMOD_GUI) != 0 ? rasm::KEY_MOD_SUPER : 0));
    }

    // One SDL event to RASM. Returns false when the application should stop.
    bool forwardEvent(rasm::Renderer &renderer, SDL_Window *window, const SDL_Event &event)
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return false;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (event.key.key == SDLK_ESCAPE && event.key.down)
                return false;
            renderer.addInputEvent({.type = rasm::InputEventType::KEY,
                                    .key = toKey(event.key.key),
                                    .physicalKey = toPhysicalKey(event.key.scancode),
                                    .down = event.key.down,
                                    .repeat = event.key.repeat,
                                    .modifiers = toModifiers(event.key.mod)});
            break;
        case SDL_EVENT_TEXT_INPUT:
            // ASCII only, to stay short: RASM's SDL platform decodes all of UTF-8.
            for (const char *c = event.text.text; *c != 0; ++c)
            {
                if (static_cast<unsigned char>(*c) < 0x80)
                    renderer.addInputEvent({.type = rasm::InputEventType::TEXT, .codepoint = static_cast<uint32_t>(*c)});
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button >= SDL_BUTTON_LEFT && event.button.button <= SDL_BUTTON_X2)
            {
                // SDL numbers left, middle, right; RASM left, right, middle.
                const rasm::MouseButton buttons[] = {rasm::MouseButton::LEFT, rasm::MouseButton::MIDDLE, rasm::MouseButton::RIGHT, rasm::MouseButton::BACK, rasm::MouseButton::FORWARD};
                renderer.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = event.button.down, .button = buttons[event.button.button - 1], .x = event.button.x, .y = event.button.y});
                SDL_CaptureMouse(event.button.down || SDL_GetMouseState(nullptr, nullptr) != 0);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            renderer.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = event.motion.x, .y = event.motion.y});
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            renderer.addInputEvent({.type = rasm::InputEventType::MOUSE_WHEEL, .x = event.wheel.x, .y = event.wheel.y});
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            renderer.addInputEvent({.type = rasm::InputEventType::MOUSE_LEAVE});
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            renderer.addInputEvent({.type = rasm::InputEventType::FOCUS, .down = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED});
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        {
            int width = 0;
            int height = 0;
            int pixelWidth = 0;
            int pixelHeight = 0;
            SDL_GetWindowSize(window, &width, &height);
            SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
            // A minimized window is drawn at size 0: the renderer skips its frames.
            const bool minimized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
            renderer.setWindowSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height), minimized ? 0 : static_cast<uint32_t>(pixelWidth), minimized ? 0 : static_cast<uint32_t>(pixelHeight));
            break;
        }
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        {
            const float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
            if (scale > 0.0f)
                renderer.setWindowContentScale(scale);
            break;
        }
        default:
            break;
        }
        return true;
    }

    // The second window's events: only its size and its closing matter, since its view takes no input.
    void forwardSecondEvent(rasm::Renderer &renderer, SDL_Window *&window, rasm::WindowHandle &handle, const SDL_Event &event)
    {
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        {
            // The renderer lets go of its swapchain first; the window goes after it.
            renderer.removeWindow(handle);
            handle = {};
            SDL_HideWindow(window);
            return;
        }
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_MINIMIZED ||
            event.type == SDL_EVENT_WINDOW_RESTORED)
        {
            int width = 0;
            int height = 0;
            int pixelWidth = 0;
            int pixelHeight = 0;
            SDL_GetWindowSize(window, &width, &height);
            SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
            const bool minimized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
            renderer.setWindowSize(handle, static_cast<uint32_t>(width), static_cast<uint32_t>(height), minimized ? 0 : static_cast<uint32_t>(pixelWidth), minimized ? 0 : static_cast<uint32_t>(pixelHeight));
        }
    }

    // The scripted user of --headless: frame by frame, the events a person would make.
    void playScript(rasm::Renderer &renderer, uint32_t frame, const UiTargets &targets)
    {
        const float x = 0.5f * static_cast<float>(WIDTH);
        const float y = 0.6f * static_cast<float>(HEIGHT);
        auto move = [&](float px, float py)
        { renderer.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = px, .y = py}); };
        auto button = [&](float px, float py, bool down)
        { renderer.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = down, .x = px, .y = py}); };

        // A drag to the right and a little down orbits the camera around the helmet.
        if (frame == 2)
            move(x, y);
        if (frame == 3)
            button(x, y, true);
        if (frame >= 4 && frame <= 18)
            move(x + 14.0f * static_cast<float>(frame - 3), y + 2.0f * static_cast<float>(frame - 3));
        if (frame == 19)
            button(x + 14.0f * 15.0f, y + 30.0f, false);
        // A click on the checkbox, then one in the text field and a word typed into it, where the UI put them.
        if (frame == 22)
            move(targets.checkbox[0], targets.checkbox[1]);
        if (frame == 23)
            button(targets.checkbox[0], targets.checkbox[1], true);
        if (frame == 24)
            button(targets.checkbox[0], targets.checkbox[1], false);
        if (frame == 27)
            move(targets.textField[0], targets.textField[1]);
        if (frame == 28)
            button(targets.textField[0], targets.textField[1], true);
        if (frame == 29)
            button(targets.textField[0], targets.textField[1], false);
        if (frame == 32)
        {
            for (const char c : {'R', 'A', 'S', 'M'})
                renderer.addInputEvent({.type = rasm::InputEventType::TEXT, .codepoint = static_cast<uint32_t>(c)});
        }
    }

    // An orbit around the helmet from where the camera is: at its distance, turned at once to look at the helmet. A
    // trackball always looks at its center, so a rig that does not would jump on its first frame.
    rasm::CameraRig orbitFrom(const rasm::CameraController &orbit, const rasm::Transform &pose)
    {
        const glm::vec3 offset = pose.position - orbit.trackBall.center;
        const float distance = glm::length(offset);
        if (distance < 1e-3f)
            return rasm::createCameraRig(orbit, pose);
        rasm::CameraController controller = orbit;
        controller.trackBall.radius = distance;
        const glm::quat lookAtCenter = glm::quatLookAt(-offset / distance, glm::vec3(0.0f, 1.0f, 0.0f));
        return rasm::createCameraRig(controller, rasm::Transform{pose.position, lookAtCenter, pose.scale});
    }

    // One frame of the application's camera: its rig takes the events of the last pollEvents, in the window's area, and
    // skips what the UI took. F switches between orbit and fly from where the camera is.
    void updateCamera(rasm::Renderer &renderer, rasm::CameraHandle camera, rasm::CameraRig &rig, rasm::CameraInputState &cameraInput, const rasm::CameraController &orbit, const rasm::CameraController &fly)
    {
        const rasm::UiInputCapture capture = renderer.getUiInputCapture();
        const rasm::Window &window = renderer.getMainWindow();
        const rasm::InputArea area = {.width = static_cast<float>(window.width), .height = static_cast<float>(window.height)};
        uint32_t count = 0;
        const rasm::InputEvent *events = renderer.getInputEvents(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            rasm::addCameraInputEvent(cameraInput, events[i], area, capture);
            const bool switchKey = events[i].type == rasm::InputEventType::KEY && events[i].key == rasm::Key::F && events[i].down && !events[i].repeat;
            if (switchKey && !capture.keyboard)
            {
                const bool flying = rig.controller.type == rasm::CameraController::Type::FREE_FLY;
                rig = flying ? orbitFrom(orbit, rig.transform) : rasm::createCameraRig(fly, rig.transform);
            }
        }
        rasm::updateCameraRig(rig, rasm::takeCameraInput(cameraInput, capture, renderer.getDeltaTime()));
        renderer.setCameraTransform(camera, rig.transform);
    }

    int run(ExampleArgs &args, SDL_Window *window, const rasm::NativeWindow &native, SDL_Window *secondWindow, const rasm::NativeWindow &secondNative)
    {
        rasm::RendererConfig config = {
            .appName = "example_19",
            .windowWidth = WIDTH,
            .windowHeight = HEIGHT,
            .enableValidation = true,
            .preferredBackend = rasm::Backend::VULKAN,
            .shaderDir = RASM_EXAMPLE_SHADER_DIR,
            .ui = true};
        applyExampleArgs(args, config);
        // With a window, also a hidden one, RASM draws into the application's; --no-window gives it none, so it draws into
        // offscreen images.
        if (window != nullptr)
            config.nativeWindow = native;

        rasm::Renderer renderer(config);
        if (!renderer.running())
        {
            if (window != nullptr && args.headless)
                std::fprintf(stderr, "error: RASM could not draw into the hidden window (see the log). --no-window runs the same script without a window.\n");
            return 1;
        }
        // The files load on the loader's workers, and the renderer finishes them at the start of each frame. Made after
        // the renderer, so it is destroyed first, as it must be.
        rasm::AssetLoader loader(renderer, {.assetRoot = RASM_EXAMPLE_ASSET_ROOT});

        const rasm::SceneHandle scene = renderer.createScene();
        const rasm::EnvironmentHandle sunset = loader.loadEnvironment("environments/venice_sunset_2k.hdr");
        const rasm::MeshHandle helmet = loader.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
        const rasm::MeshHandle cube = createCube(renderer);
        if (!sunset.isValid() || !helmet.isValid() || !cube.isValid())
            return 1;
        renderer.setSceneEnvironment(scene, {.environment = sunset, .intensity = 1.0f, .rotation = 1.15f});

        const rasm::MaterialHandle ground = renderer.createMaterial(rasm::MaterialType::PBR);
        rasm::Material groundValues = renderer.getMaterial(ground);
        groundValues.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.9f;
        renderer.updateMaterial(ground, groundValues);
        renderer.addEntityToScene(scene, renderer.createEntity("ground", cube, ground, rasm::Transform{glm::vec3(0.0f, -1.2f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(20.0f, 0.5f, 20.0f)}));
        renderer.addEntityToScene(scene, renderer.createEntity("helmet", helmet, {}, rasm::Transform{}));

        // Looking at the helmet, so the camera is on its orbit from the start.
        const glm::vec3 startPosition(0.0f, 0.4f, 4.2f);
        const rasm::Transform start{startPosition, glm::quatLookAt(-glm::normalize(startPosition), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)};
        const rasm::CameraHandle camera = renderer.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                                       .perspective{.fovY = 45.0f, .aspect = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT), .nearZ = 0.1f}},
                                                                start);
        renderer.setCameraController(camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        const rasm::CameraController orbit{.type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = 4.2f, .center = glm::vec3(0.0f), .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f};
        const rasm::CameraController fly{.type = rasm::CameraController::Type::FREE_FLY,
                                         .freeFly = {.speed = 2.0f, .lookSensitivity = 1.5f},
                                         .easingSpeed = 6.0f};
        rasm::CameraRig rig = orbitFrom(orbit, start);
        rasm::CameraInputState cameraInput;
        renderer.addCameraToScene(scene, camera);

        const rasm::LightHandle sun = renderer.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.8f, 0.6f), glm::vec3(0.0f), glm::normalize(glm::vec3(-0.6f, -0.7f, -0.4f)), 2.0f, 0.0f);
        renderer.addLightToScene(scene, sun);
        if (!camera.isValid() || !sun.isValid())
            return 1;

        // The second window: the application's (NATIVE), or an offscreen image of a headless renderer. Its camera looks
        // down at the helmet from above.
        rasm::WindowHandle second = renderer.addWindow(secondWindow != nullptr ? rasm::WindowDesc{.width = SECOND_WIDTH, .height = SECOND_HEIGHT, .native = secondNative}
                                                                               : rasm::WindowDesc{.width = SECOND_WIDTH, .height = SECOND_HEIGHT});
        const glm::vec3 abovePosition(0.0f, 6.0f, 0.5f);
        const rasm::CameraHandle above = renderer.createCamera(
            rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                   .perspective{.fovY = 40.0f, .aspect = static_cast<float>(SECOND_WIDTH) / static_cast<float>(SECOND_HEIGHT), .nearZ = 0.1f}},
            rasm::Transform{abovePosition, glm::quatLookAt(-glm::normalize(abovePosition), glm::vec3(0.0f, 0.0f, -1.0f)), glm::vec3(1.0f)});
        if (!second.isValid() || !above.isValid())
            return 1;
        renderer.setCameraController(above, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        rasm::ReadbackHandle secondReadback;

        UiTargets targets;
#if defined(RASM_IMGUI)
        bool blueSun = false;
        char text[32] = "";
        renderer.setUiCallback(
            [&]()
            {
                const float scale = ImGui::GetStyle().FontScaleDpi;
                ImGui::SetNextWindowPos(ImVec2(10.0f * scale, 10.0f * scale), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(300.0f * scale, 0.0f), ImGuiCond_Always);
                ImGui::Begin("Embedded application");
                if (ImGui::Checkbox("Blue sun", &blueSun))
                {
                    rasm::Light light = renderer.getLight(sun);
                    light.color = blueSun ? glm::vec3(0.4f, 0.6f, 1.0f) : glm::vec3(1.0f, 0.8f, 0.6f);
                    renderer.updateLight(sun, light);
                }
                targets.checkbox[0] = 0.5f * (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x);
                targets.checkbox[1] = 0.5f * (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y);
                ImGui::InputText("Name", text, sizeof(text));
                targets.textField[0] = 0.5f * (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x);
                targets.textField[1] = 0.5f * (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y);
                ImGui::End();
            });
#endif

        if (args.waitForLoads)
            renderer.waitForLoads();

        // SDL starts text input on request; the UI asks for it while its text field has the focus.
        bool textInput = false;
        bool open = true;
        for (uint32_t frame = 1; open && exampleRunning(renderer, args); ++frame)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                // A scripted run takes no input from the hidden windows.
                if (args.headless)
                    continue;
                if (secondWindow != nullptr && SDL_GetWindowFromEvent(&event) == secondWindow)
                {
                    if (second.isValid())
                        forwardSecondEvent(renderer, secondWindow, second, event);
                }
                else
                    open = forwardEvent(renderer, window, event) && open;
            }
            if (args.headless)
                playScript(renderer, frame, targets);
            // The same frame as the main window's copy (exampleRunning asks for it before this frame too).
            if (args.headless && frame == args.frames && second.isValid())
                secondReadback = renderer.readFrame(second);

            // The frame's events, the same the UI gets, move the example's camera before the frame.
            renderer.pollEvents();
            updateCamera(renderer, camera, rig, cameraInput, orbit, fly);
            const rasm::View views[] = {{.scene = scene, .camera = camera}, {.scene = scene, .camera = above, .output = {.window = second}}};
            renderer.render(views, second.isValid() ? 2 : 1);

#if defined(RASM_IMGUI)
            if (window != nullptr && ImGui::GetIO().WantTextInput != textInput)
            {
                textInput = ImGui::GetIO().WantTextInput;
                if (textInput)
                    SDL_StartTextInput(window);
                else
                    SDL_StopTextInput(window);
            }
#endif
        }
        (void)textInput;
        if (secondReadback.isValid())
        {
            // <out> with _window2 before its extension.
            const std::string out = args.out;
            const size_t dot = out.find_last_of('.');
            const std::string path = (dot == std::string::npos ? out : out.substr(0, dot)) + "_window2.png";
            rasm::ReadbackImage image;
            if (!renderer.takeReadback(secondReadback, image) || !rasm::writePng(path, image))
            {
                std::fprintf(stderr, "error: the second window's frame was not written to %s.\n", path.c_str());
                return 1;
            }
            std::printf("Wrote the second window's frame %u to %s.\n", args.frames, path.c_str());
        }
        return exampleExit(args);
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    bool noWindow = false;
    for (int i = 1; i < argc; ++i)
        noWindow = noWindow || std::strcmp(argv[i], "--no-window") == 0;
    if (noWindow && !args.headless)
    {
        std::fprintf(stderr, "error: --no-window needs --headless.\n");
        return 1;
    }
    if (noWindow)
        return run(args, nullptr, {}, nullptr, {});

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
#if defined(__APPLE__)
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL;
#else
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
#endif
    if (args.headless)
        flags |= SDL_WINDOW_HIDDEN;
    SDL_Window *window = SDL_CreateWindow("RASM in an application's window", static_cast<int>(WIDTH), static_cast<int>(HEIGHT), flags);
    SDL_Window *secondWindow = SDL_CreateWindow("RASM: the helmet from above", static_cast<int>(SECOND_WIDTH), static_cast<int>(SECOND_HEIGHT), flags);
    rasm::NativeWindow native;
    rasm::NativeWindow secondNative;
    SDL_MetalView metalView = nullptr;
    SDL_MetalView secondMetalView = nullptr;
    int result = 1;
    if (window == nullptr || secondWindow == nullptr)
        std::fprintf(stderr, "error: SDL_CreateWindow failed: %s\n", SDL_GetError());
    else if (!getNativeWindow(window, native, metalView) || !getNativeWindow(secondWindow, secondNative, secondMetalView))
        std::fprintf(stderr, "error: SDL gave no native handle for its window: %s\n", SDL_GetError());
    else
        result = run(args, window, native, secondWindow, secondNative);

    // The renderer is gone by now: the windows and their layers go after it.
#if defined(__APPLE__)
    if (metalView != nullptr)
        SDL_Metal_DestroyView(metalView);
    if (secondMetalView != nullptr)
        SDL_Metal_DestroyView(secondMetalView);
#endif
    if (secondWindow != nullptr)
        SDL_DestroyWindow(secondWindow);
    if (window != nullptr)
        SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
