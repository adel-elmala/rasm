#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include "imgui.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

// Dear ImGui in RASM: the RASM window (F1) with the frame's stats, and a window of the application's own, built in the
// UI callback, that changes the sun, the exposure and the helmet while the frame runs, and shows a texture with
// ImGui::Image. Engine::render adds the UI pass after the tone map.
// --gpu-times prints each GPU timer's mean over frames 300 to 600, then quits; --no-ui runs the same scene with the UI
// off, for comparison. --debug-view <none|normals|uvs|overdraw|lods|culling|light-count> starts in that debug view, and
// --freeze-culling freezes culling at frame 30, as the RASM window's Debug views section does.

namespace
{
    rasm::MeshHandle createCube(rasm::Engine &engine)
    {
        // One quad per face, so each face has its own normal.
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const auto &n : normals)
        {
            // Two axes in the face's plane, ordered so the quad is counter-clockwise seen from outside.
            const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 v = glm::cross(n, u);
            const glm::vec3 corners[4] = {n - u - v, n + u - v, n + u + v, n - u + v};

            const auto first = static_cast<uint32_t>(vertices.size());
            for (int i = 0; i < 4; ++i)
                vertices.push_back({.pos = corners[i] * 0.5f, .normal = n, .uv = {uvs[i], uvs[i], uvs[i]}, .tangent = glm::vec4(0.0f)});
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(first + index);
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    // --debug-view's names, in the order of rasm::DebugView.
    rasm::DebugView parseDebugView(const char *name)
    {
        const char *names[] = {"none", "normals", "uvs", "overdraw", "lods", "culling", "light-count"};
        for (uint32_t i = 0; i < std::size(names); ++i)
        {
            if (std::strcmp(name, names[i]) == 0)
                return static_cast<rasm::DebugView>(i);
        }
        std::printf("Unknown debug view '%s'.\n", name);
        return rasm::DebugView::NONE;
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    bool printGpuTimes = false;
    bool ui = true;
    bool freezeCulling = false;
    for (int i = 1; i < argc; ++i)
    {
        printGpuTimes = printGpuTimes || std::strcmp(argv[i], "--gpu-times") == 0;
        ui = ui && std::strcmp(argv[i], "--no-ui") != 0;
        freezeCulling = freezeCulling || std::strcmp(argv[i], "--freeze-culling") == 0;
    }

    rasm::EngineConfig config = {
        .appName = "example_15",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .showStats = true,
        .ui = ui};

    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    // --debug-view <name> shows the scene's debug view, as the RASM window's View does.
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--debug-view") == 0)
            engine.setDebugView(parseDebugView(argv[i + 1]));
    }

    rasm::SceneHandle scene = engine.createScene();
    const rasm::EnvironmentHandle sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    const rasm::MeshHandle helmet = engine.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
    const rasm::MeshHandle cube = createCube(engine);
    // The same image the helmet's material samples, loaded once more to be shown in the UI.
    const rasm::TextureHandle albedo = engine.loadTexture("models/gltf/DamagedHelmet/Default_albedo.jpg");
    if (!sunset.isValid() || !helmet.isValid() || !cube.isValid() || !albedo.isValid())
        return 1;
    rasm::SceneEnvironment environment{.environment = sunset, .intensity = 1.0f, .rotation = 1.15f};
    engine.setSceneEnvironment(scene, environment);

    const rasm::MaterialHandle ground = engine.createMaterial(rasm::MaterialType::PBR);
    rasm::Material groundValues = engine.getMaterial(ground);
    groundValues.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.9f;
    engine.updateMaterial(ground, groundValues);
    engine.addEntityToScene(scene, engine.createEntity("ground", cube, ground, rasm::Transform{glm::vec3(0.0f, -1.2f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(20.0f, 0.5f, 20.0f)}));
    const rasm::EntityHandle helmetEntity = engine.createEntity("helmet", helmet, {}, rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    engine.addEntityToScene(scene, helmetEntity);

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f}},
                                                    rasm::Transform{glm::vec3(1.2f, 0.4f, 4.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = 4.2f, .center = glm::vec3(0.0f), .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);

    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.8f, 0.6f), glm::vec3(0.0f), glm::normalize(glm::vec3(-0.6f, -0.7f, -0.4f)), 2.0f, 0.0f);
    engine.addLightToScene(scene, sun);
    if (!camera.isValid() || !sun.isValid())
        return 1;

    // The application's window. Its changes reach the frame they are made in: the callback runs before the scene is
    // uploaded.
    bool turn = true;
    float turnSpeed = 0.5f;
    if (ui)
        engine.setUiCallback(
            [&]()
            {
                const float scale = ImGui::GetStyle().FontScaleDpi;
                ImGui::SetNextWindowPos(ImVec2(static_cast<float>(engine.getMainWindow().width) - 330.0f * scale, 10.0f * scale), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(320.0f * scale, 0.0f), ImGuiCond_FirstUseEver);
                ImGui::Begin("Scene");

                rasm::Light light = engine.getLight(sun);
                bool lightChanged = ImGui::SliderFloat("Sun intensity", &light.intensity, 0.0f, 10.0f);
                lightChanged |= ImGui::ColorEdit3("Sun color", &light.color.x);
                if (lightChanged)
                    engine.updateLight(sun, light);

                float exposure = engine.getCameraExposure(camera);
                if (ImGui::SliderFloat("Exposure", &exposure, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
                    engine.setCameraExposure(camera, exposure);
                if (ImGui::SliderFloat("Sky light", &environment.intensity, 0.0f, 3.0f))
                    engine.setSceneEnvironment(scene, environment);

                ImGui::Checkbox("Turn the helmet", &turn);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##speed", &turnSpeed, 0.0f, 2.0f, "%.2f deg");

                // Any sampled texture with a bindless slot, through its ImTextureID.
                if (engine.isLoaded(albedo))
                {
                    ImGui::TextUnformatted("Default_albedo.jpg");
                    const float side = ImGui::GetContentRegionAvail().x;
                    ImGui::Image(ImTextureRef(engine.getUiTexture(albedo)), ImVec2(side, side));
                }
                ImGui::End();
            });

    std::vector<std::string> timerNames;
    std::vector<double> timerSums;

    if (args.waitForLoads)
        engine.waitForLoads();

    for (uint32_t frame = 0; exampleRunning(engine, args); ++frame)
    {
        // Culling freezes after a few frames, once it has seen the scene.
        if (freezeCulling && frame == 30)
            engine.setCullingFrozen(true);
        // A fixed angle per frame, so frame N looks the same in every run.
        if (turn)
            engine.updateTransform(helmetEntity, engine.getTransform(helmetEntity).rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(turnSpeed)));
        engine.render(scene, camera);
        if (!printGpuTimes || frame < 300)
            continue;
        const rasm::FrameStats &stats = engine.getFrameStats();
        for (uint32_t i = 0; i < stats.gpuTimerCount; ++i)
        {
            size_t t = 0;
            while (t < timerNames.size() && timerNames[t] != stats.gpuTimers[i].name)
                ++t;
            if (t == timerNames.size())
            {
                timerNames.push_back(stats.gpuTimers[i].name);
                timerSums.push_back(0.0);
            }
            timerSums[t] += stats.gpuTimers[i].milliseconds;
        }
        if (frame == 600)
        {
            for (size_t t = 0; t < timerNames.size(); ++t)
                std::printf("%-20s %.3f ms\n", timerNames[t].c_str(), timerSums[t] / 301.0);
            break;
        }
    }
    return 0;
}
