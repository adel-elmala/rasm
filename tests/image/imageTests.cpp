#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "allocationCounter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

// Golden-image tests: each test renders a scene headless at 512 x 288, reads the frame back and compares it with
// tests/golden/lavapipe/<name>.png. A pixel differs when one of its channels differs by more than the test's threshold;
// the test fails when more than its fraction of the pixels differ. A failed test writes actual.png, expected.png and
// diff.png to <out-dir>/<name>/. The goldens are lavapipe's (Mesa's CPU driver), so tools/image_tests.py runs this
// on lavapipe; on another driver every test is expected to differ.
//   --golden-dir <dir>   the goldens (default tests/golden/lavapipe)
//   --out-dir <dir>      where failures go (default image-tests in the build folder)
//   --scenes a,b         only these tests
//   --update             write the rendered images as the new goldens instead of comparing
//   --sponza <file>      Sponza.gltf, which tools/image_tests.py fetches (it is not in the repository)
//   --list               print the tests and quit
// After its frame is read, a test renders STEADY_FRAMES more and fails if the main thread allocates on the heap in them
// (Windows only, allocationCounter.cpp).

namespace
{
    constexpr uint32_t WIDTH = 512;
    constexpr uint32_t HEIGHT = 288;
    constexpr float ENVIRONMENT_ROTATION = 1.15f;
    constexpr uint32_t GRID_LIGHTS = 32; // per side: 1024 point lights
    constexpr uint32_t STRESS_TARGETS = 8;
    // After the frame is read: frames that let the readback settle, then frames that must not allocate (P27).
    constexpr uint32_t SETTLE_FRAMES = 2;
    constexpr uint32_t STEADY_FRAMES = 4;
    constexpr glm::dvec3 FAR_ORIGIN(4197000.0, 168000.0, 4786000.0);

    struct Options
    {
        std::string goldenDir = RASM_IMAGE_TESTS_GOLDEN_DIR;
        std::string outDir = RASM_IMAGE_TESTS_OUT_DIR;
        std::string sponza = RASM_IMAGE_TESTS_SPONZA;
        std::vector<std::string> scenes;
        bool update = false;
        bool list = false;
    };

    // A tile of the tile_streaming test: its record, and the box its content is.
    struct TileBox
    {
        rasm::TileHandle tile;
        rasm::Transform transform;
        glm::vec3 color = glm::vec3(1.0f);
    };

    // A pick check of a picking test: a point of the content (moved with it), and what a pick at the pixel it projects to
    // must answer. A plane through the point gives the exact position (where the pixel's ray meets it), else the hit
    // must be within some pixels of the point.
    struct PickProbe
    {
        const char *name = "";
        glm::dvec3 point = glm::dvec3(0.0);
        rasm::EntityHandle entity;
        uint32_t feature = 0;
        uint32_t flags = 0;                 // PICK_HIT_* the hit has
        glm::dvec3 plane = glm::dvec3(0.0); // the plane's normal; zero: no plane
        double pixels = 3.0;
        double metres = 0.0; // added to the tolerance without a plane
        uint32_t skip = 0;
        float minAlpha = 0.5f;
        uint32_t size = 1; // above 1: a size x size rect, which must hold such a hit anywhere
    };

    // What a test's scene holds besides its entities: the lights that cast shadows, and where the camera looks from.
    struct Content
    {
        rasm::SceneHandle scene;
        rasm::LightHandle sun;
        float sunDistance = 20.0f;
        rasm::LightHandle local[2];
        uint32_t localCount = 0;
        glm::vec3 eye = glm::vec3(0.0f, 0.0f, 5.0f);
        glm::vec3 center = glm::vec3(0.0f);
        float fovY = 40.0f;
        // A test of Engine::render(views, count): a side camera, a top-down map and a security camera, whose view
        // texture a screen in the scene shows.
        rasm::CameraHandle side;
        rasm::CameraHandle map;
        rasm::CameraHandle security;
        rasm::RenderTargetHandle viewTexture;
        rasm::WindowHandle window; // a second, offscreen window, which the security camera's view also draws
        // What moveContent moves: the entities and lights that addEntity, addModel, addGround and addLight made.
        std::vector<rasm::EntityHandle> entities;
        std::vector<rasm::LightHandle> lights;
        glm::dvec3 origin = glm::dvec3(0.0); // where the content's origin is in the world
        // A test of TileStreamer: each frame updates it with the cut, and gives the tiles it hands out their boxes.
        std::shared_ptr<rasm::TileStreamer> tiles;
        std::vector<TileBox> tileBoxes;
        std::vector<rasm::TileHandle> cut;
        std::vector<PickProbe> probes;
        rasm::Grid grid;          // the camera's, its origin moved with the content
        float orthoHeight = 0.0f; // above 0: an orthographic camera whose box is this high, from the eye
    };

    // What every scene may use, loaded once.
    struct Assets
    {
        rasm::EnvironmentHandle sunset;
        rasm::MeshHandle cube;
        std::string sponza;
    };

    using BuildContent = bool (*)(rasm::Engine &, const Assets &, Content &);

    // A terrain tile and the samples of its heights, which vectors drape on.
    struct DrapeTile
    {
        rasm::TerrainTileDesc desc;
        std::vector<uint16_t> samples;
    };

    struct ImageTest
    {
        const char *name;
        BuildContent build;   // tests with the same build share one scene
        uint32_t samples = 1; // MSAA
        bool taa = false;
        bool bloom = false;
        rasm::DebugView view = rasm::DebugView::NONE;
        uint32_t frames = 4;                 // rendered before the frame that is read
        double time = 0.0;                   // the animation clock of every frame, in seconds
        glm::dvec3 origin = glm::dvec3(0.0); // the scene, its lights and the camera moved here by a translation
        rasm::MeshletPath meshlets = rasm::MeshletPath::OFF;
        bool computePoints = true;    // small points by the compute rasterizer where the device has 64-bit atomics
        uint32_t threshold = 3;       // a channel differs when it is off by more than this (0-255)
        double fraction = 0.001;      // the test fails when more of the pixels than this differ
        bool views = false;           // drawn by Engine::render with the content's views instead of a graph
        bool secondWindow = false;    // a views test that reads the content's second window instead of the main one
        const char *sameAs = nullptr; // compare with this test's image of the same run instead of a golden
        bool picks = false;           // also checks the content's pick probes, and picks under the centre in the steady frames
    };

    struct Comparison
    {
        uint64_t differing = 0;
        uint32_t maxDiff = 0;
        bool sizeMatches = false;
    };

    struct BuiltContent
    {
        BuildContent build = nullptr;
        glm::dvec3 origin = glm::dvec3(0.0);
        Content content;
        bool ok = false;
    };

    struct Rendered
    {
        std::string name;
        rasm::ReadbackImage image;
    };
}

namespace
{
    rasm::Transform makeTransform(const glm::vec3 &position, float scale, float yaw = 0.0f)
    {
        return rasm::Transform{position, glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(scale)};
    }

    // A unit cube, one quad per face so each face has its own normal.
    rasm::MeshHandle createCube(rasm::Engine &engine)
    {
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const glm::vec3 &n : normals)
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
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec3 &color, float roughness, float metallic = 0.0f)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::PBR);
        rasm::Material values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = metallic;
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::EntityHandle addEntity(rasm::Engine &engine, Content &content, const char *name, rasm::MeshHandle mesh, rasm::MaterialHandle material, const rasm::Transform &transform)
    {
        const rasm::EntityHandle entity = engine.createEntity(name, mesh, material, transform);
        engine.addEntityToScene(content.scene, entity);
        content.entities.push_back(entity);
        return entity;
    }

    rasm::LightHandle addLight(rasm::Engine &engine, Content &content, rasm::LightHandle light)
    {
        engine.addLightToScene(content.scene, light);
        content.lights.push_back(light);
        return light;
    }

    void addGround(rasm::Engine &engine, const Assets &assets, Content &content, float halfSize, const glm::vec3 &color)
    {
        addEntity(engine, content, "ground", assets.cube, createColorMaterial(engine, color, 0.85f),
                  rasm::Transform{glm::vec3(0.0f, -0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(2.0f * halfSize, 0.1f, 2.0f * halfSize)});
    }

    rasm::EntityHandle addModel(rasm::Engine &engine, Content &content, const char *name, rasm::MeshHandle mesh, const rasm::Transform &transform)
    {
        return addEntity(engine, content, name, mesh, {}, transform);
    }

    // The content moved by a translation, far from the world's origin. Each view draws in its camera's render space, so
    // it looks the same as at the origin.
    void moveContent(rasm::Engine &engine, Content &content, const glm::dvec3 &origin)
    {
        content.origin = origin;
        for (rasm::EntityHandle entity : content.entities)
        {
            rasm::Transform transform = engine.getTransform(entity);
            transform.position += origin;
            engine.updateTransform(entity, transform);
        }
        for (rasm::LightHandle light : content.lights)
        {
            rasm::Light values = engine.getLight(light);
            values.position += origin;
            engine.updateLight(light, values);
        }
    }

    // The venice sunset photo around the scene, and a sun higher than its own, so the shadows are short enough to see.
    bool addSunsetLight(rasm::Engine &engine, const Assets &assets, Content &content, float intensity, const glm::vec3 &sunDirection, float sunIntensity)
    {
        engine.setSceneEnvironment(content.scene, rasm::SceneEnvironment{.environment = assets.sunset, .intensity = intensity, .rotation = ENVIRONMENT_ROTATION});
        content.sun = addLight(engine, content, engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.85f, 0.7f), glm::vec3(0.0f), glm::normalize(sunDirection), sunIntensity, 0.0f));
        return content.sun.isValid();
    }

    glm::vec3 getHue(uint32_t i, uint32_t count)
    {
        const float hue = static_cast<float>(i) / static_cast<float>(count);
        return glm::clamp(glm::abs(glm::mod(hue * 6.0f + glm::vec3(0.0f, 4.0f, 2.0f), 6.0f) - 3.0f) - 1.0f, 0.0f, 1.0f);
    }

    bool buildHelmet(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle helmet = engine.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
        addGround(engine, assets, content, 30.0f, glm::vec3(0.5f));
        addModel(engine, content, "helmet", helmet, rasm::Transform{glm::vec3(0.0f, 1.0f, 0.0f), glm::angleAxis(glm::radians(-25.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
        content.eye = glm::vec3(0.0f, 1.4f, 3.6f);
        content.center = glm::vec3(0.0f, 1.0f, 0.0f);
        return helmet.isValid() && addSunsetLight(engine, assets, content, 1.0f, glm::vec3(0.6f, -1.0f, -0.35f), 3.0f);
    }

    bool buildSponza(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        if (!std::filesystem::exists(assets.sponza))
        {
            std::printf("  Sponza is not at %s: tools/image_tests.py fetches it.\n", assets.sponza.c_str());
            return false;
        }
        const rasm::MeshHandle sponza = engine.loadGLTF(assets.sponza);
        addModel(engine, content, "sponza", sponza, makeTransform(glm::vec3(0.0f), 1.0f));
        content.sunDistance = 40.0f;
        content.eye = glm::vec3(6.0f, 1.5f, -0.6f);
        content.center = glm::vec3(-6.0f, 3.4f, 1.6f);
        content.fovY = 60.0f;
        return sponza.isValid() && addSunsetLight(engine, assets, content, 1.0f, glm::vec3(0.25f, -1.0f, 0.3f), 6.0f);
    }

    bool buildAlphaModes(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle alphaModes = engine.loadGLTF("models/gltf/AlphaBlendModeTest/AlphaBlendModeTest.glb");
        addGround(engine, assets, content, 20.0f, glm::vec3(0.5f));
        addModel(engine, content, "alpha modes", alphaModes, makeTransform(glm::vec3(0.0f, 0.48f, 0.0f), 1.0f));
        content.eye = glm::vec3(0.0f, 2.2f, 7.0f);
        content.center = glm::vec3(0.0f, 1.1f, 0.0f);
        return alphaModes.isValid() && addSunsetLight(engine, assets, content, 1.0f, glm::vec3(0.45f, -0.75f, -0.5f), 3.0f);
    }

    bool buildEmissive(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle emissive = engine.loadGLTF("models/gltf/EmissiveStrengthTest/EmissiveStrengthTest.glb");
        addGround(engine, assets, content, 20.0f, glm::vec3(0.3f));
        addModel(engine, content, "emissive strength", emissive, makeTransform(glm::vec3(0.0f, 1.2f, 0.0f), 0.3f));
        content.eye = glm::vec3(0.0f, 1.6f, 6.0f);
        content.center = glm::vec3(0.0f, 1.2f, 0.0f);
        return emissive.isValid() && addSunsetLight(engine, assets, content, 0.15f, glm::vec3(0.45f, -0.75f, -0.5f), 0.3f);
    }

    // CesiumMan and two foxes, each held at its clip's pose at the test's time.
    bool buildSkinning(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle man = engine.loadGLTF("models/gltf/CesiumMan/CesiumMan.glb");
        const rasm::MeshHandle fox = engine.loadGLTF("models/gltf/Fox/Fox.glb");
        // The clips' names are known once the meshes are loaded.
        engine.waitForLoads();
        const int32_t walk = engine.findAnimation(fox, "Walk");
        const int32_t run = engine.findAnimation(fox, "Run");
        if (!engine.isLoaded(man) || !engine.isLoaded(fox) || walk < 0 || run < 0)
            return false;
        addGround(engine, assets, content, 20.0f, glm::vec3(0.55f));
        engine.playAnimation(addModel(engine, content, "CesiumMan", man, makeTransform(glm::vec3(-0.9f, 0.0f, 0.3f), 1.0f, 0.5f)), rasm::AnimationPlay{.clip = 0});
        engine.playAnimation(addModel(engine, content, "Fox walk", fox, makeTransform(glm::vec3(0.4f, 0.0f, 0.6f), 0.012f, 0.9f)), rasm::AnimationPlay{.clip = static_cast<uint32_t>(walk)});
        engine.playAnimation(addModel(engine, content, "Fox run", fox, makeTransform(glm::vec3(0.9f, 0.0f, -0.8f), 0.012f, -0.6f)), rasm::AnimationPlay{.clip = static_cast<uint32_t>(run)});
        content.eye = glm::vec3(0.0f, 1.3f, 4.2f);
        content.center = glm::vec3(0.0f, 0.6f, 0.0f);
        return addSunsetLight(engine, assets, content, 0.6f, glm::vec3(0.4f, -0.8f, -0.45f), 3.0f);
    }

    // MorphStressTest with fixed weights, set by the application: no clip plays.
    bool buildMorph(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle stress = engine.loadGLTF("models/gltf/MorphStressTest/MorphStressTest.glb");
        engine.waitForLoads();
        if (!engine.isLoaded(stress) || engine.getMorphTargetNames(stress).size() != STRESS_TARGETS)
            return false;
        addGround(engine, assets, content, 20.0f, glm::vec3(0.55f));
        const rasm::EntityHandle entity = addModel(engine, content, "MorphStressTest", stress, makeTransform(glm::vec3(0.0f, 0.1f, 0.0f), 1.0f));
        const float weights[STRESS_TARGETS] = {1.0f, 0.5f, 0.0f, 0.75f, 0.25f, 1.0f, 0.0f, 0.6f};
        engine.setMorphWeights(entity, weights, STRESS_TARGETS);
        content.eye = glm::vec3(0.0f, 2.2f, 3.4f);
        content.center = glm::vec3(0.0f, 0.4f, 0.0f);
        return addSunsetLight(engine, assets, content, 0.8f, glm::vec3(0.4f, -0.8f, -0.45f), 3.0f);
    }

    bool buildNodeAnimation(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle cube = engine.loadGLTF("models/gltf/AnimatedCube/AnimatedCube.gltf");
        addGround(engine, assets, content, 20.0f, glm::vec3(0.55f));
        engine.playAnimation(addModel(engine, content, "AnimatedCube", cube, makeTransform(glm::vec3(0.0f, 1.2f, 0.0f), 0.8f)), rasm::AnimationPlay{.clip = rasm::ALL_ANIMATION_CLIPS});
        content.eye = glm::vec3(0.0f, 2.4f, 4.6f);
        content.center = glm::vec3(0.0f, 1.0f, 0.0f);
        return cube.isValid() && addSunsetLight(engine, assets, content, 0.8f, glm::vec3(0.4f, -0.8f, -0.45f), 3.0f);
    }

    // A grid of colored point lights over a floor with a few boxes, under a dim moon and no sky.
    bool buildManyLights(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle cube = assets.cube;
        addGround(engine, assets, content, 20.0f, glm::vec3(0.6f));
        const rasm::MaterialHandle stone = createColorMaterial(engine, glm::vec3(0.7f), 0.6f);
        for (int x = -2; x <= 2; ++x)
            for (int z = -2; z <= 2; ++z)
                engine.addEntityToScene(content.scene, engine.createEntity("box", cube, stone, makeTransform(glm::vec3(3.2f * static_cast<float>(x), 0.5f, 3.2f * static_cast<float>(z)), 1.0f, 0.3f * static_cast<float>(x + z))));
        const float spacing = 0.5f;
        for (uint32_t i = 0; i < GRID_LIGHTS * GRID_LIGHTS; ++i)
        {
            const glm::vec3 position((static_cast<float>(i % GRID_LIGHTS) - 0.5f * (GRID_LIGHTS - 1)) * spacing, 0.35f, (static_cast<float>(i / GRID_LIGHTS) - 0.5f * (GRID_LIGHTS - 1)) * spacing);
            const rasm::LightHandle light = engine.createLight(rasm::LightType::POINT, getHue(i % 7 + i / GRID_LIGHTS % 5, 12), position, glm::vec3(0.0f), 0.6f, 1.2f);
            engine.addLightToScene(content.scene, light);
        }
        content.sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(0.5f, 0.6f, 1.0f), glm::vec3(0.0f), glm::normalize(glm::vec3(0.3f, -1.0f, -0.5f)), 0.15f, 0.0f);
        engine.addLightToScene(content.scene, content.sun);
        content.eye = glm::vec3(0.0f, 9.0f, 12.0f);
        content.center = glm::vec3(0.0f, 0.0f, 0.5f);
        content.fovY = 50.0f;
        return cube.isValid();
    }

    // A spot light and a point light with shadow maps (a cube for the point light) on pillars and boxes, at night.
    bool buildLocalShadows(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle cube = assets.cube;
        addGround(engine, assets, content, 20.0f, glm::vec3(0.6f));
        const rasm::MaterialHandle stone = createColorMaterial(engine, glm::vec3(0.75f, 0.7f, 0.65f), 0.7f);
        for (int i = 0; i < 4; ++i)
        {
            const float angle = glm::radians(45.0f + 90.0f * static_cast<float>(i));
            addEntity(engine, content, "pillar", cube, stone, rasm::Transform{glm::vec3(2.0f * std::cos(angle), 1.0f, 2.0f * std::sin(angle)), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.4f, 2.0f, 0.4f)});
        }
        addEntity(engine, content, "box", cube, stone, makeTransform(glm::vec3(-4.0f, 0.5f, 1.0f), 1.0f, 0.4f));
        addEntity(engine, content, "box", cube, stone, makeTransform(glm::vec3(4.2f, 0.6f, -0.5f), 1.2f, -0.3f));
        const glm::vec3 lampPosition(-5.0f, 6.0f, 4.0f);
        content.local[0] = engine.createLight(rasm::LightType::SPOT, glm::vec3(1.0f, 0.95f, 0.8f), lampPosition, glm::normalize(glm::vec3(-1.0f, 0.0f, 0.0f) - lampPosition), 80.0f, 30.0f,
                                              glm::radians(16.0f), glm::radians(26.0f));
        content.local[1] = engine.createLight(rasm::LightType::POINT, glm::vec3(1.0f, 0.6f, 0.3f), glm::vec3(0.0f, 1.2f, 0.0f), glm::vec3(0.0f), 40.0f, 10.0f);
        content.localCount = 2;
        addLight(engine, content, content.local[0]);
        addLight(engine, content, content.local[1]);
        content.eye = glm::vec3(1.5f, 5.5f, 9.0f);
        content.center = glm::vec3(0.0f, 0.5f, 0.0f);
        content.fovY = 50.0f;
        return cube.isValid() && content.local[0].isValid() && content.local[1].isValid();
    }

    // FlightHelmet's textures are KTX2 (Basis), transcoded to the best block format the device samples.
    bool buildKtx2(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle helmet = engine.loadGLTF("models/gltf/FlightHelmet/FlightHelmet.gltf");
        addGround(engine, assets, content, 20.0f, glm::vec3(0.5f));
        addModel(engine, content, "FlightHelmet", helmet, makeTransform(glm::vec3(0.0f), 2.0f, glm::radians(20.0f)));
        content.eye = glm::vec3(0.0f, 1.0f, 2.4f);
        content.center = glm::vec3(0.0f, 0.65f, 0.0f);
        return helmet.isValid() && addSunsetLight(engine, assets, content, 1.0f, glm::vec3(0.45f, -0.75f, -0.5f), 3.0f);
    }

    bool buildMaterials(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle transforms = engine.loadGLTF("models/gltf/TextureTransformTest/TextureTransformTest.gltf");
        const rasm::MeshHandle colors = engine.loadGLTF("models/gltf/VertexColorTest/VertexColorTest.glb");
        const rasm::MeshHandle lantern = engine.loadGLTF("models/gltf/LanternQuantized/Lantern.gltf");
        addGround(engine, assets, content, 20.0f, glm::vec3(0.5f));
        addModel(engine, content, "texture transforms", transforms, makeTransform(glm::vec3(-2.6f, 1.4f, 0.0f), 1.0f));
        addModel(engine, content, "vertex colors", colors, makeTransform(glm::vec3(0.8f, 1.0f, 0.0f), 0.9f));
        addModel(engine, content, "lantern", lantern, makeTransform(glm::vec3(3.4f, 0.0f, -0.5f), 0.12f, -0.5f));
        content.eye = glm::vec3(0.3f, 2.6f, 9.0f);
        content.center = glm::vec3(0.3f, 1.6f, 0.0f);
        return transforms.isValid() && colors.isValid() && lantern.isValid() && addSunsetLight(engine, assets, content, 1.0f, glm::vec3(0.45f, -0.75f, -0.5f), 3.0f);
    }

    // Split screen, a map over it and a view texture on a screen in the scene (Engine::render): the helmet from the
    // test's camera on the left and from the side on the right, the map in the top-right corner, and a security camera
    // drawn into a view texture first, which the screen behind the helmet glows with.
    bool buildViews(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle helmet = engine.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
        addGround(engine, assets, content, 8.0f, glm::vec3(0.5f));
        addModel(engine, content, "helmet", helmet, makeTransform(glm::vec3(0.0f, 1.0f, 0.0f), 1.0f));
        for (uint32_t i = 0; i < 4; ++i)
        {
            const float angle = glm::radians(45.0f + 90.0f * static_cast<float>(i));
            engine.addEntityToScene(content.scene, engine.createEntity("box", assets.cube, createColorMaterial(engine, getHue(i, 4), 0.5f),
                                                                       makeTransform(glm::vec3(4.0f * std::cos(angle), 0.6f, 4.0f * std::sin(angle)), 1.2f)));
        }
        content.viewTexture = engine.createRenderTarget({.name = "screen", .width = 192, .height = 108, .colorFormats = {rasm::Format::R8G8B8A8_SRGB}});
        if (!content.viewTexture.isValid())
            return false;
        const rasm::MaterialHandle screen = createColorMaterial(engine, glm::vec3(0.02f), 0.3f);
        rasm::Material values = engine.getMaterial(screen);
        values.textures[static_cast<uint32_t>(rasm::PbrSlot::EMISSIVE)] = engine.getRenderTargetTexture(content.viewTexture);
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::EMISSIVE_FACTOR) + i] = 1.0f;
        values.params[static_cast<uint32_t>(rasm::PbrParam::EMISSIVE_STRENGTH)] = 1.0f;
        engine.updateMaterial(screen, values);
        engine.addEntityToScene(content.scene, engine.createEntity("screen", assets.cube, screen,
                                                                   rasm::Transform{glm::vec3(0.0f, 1.8f, -2.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(3.2f, 1.8f, 0.05f)}));
        content.eye = glm::vec3(0.0f, 2.0f, 6.0f);
        content.center = glm::vec3(0.0f, 1.2f, 0.0f);

        const rasm::CameraProjection perspective = {.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 45.0f, .aspect = 16.0f / 9.0f, .nearZ = 0.1f}};
        const glm::vec3 target(0.0f, 1.0f, 0.0f);
        const glm::vec3 sideEye(6.0f, 2.5f, 1.5f);
        const glm::vec3 securityEye(4.5f, 3.0f, 4.5f);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        content.side = engine.createCamera(perspective, rasm::Transform{sideEye, glm::quatLookAt(glm::normalize(target - sideEye), up), glm::vec3(1.0f)});
        content.security = engine.createCamera(perspective, rasm::Transform{securityEye, glm::quatLookAt(glm::normalize(target - securityEye), up), glm::vec3(1.0f)});
        content.map = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                                                                 .orthographic{.left = -8.0f, .right = 8.0f, .bottom = -8.0f, .top = 8.0f, .nearZ = 0.1f, .farZ = 40.0f}},
                                          rasm::Transform{glm::vec3(0.0f, 20.0f, 0.0f), glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(1.0f)});
        for (const rasm::CameraHandle camera : {content.side, content.security, content.map})
        {
            engine.setCameraController(camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
            engine.addCameraToScene(content.scene, camera);
        }
        content.window = engine.addWindow({.width = 256, .height = 144});
        return helmet.isValid() && content.side.isValid() && content.security.isValid() && content.map.isValid() && content.window.isValid() &&
               addSunsetLight(engine, assets, content, 1.0f, glm::vec3(0.6f, -1.0f, -0.35f), 3.0f);
    }

    // A quadtree of boxes drawn by a TileStreamer: the root, its four children, and the first child's four children. The
    // cut wants the grandchildren, two children and a building on the fourth child, whose refinement is ADD, so it stays
    // drawn under the building. Drawn: the four grandchildren, three children and the building; not the root, nor the
    // first child, which its children replace. The boxes are the shared cube with a material of their own each.
    bool buildTiles(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        content.tiles = std::make_shared<rasm::TileStreamer>(engine, content.scene, rasm::TileStreamerConfig{.maxLoadsPerFrame = 64, .maxLoadsInFlight = 64});
        auto add = [&](rasm::TileHandle parent, const glm::vec3 &center, float size, float height, uint32_t level, rasm::TileRefine refine)
        {
            const rasm::TileHandle tile = content.tiles->createTile({.parent = parent,
                                                                     .center = glm::dvec3(center),
                                                                     .radius = 0.75 * size + height,
                                                                     .geometricError = level < 2 ? 0.5 * size : 0.0,
                                                                     .refine = refine});
            content.tileBoxes.push_back({.tile = tile, .transform = rasm::Transform{center, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.94f * size, height, 0.94f * size)}, .color = getHue(level, 3)});
            return tile;
        };
        const rasm::TileHandle root = add({}, glm::vec3(0.0f, -0.1f, 0.0f), 8.0f, 0.2f, 0, rasm::TileRefine::REPLACE);
        rasm::TileHandle children[4];
        for (int32_t i = 0; i < 4; ++i)
            children[i] = add(root, glm::vec3(i % 2 == 0 ? -2.0f : 2.0f, -0.1f, i < 2 ? -2.0f : 2.0f), 4.0f, 0.2f, 1, i == 3 ? rasm::TileRefine::ADD : rasm::TileRefine::REPLACE);
        for (int32_t i = 0; i < 4; ++i)
            content.cut.push_back(add(children[0], glm::vec3(i % 2 == 0 ? -3.0f : -1.0f, -0.1f, i < 2 ? -3.0f : -1.0f), 2.0f, 0.2f, 2, rasm::TileRefine::REPLACE));
        content.cut.push_back(children[1]);
        content.cut.push_back(children[2]);
        content.cut.push_back(add(children[3], glm::vec3(2.0f, 1.0f, 2.0f), 1.0f, 2.0f, 2, rasm::TileRefine::REPLACE));
        content.eye = glm::vec3(0.0f, 9.0f, 10.0f);
        content.center = glm::vec3(0.0f, 0.0f, 0.5f);
        content.fovY = 45.0f;
        return assets.cube.isValid() && addSunsetLight(engine, assets, content, 1.0f, glm::vec3(0.45f, -0.75f, -0.5f), 3.0f);
    }

    // A terrain tile of cells x cells over [x0, x0 + size] x [z0, z0 + size]: heights on the world's grid of 1/64 m, so
    // neighbours share bit-equal edges and border samples, and an image of rings whose colour is the tile's level. It
    // returns the tile and its samples, to drape vectors on.
    DrapeTile addTerrainTile(rasm::Engine &engine, Content &content, double x0, double z0, double size, uint32_t cells, const glm::vec3 &tint)
    {
        constexpr double STEP = 1.0 / 64.0;
        const uint32_t width = cells + 3;
        const double cell = size / cells;
        std::vector<int64_t> steps(static_cast<size_t>(width) * width);
        int64_t lowest = INT64_MAX;
        int64_t highest = INT64_MIN;
        for (uint32_t j = 0; j < width; ++j)
        {
            for (uint32_t i = 0; i < width; ++i)
            {
                const double x = x0 + (static_cast<double>(i) - 1.0) * cell;
                const double z = z0 + (static_cast<double>(j) - 1.0) * cell;
                const double height = 1.6 * std::sin(x * 0.45) * std::cos(z * 0.35) + 0.5 * std::sin(x * 1.3 + z * 0.9);
                steps[static_cast<size_t>(j) * width + i] = std::llround(height / STEP);
                lowest = std::min(lowest, steps[static_cast<size_t>(j) * width + i]);
                highest = std::max(highest, steps[static_cast<size_t>(j) * width + i]);
            }
        }
        std::vector<uint16_t> samples(steps.size());
        for (size_t k = 0; k < steps.size(); ++k)
            samples[k] = static_cast<uint16_t>(steps[k] - lowest);
        const rasm::TextureHandle heights = engine.createTexture({.name = "terrain heights", .width = width, .height = width, .format = rasm::Format::R16_UNORM, .usage = rasm::TextureUsage::SAMPLED});
        engine.getRenderContext().fillTexture(heights, samples.data());
        // Rings around the world's origin, so lines that run across tiles show a seam if there is one.
        constexpr uint32_t IMAGE = 64;
        std::vector<uint8_t> pixels(IMAGE * IMAGE * 4);
        for (uint32_t j = 0; j < IMAGE; ++j)
        {
            for (uint32_t i = 0; i < IMAGE; ++i)
            {
                const double x = x0 + size * (i + 0.5) / IMAGE;
                const double z = z0 + size * (j + 0.5) / IMAGE;
                const float ring = 0.65f + 0.35f * static_cast<float>(std::cos(std::sqrt(x * x + z * z) * 2.0));
                const glm::vec3 color = tint * ring;
                for (uint32_t c = 0; c < 3; ++c)
                    pixels[(j * IMAGE + i) * 4 + c] = static_cast<uint8_t>(255.0f * glm::clamp(color[c], 0.0f, 1.0f));
                pixels[(j * IMAGE + i) * 4 + 3] = 255;
            }
        }
        const rasm::TextureHandle imagery = engine.createTexture({.name = "terrain imagery", .width = IMAGE, .height = IMAGE, .format = rasm::Format::R8G8B8A8_SRGB, .usage = rasm::TextureUsage::SAMPLED});
        engine.getRenderContext().fillTexture(imagery, pixels.data());
        DrapeTile drape = {.desc = {.center = glm::dvec3(x0 + 0.5 * size, 0.0, z0 + 0.5 * size),
                                    .size = size,
                                    .terrain = {.heights = heights,
                                                .heightScale = static_cast<float>(STEP),
                                                .heightOffset = static_cast<float>(static_cast<double>(lowest) * STEP),
                                                .minHeight = static_cast<float>(static_cast<double>(lowest) * STEP),
                                                .maxHeight = static_cast<float>(static_cast<double>(highest) * STEP),
                                                .skirt = static_cast<float>(0.1 * size)},
                                    .imagery = imagery},
                           .samples = std::move(samples)};
        const rasm::TerrainTile tile = engine.createTerrainTile(drape.desc);
        engine.addEntityToScene(content.scene, tile.entity);
        content.entities.push_back(tile.entity);
        return drape;
    }

    // Terrain of two levels side by side: three tiles of 8 m with 16 cells, and four of 4 m in the fourth quarter, so the
    // levels meet along two edges (T-junctions the skirts hide). The sun's shadows fall across the tiles.
    bool buildTerrain(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        for (int32_t i = 0; i < 3; ++i)
            addTerrainTile(engine, content, i % 2 == 0 ? -8.0 : 0.0, i < 2 ? -8.0 : 0.0, 8.0, 16, glm::vec3(0.55f, 0.75f, 0.45f));
        for (int32_t i = 0; i < 4; ++i)
            addTerrainTile(engine, content, i % 2 == 0 ? 0.0 : 4.0, i < 2 ? 0.0 : 4.0, 4.0, 16, glm::vec3(0.75f, 0.65f, 0.45f));
        content.eye = glm::vec3(-6.0f, 7.0f, 11.0f);
        content.center = glm::vec3(1.0f, 0.0f, 0.0f);
        content.fovY = 50.0f;
        return addSunsetLight(engine, assets, content, 0.6f, glm::vec3(0.7f, -0.3f, -0.3f), 3.0f);
    }

    rasm::MaterialHandle createVectorMaterial(rasm::Engine &engine, const rasm::VectorMaterial &style)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::VECTOR);
        rasm::Material values = engine.getMaterial(material);
        values.vector = style;
        engine.updateMaterial(material, values);
        return material;
    }

    void addVectorEntity(rasm::Engine &engine, Content &content, rasm::VectorGeometry geometry, rasm::MaterialHandle material)
    {
        addEntity(engine, content, "vectors", engine.createMesh(rasm::buildVectorMesh(std::move(geometry))), material, rasm::Transform{});
    }

    // Vector features on a flat ground: three zigzags with round, miter and bevel joins and round, square and butt caps,
    // outlined; a dashed line, a line 0.3 px wide (it keeps one pixel and fades), a line in metres on the ground's plane, a
    // see-through polygon with a hole under the lines (its draw order is lower), points with a ring, a box with a line
    // behind it (hidden) and an overlay line (over it), and a short line with 120 px round caps just past the left edge,
    // whose anchors are off screen: culling's pad must keep it.
    bool buildVectors(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        addGround(engine, assets, content, 8.0f, glm::vec3(0.75f, 0.74f, 0.7f));
        const glm::dvec3 origin(0.0);
        const rasm::MaterialHandle zigzag = createVectorMaterial(engine, {.color = glm::vec4(1.0f, 0.6f, 0.15f, 1.0f), .width = 14.0f, .outlineColor = glm::vec4(0.3f, 0.15f, 0.05f, 1.0f), .outlineWidth = 3.0f});
        rasm::VectorGeometry zigzags;
        const rasm::LineJoin joins[3] = {rasm::LineJoin::ROUND, rasm::LineJoin::MITER, rasm::LineJoin::BEVEL};
        const rasm::LineCap caps[3] = {rasm::LineCap::ROUND, rasm::LineCap::SQUARE, rasm::LineCap::BUTT};
        for (uint32_t i = 0; i < 3; ++i)
        {
            const double z = -2.2 + 1.1 * i;
            const glm::dvec3 points[4] = {{-3.6, 0.0, z}, {-2.8, 0.0, z + 0.7}, {-2.0, 0.0, z}, {-1.0, 0.0, z + 0.5}};
            const uint32_t size = 4;
            rasm::addVectorLines(zigzags, origin, points, &size, 1, {.join = joins[i], .cap = caps[i]});
        }
        addVectorEntity(engine, content, std::move(zigzags), zigzag);

        rasm::VectorGeometry dashed;
        std::vector<glm::dvec3> wave;
        for (int32_t k = 0; k <= 40; ++k)
            wave.push_back({0.2 + 0.09 * k, 0.0, -2.0 + 0.25 * std::sin(0.4 * k)});
        const auto waveSize = static_cast<uint32_t>(wave.size());
        rasm::addVectorLines(dashed, origin, wave.data(), &waveSize, 1);
        addVectorEntity(engine, content, std::move(dashed), createVectorMaterial(engine, {.color = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f), .width = 4.0f, .dashLength = 12.0f, .gapLength = 8.0f}));

        rasm::VectorGeometry thin;
        const glm::dvec3 thinPoints[2] = {{0.2, 0.0, -1.2}, {3.8, 0.0, -0.9}};
        const uint32_t two = 2;
        rasm::addVectorLines(thin, origin, thinPoints, &two, 1);
        addVectorEntity(engine, content, std::move(thin), createVectorMaterial(engine, {.color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), .width = 0.3f}));

        rasm::VectorGeometry metres;
        const glm::dvec3 metrePoints[3] = {{0.2, 0.0, -0.4}, {2.0, 0.0, 0.1}, {3.8, 0.0, -0.4}};
        const uint32_t three = 3;
        rasm::addVectorLines(metres, origin, metrePoints, &three, 1, {.join = rasm::LineJoin::MITER});
        addVectorEntity(engine, content, std::move(metres),
                        createVectorMaterial(engine, {.color = glm::vec4(0.2f, 0.5f, 0.2f, 1.0f), .width = 0.2f, .widthUnit = rasm::VectorUnit::METRES, .facing = rasm::VectorFacing::GROUND}));

        rasm::VectorGeometry polygon;
        const glm::dvec3 rings[8] = {{0.4, 0.0, -2.6}, {2.6, 0.0, -2.6}, {2.6, 0.0, -0.8}, {0.4, 0.0, -0.8}, {1.2, 0.0, -2.0}, {1.8, 0.0, -2.0}, {1.8, 0.0, -1.4}, {1.2, 0.0, -1.4}};
        const uint32_t ringSizes[2] = {4, 4};
        const uint32_t ringCount = 2;
        rasm::addVectorPolygons(polygon, origin, rings, ringSizes, &ringCount, 1);
        addVectorEntity(engine, content, std::move(polygon), createVectorMaterial(engine, {.color = glm::vec4(0.2f, 0.4f, 0.9f, 0.6f), .drawOrder = -5}));

        rasm::VectorGeometry points;
        glm::dvec3 places[5];
        for (int32_t k = 0; k < 5; ++k)
            places[k] = {-3.2 + 1.6 * k, 0.0, 1.6};
        rasm::addVectorPoints(points, origin, places, 5);
        addVectorEntity(engine, content, std::move(points),
                        createVectorMaterial(engine, {.color = glm::vec4(0.85f, 0.1f, 0.2f, 1.0f), .width = 16.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 3.0f, .drawOrder = 5}));

        addEntity(engine, content, "box", assets.cube, createColorMaterial(engine, glm::vec3(0.5f, 0.5f, 0.55f), 0.6f),
                  rasm::Transform{glm::vec3(1.2f, 0.35f, 0.9f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.7f)});
        rasm::VectorGeometry behind;
        const glm::dvec3 behindPoints[2] = {{0.2, 0.0, 0.5}, {2.4, 0.0, 0.5}};
        rasm::addVectorLines(behind, origin, behindPoints, &two, 1);
        addVectorEntity(engine, content, std::move(behind), createVectorMaterial(engine, {.color = glm::vec4(0.1f, 0.6f, 0.6f, 1.0f), .width = 6.0f}));
        rasm::VectorGeometry over;
        const glm::dvec3 overPoints[2] = {{0.2, 0.0, 0.2}, {2.4, 0.0, 0.2}};
        rasm::addVectorLines(over, origin, overPoints, &two, 1);
        addVectorEntity(engine, content, std::move(over), createVectorMaterial(engine, {.color = glm::vec4(0.7f, 0.1f, 0.8f, 1.0f), .width = 6.0f, .drawOrder = 20, .depth = rasm::VectorDepth::OVERLAY}));

        rasm::VectorGeometry edge;
        const glm::dvec3 edgePoints[2] = {{-5.55, 0.0, 0.0}, {-5.55, 0.0, 0.05}};
        rasm::addVectorLines(edge, origin, edgePoints, &two, 1, {.cap = rasm::LineCap::ROUND});
        addVectorEntity(engine, content, std::move(edge), createVectorMaterial(engine, {.color = glm::vec4(0.1f, 0.3f, 0.9f, 1.0f), .width = 120.0f}));

        content.eye = glm::vec3(0.0f, 4.0f, 5.5f);
        content.center = glm::vec3(0.0f, 0.0f, -0.3f);
        content.fovY = 45.0f;
        return assets.cube.isValid() && addSunsetLight(engine, assets, content, 0.8f, glm::vec3(0.4f, -0.8f, -0.3f), 2.5f);
    }

    // Labels from Roboto's distance atlas: places with a point each and their names beside them (UTF-8 past ASCII), two
    // names over each other where the higher priority wins, two lines, light text with a dark halo, names in metres on
    // the ground that shrink with distance, a name over a box and one behind it that the box hides by depth.
    bool buildLabels(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        addGround(engine, assets, content, 8.0f, glm::vec3(0.75f, 0.74f, 0.7f));
        rasm::FontAtlas atlas;
        if (!engine.loadFont("fonts/Roboto-Medium.ttf", atlas))
            return false;
        const rasm::FontHandle font = engine.createFont(atlas);
        const glm::dvec3 origin(0.0);
        const rasm::MaterialHandle text =
            createVectorMaterial(engine, {.color = glm::vec4(0.12f, 0.1f, 0.08f, 1.0f), .width = 14.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 2.0f, .drawOrder = 40, .font = font});

        rasm::VectorGeometry points;
        rasm::VectorGeometry names;
        const char *placeNames[4] = {"Caf\u00E9 Ost", "Stra\u00DFe 2", "\u00C5lesund", "AVA Tower"};
        rasm::LabelDesc labels[4];
        glm::dvec3 places[4];
        for (int32_t k = 0; k < 4; ++k)
        {
            places[k] = {-3.4 + 1.9 * k, 0.0, 1.5};
            labels[k] = {.position = places[k], .text = placeNames[k], .priority = 1.0f, .anchor = rasm::LabelAnchor::LEFT, .offset = glm::vec2(0.2f, 0.0f)};
        }
        rasm::addVectorPoints(points, origin, places, 4);
        rasm::addVectorLabels(names, origin, atlas, labels, 4);
        addVectorEntity(engine, content, std::move(points),
                        createVectorMaterial(engine, {.color = glm::vec4(0.85f, 0.1f, 0.2f, 1.0f), .width = 10.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 2.0f, .drawOrder = 5}));
        // Over each other: the higher priority is placed, the other hidden.
        const rasm::LabelDesc overlap[3] = {{.position = {-0.6, 0.0, -0.8}, .text = "Old Town", .priority = 1.0f},
                                            {.position = {-0.4, 0.0, -0.75}, .text = "Harbour", .priority = 5.0f},
                                            {.position = {-3.2, 0.0, -0.6}, .text = "Two lines\nof text", .priority = 2.0f}};
        rasm::addVectorLabels(names, origin, atlas, overlap, 3);
        addVectorEntity(engine, content, std::move(names), text);

        rasm::VectorGeometry night;
        const rasm::LabelDesc nightLabel = {.position = {2.6, 0.0, -0.4}, .text = "Night", .priority = 3.0f};
        rasm::addVectorLabels(night, origin, atlas, &nightLabel, 1);
        addVectorEntity(engine, content, std::move(night),
                        createVectorMaterial(engine, {.color = glm::vec4(1.0f, 0.95f, 0.6f, 1.0f), .width = 20.0f, .outlineColor = glm::vec4(0.05f, 0.05f, 0.15f, 1.0f), .outlineWidth = 3.0f, .drawOrder = 40, .font = font}));

        rasm::VectorGeometry metres;
        const rasm::LabelDesc metreLabels[2] = {{.position = {-1.5, 0.0, -3.5}, .text = "Far"}, {.position = {-1.2, 0.0, 0.5}, .text = "Near"}};
        rasm::addVectorLabels(metres, origin, atlas, metreLabels, 2);
        addVectorEntity(engine, content, std::move(metres),
                        createVectorMaterial(engine, {.color = glm::vec4(0.2f, 0.25f, 0.6f, 1.0f), .width = 0.3f, .widthUnit = rasm::VectorUnit::METRES, .outlineColor = glm::vec4(1.0f), .outlineWidth = 0.03f, .drawOrder = 30, .font = font}));

        addEntity(engine, content, "box", assets.cube, createColorMaterial(engine, glm::vec3(0.5f, 0.5f, 0.55f), 0.6f),
                  rasm::Transform{glm::vec3(1.4f, 0.4f, -1.2f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.8f)});
        rasm::VectorGeometry box;
        const rasm::LabelDesc boxLabels[2] = {{.position = {1.4, 0.81, -1.2}, .text = "On top", .anchor = rasm::LabelAnchor::BOTTOM}, {.position = {1.4, 0.0, -2.1}, .text = "Behind the box"}};
        rasm::addVectorLabels(box, origin, atlas, boxLabels, 2);
        addVectorEntity(engine, content, std::move(box), text);

        content.eye = glm::vec3(0.0f, 4.0f, 5.5f);
        content.center = glm::vec3(0.0f, 0.0f, -0.6f);
        content.fovY = 45.0f;
        return assets.cube.isValid() && font.isValid() && addSunsetLight(engine, assets, content, 0.8f, glm::vec3(0.4f, -0.8f, -0.3f), 2.5f);
    }

    // Roads and labels on hills (B35, B36): a road 1.2 m wide on cells of 0.5 m, draped across its whole width
    // (LineShape::drapeWidth), so no hill beside its middle covers its edges; a road 5 px wide on the ground's plane,
    // draped along its middle; names on the slopes, which the hills beside their anchors never cut; a post across a
    // name, which stays whole over it; and a name behind a box, hidden whole.
    bool buildDraped(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const DrapeTile tile = addTerrainTile(engine, content, -4.0, -4.0, 8.0, 16, glm::vec3(0.55f, 0.75f, 0.45f));
        const rasm::TerrainHeights heights = {.unorm16 = tile.samples.data(), .width = 19};
        const rasm::VectorPlacement drape = {.drapeTile = &tile.desc, .drapeHeights = heights};
        const glm::dvec3 origin(0.0);
        auto ground = [&](double x, double z)
        { return rasm::getTerrainPosition(tile.desc, heights, x / tile.desc.size + 0.5, z / tile.desc.size + 0.5); };

        std::vector<glm::dvec3> road;
        std::vector<glm::dvec3> lane;
        for (int32_t k = 0; k <= 36; ++k)
        {
            road.push_back({-3.6 + 0.2 * k, 0.0, 0.6 + 0.9 * std::sin(0.25 * k)});
            lane.push_back({-3.6 + 0.2 * k, 0.0, -2.4 + 0.4 * std::cos(0.3 * k)});
        }
        const auto count = static_cast<uint32_t>(road.size());
        rasm::VectorGeometry roads;
        rasm::addVectorLines(roads, origin, road.data(), &count, 1, {.drapeWidth = 1.2f}, drape);
        addVectorEntity(engine, content, std::move(roads),
                        createVectorMaterial(engine, {.color = glm::vec4(0.3f, 0.3f, 0.32f, 1.0f), .width = 1.2f, .widthUnit = rasm::VectorUnit::METRES, .facing = rasm::VectorFacing::GROUND, .outlineColor = glm::vec4(0.95f, 0.9f, 0.6f, 1.0f), .outlineWidth = 0.15f}));
        rasm::VectorGeometry lanes;
        rasm::addVectorLines(lanes, origin, lane.data(), &count, 1, {}, drape);
        addVectorEntity(engine, content, std::move(lanes), createVectorMaterial(engine, {.color = glm::vec4(0.75f, 0.2f, 0.15f, 1.0f), .width = 5.0f, .facing = rasm::VectorFacing::GROUND}));

        rasm::FontAtlas atlas;
        if (!engine.loadFont("fonts/Roboto-Medium.ttf", atlas))
            return false;
        const rasm::FontHandle font = engine.createFont(atlas);
        addEntity(engine, content, "post", assets.cube, createColorMaterial(engine, glm::vec3(0.35f, 0.3f, 0.3f), 0.6f),
                  rasm::Transform{ground(1.7, -0.6) + glm::dvec3(0.0, 0.6, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.08f, 1.2f, 0.08f)});
        addEntity(engine, content, "box", assets.cube, createColorMaterial(engine, glm::vec3(0.5f, 0.5f, 0.55f), 0.6f),
                  rasm::Transform{ground(-1.8, -0.7) + glm::dvec3(0.0, 0.6, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.9f, 1.6f, 0.9f)});
        const rasm::LabelDesc labels[4] = {{.position = ground(-2.6, 2.6), .text = "West Slope", .priority = 1.0f},
                                           {.position = ground(2.6, 2.4), .text = "Ridge Road", .priority = 1.0f},
                                           {.position = ground(1.4, -1.5), .text = "Across a post", .priority = 1.0f},
                                           {.position = ground(-1.8, -1.4), .text = "Behind the box", .priority = 1.0f}};
        rasm::VectorGeometry names;
        rasm::addVectorLabels(names, origin, atlas, labels, 4);
        addVectorEntity(engine, content, std::move(names),
                        createVectorMaterial(engine, {.color = glm::vec4(0.12f, 0.1f, 0.08f, 1.0f), .width = 16.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 2.0f, .drawOrder = 40, .font = font}));

        content.eye = glm::vec3(-1.0f, 5.5f, 7.5f);
        content.center = glm::vec3(0.0f, 0.0f, 0.0f);
        content.fovY = 50.0f;
        return assets.cube.isValid() && font.isValid() && addSunsetLight(engine, assets, content, 0.8f, glm::vec3(0.4f, -0.8f, -0.3f), 2.5f);
    }

    // What the picking tests pick: a ground and a box (planes, so the positions are exact), a terrain tile, CesiumMan
    // (skinned, posed on the GPU), a line, a label, a see-through polygon and an overlay line over the box.
    bool buildPicking(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        addGround(engine, assets, content, 8.0f, glm::vec3(0.75f, 0.74f, 0.7f));
        const rasm::EntityHandle ground = content.entities.back();
        addTerrainTile(engine, content, 2.0, -4.0, 4.0, 16, glm::vec3(0.55f, 0.75f, 0.45f));
        const rasm::EntityHandle terrain = content.entities.back();
        const rasm::EntityHandle box = addEntity(engine, content, "box", assets.cube, createColorMaterial(engine, glm::vec3(0.5f, 0.5f, 0.55f), 0.6f),
                                                 rasm::Transform{glm::vec3(1.2f, 0.35f, 0.9f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.7f)});
        const rasm::MeshHandle manMesh = engine.loadGLTF("models/gltf/CesiumMan/CesiumMan.glb");
        rasm::FontAtlas atlas;
        if (!manMesh.isValid() || !engine.loadFont("fonts/Roboto-Medium.ttf", atlas))
            return false;
        const rasm::EntityHandle man = addModel(engine, content, "CesiumMan", manMesh, makeTransform(glm::vec3(-2.6f, 0.0f, 1.0f), 1.0f, 0.4f));
        engine.playAnimation(man, rasm::AnimationPlay{.clip = 0});

        const glm::dvec3 origin(0.0);
        const uint32_t two = 2;
        const uint32_t lineId = 11;
        rasm::VectorGeometry line;
        const glm::dvec3 linePoints[2] = {{-3.0, 0.0, -1.0}, {-0.5, 0.0, -1.0}};
        rasm::addVectorLines(line, origin, linePoints, &two, 1, {}, {}, &lineId);
        addVectorEntity(engine, content, std::move(line), createVectorMaterial(engine, {.color = glm::vec4(0.1f, 0.3f, 0.8f, 1.0f), .width = 8.0f}));
        const rasm::EntityHandle lineEntity = content.entities.back();

        const rasm::FontHandle font = engine.createFont(atlas);
        rasm::VectorGeometry text;
        const rasm::LabelDesc label = {.position = {-1.5, 0.0, 0.6}, .text = "PICK", .priority = 1.0f, .featureId = 21};
        rasm::addVectorLabels(text, origin, atlas, &label, 1);
        addVectorEntity(engine, content, std::move(text),
                        createVectorMaterial(engine, {.color = glm::vec4(0.12f, 0.1f, 0.08f, 1.0f), .width = 22.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 3.0f, .drawOrder = 40, .font = font}));
        const rasm::EntityHandle labelEntity = content.entities.back();

        // A small square drawn over the big one by its draw order, though it comes first in the draw list.
        const uint32_t four = 4;
        const uint32_t oneRing = 1;
        rasm::VectorGeometry small;
        const glm::dvec3 smallSquare[4] = {{-3.6, 0.0, -2.9}, {-3.2, 0.0, -2.9}, {-3.2, 0.0, -2.6}, {-3.6, 0.0, -2.6}};
        const uint32_t smallId = 43;
        rasm::addVectorPolygons(small, origin, smallSquare, &four, &oneRing, 1, {}, &smallId);
        addVectorEntity(engine, content, std::move(small), createVectorMaterial(engine, {.color = glm::vec4(0.2f, 0.6f, 0.2f, 0.9f), .drawOrder = 10}));
        const rasm::EntityHandle smallEntity = content.entities.back();

        rasm::VectorGeometry polygon;
        const glm::dvec3 square[4] = {{-3.8, 0.0, -3.0}, {-2.2, 0.0, -3.0}, {-2.2, 0.0, -1.8}, {-3.8, 0.0, -1.8}};
        const uint32_t polygonId = 41;
        rasm::addVectorPolygons(polygon, origin, square, &four, &oneRing, 1, {}, &polygonId);
        addVectorEntity(engine, content, std::move(polygon), createVectorMaterial(engine, {.color = glm::vec4(0.9f, 0.4f, 0.1f, 0.4f), .drawOrder = -5}));
        const rasm::EntityHandle polygonEntity = content.entities.back();

        rasm::VectorGeometry over;
        const glm::dvec3 overPoints[2] = {{0.4, 0.0, 0.9}, {2.0, 0.0, 0.9}};
        const uint32_t overId = 31;
        rasm::addVectorLines(over, origin, overPoints, &two, 1, {}, {}, &overId);
        addVectorEntity(engine, content, std::move(over), createVectorMaterial(engine, {.color = glm::vec4(0.7f, 0.1f, 0.8f, 1.0f), .width = 6.0f, .drawOrder = 20, .depth = rasm::VectorDepth::OVERLAY}));
        const rasm::EntityHandle overlay = content.entities.back();

        const glm::dvec3 up(0.0, 1.0, 0.0);
        const uint32_t onlyBox = rasm::PICK_SKIP_OVERLAYS;
        content.probes = {
            {.name = "ground", .point = {-0.5, 0.0, -2.5}, .entity = ground, .plane = up},
            {.name = "box top", .point = {1.3, 0.7, 0.8}, .entity = box, .plane = up},
            {.name = "box front", .point = {1.1, 0.4, 1.25}, .entity = box, .plane = {0.0, 0.0, 1.0}},
            {.name = "terrain", .point = {3.5, 1.6 * std::sin(0.45 * 3.5) * std::cos(0.35 * -2.0) + 0.5 * std::sin(1.3 * 3.5 + 0.9 * -2.0), -2.0}, .entity = terrain, .flags = rasm::PICK_HIT_TERRAIN, .metres = 0.02},
            {.name = "skinned", .point = {-2.6, 0.9, 1.0}, .entity = man, .pixels = 30.0},
            {.name = "line", .point = {-1.5, 0.0, -1.0}, .entity = lineEntity, .feature = 11, .flags = rasm::PICK_HIT_FEATURE, .pixels = 8.0},
            {.name = "label", .point = {-1.5, 0.0, 0.6}, .entity = labelEntity, .feature = 21, .flags = rasm::PICK_HIT_LABEL, .pixels = 30.0, .size = 9},
            {.name = "overlay", .point = {1.2, 0.0, 0.9}, .entity = overlay, .feature = 31, .flags = rasm::PICK_HIT_OVERLAY, .pixels = 6.0},
            {.name = "under overlay", .point = {1.2, 0.0, 0.9}, .entity = box, .pixels = 60.0, .skip = onlyBox},
            {.name = "polygon at 0.3", .point = {-3.0, 0.0, -2.4}, .entity = polygonEntity, .feature = 41, .flags = rasm::PICK_HIT_FEATURE, .minAlpha = 0.3f},
            {.name = "polygon at 0.5", .point = {-3.0, 0.0, -2.4}, .entity = ground, .plane = up},
            {.name = "order on top", .point = {-3.4, 0.0, -2.75}, .entity = smallEntity, .feature = 43, .flags = rasm::PICK_HIT_FEATURE, .minAlpha = 0.3f},
        };
        content.eye = glm::vec3(0.0f, 4.0f, 5.5f);
        content.center = glm::vec3(0.0f, 0.0f, -0.6f);
        content.fovY = 50.0f;
        return assets.cube.isValid() && font.isValid() && addSunsetLight(engine, assets, content, 0.8f, glm::vec3(0.4f, -0.8f, -0.3f), 2.5f);
    }

    constexpr rasm::Grid FLOOR_GRID = {.enabled = true, .cellSize = 0.01, .fadeDistance = 40.0f};
    constexpr rasm::Grid ORTHO_GRID = {.enabled = true,
                                       .origin = glm::dvec3(0.5, 0.02, 0.0),
                                       .cellSize = 0.1,
                                       .extent = glm::vec2(3.0f, 2.0f),
                                       .minorColor = glm::vec4(0.01f, 0.02f, 0.06f, 0.5f),
                                       .majorColor = glm::vec4(0.01f, 0.02f, 0.06f, 0.9f)};

    // The grid tests' scene: no ground, so the camera's grid is the floor; boxes stand on it, one of them see-through,
    // which blends over the grid.
    bool buildGrid(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        addEntity(engine, content, "box", assets.cube, createColorMaterial(engine, glm::vec3(0.7f, 0.35f, 0.2f), 0.5f),
                  rasm::Transform{glm::vec3(-1.0f, 0.5f, -1.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
        addEntity(engine, content, "tall box", assets.cube, createColorMaterial(engine, glm::vec3(0.25f, 0.45f, 0.7f), 0.5f),
                  rasm::Transform{glm::vec3(1.6f, 1.0f, -3.0f), glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0.8f, 2.0f, 0.8f)});
        rasm::MaterialHandle glass = createColorMaterial(engine, glm::vec3(0.6f, 0.85f, 0.6f), 0.2f);
        rasm::Material values = engine.getMaterial(glass);
        values.alphaMode = rasm::AlphaMode::BLEND;
        values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 3] = 0.45f;
        engine.updateMaterial(glass, values);
        addEntity(engine, content, "glass", assets.cube, glass, rasm::Transform{glm::vec3(0.6f, 0.4f, 0.6f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.8f)});
        content.eye = glm::vec3(-1.5f, 1.6f, 4.5f);
        content.center = glm::vec3(0.3f, 0.0f, -2.0f);
        content.fovY = 55.0f;
        content.grid = FLOOR_GRID;
        return assets.cube.isValid() && addSunsetLight(engine, assets, content, 0.6f, glm::vec3(0.5f, -0.7f, -0.4f), 2.0f);
    }

    // The same scene through an oblique orthographic box.
    bool buildGridOrtho(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        content.orthoHeight = 7.0f;
        return buildGrid(engine, assets, content);
    }

    rasm::MaterialHandle createPointMaterial(rasm::Engine &engine, const rasm::PointMaterial &style)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::POINTS);
        rasm::Material values = engine.getMaterial(material);
        values.points = style;
        engine.updateMaterial(material, values);
        return material;
    }

    // The point cloud tests' scene: a wavy sheet of 64 K points colored by height through viridis (LOD by prefix keeps
    // part of it), a ball of RGB squares, nine spheres a few pixels wide in metres around a box, which they cut into, and
    // a pick probe on a sphere and on the sheet's nearest corner point.
    bool buildPoints(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        std::vector<glm::vec3> sheet;
        std::vector<float> heights;
        for (int z = 0; z < 256; ++z)
        {
            for (int x = 0; x < 256; ++x)
            {
                const float px = static_cast<float>(x) / 255.0f * 6.0f - 3.0f;
                const float pz = static_cast<float>(z) / 255.0f * 6.0f - 3.0f;
                sheet.emplace_back(px, 0.25f * std::sin(2.0f * px) * std::cos(1.5f * pz), pz);
                heights.push_back(sheet.back().y);
            }
        }
        const rasm::TextureHandle viridis = engine.createColormap(rasm::ColormapPreset::VIRIDIS);
        const rasm::MaterialHandle byHeight = createPointMaterial(engine, {.size = 2.0f, .colorMode = rasm::PointColorMode::SCALAR, .colormap = viridis, .range = {.min = -0.25f, .max = 0.25f}});
        const rasm::EntityHandle sheetEntity = addEntity(engine, content, "sheet",
                                                         engine.createMesh(rasm::buildPointCloudChunk({.positions = sheet.data(), .scalars = heights.data(), .count = static_cast<uint32_t>(sheet.size())})),
                                                         byHeight, rasm::Transform{glm::vec3(0.0f, 0.0f, -2.0f)});

        std::vector<glm::vec3> ball;
        std::vector<uint32_t> colors;
        for (uint32_t i = 0; i < 4000; ++i)
        {
            const float t = static_cast<float>(i) + 0.5f;
            const float y = 1.0f - 2.0f * t / 4000.0f;
            const float r = std::sqrt(1.0f - y * y);
            const float a = t * 2.39996f;
            ball.emplace_back(0.6f * r * std::cos(a), 0.6f * y, 0.6f * r * std::sin(a));
            colors.push_back(rasm::packVertexColor(glm::vec4(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * y, 0.8f, 1.0f)));
        }
        addEntity(engine, content, "ball", engine.createMesh(rasm::buildPointCloudChunk({.positions = ball.data(), .colors = colors.data(), .count = 4000})),
                  createPointMaterial(engine, {.size = 3.0f, .shape = rasm::PointShape::SQUARE}), rasm::Transform{glm::vec3(-1.6f, 0.9f, -1.0f)});

        std::vector<glm::vec3> spheres;
        for (int i = 0; i < 9; ++i)
            spheres.emplace_back(static_cast<float>(i % 3) * 0.45f - 0.45f, static_cast<float>(i / 3) * 0.45f, 0.0f);
        const rasm::EntityHandle sphereEntity =
            addEntity(engine, content, "spheres", engine.createMesh(rasm::buildPointCloudChunk({.positions = spheres.data(), .count = 9})),
                      createPointMaterial(engine, {.color = glm::vec4(0.95f, 0.45f, 0.15f, 1.0f), .size = 0.3f, .sizeUnit = rasm::VectorUnit::METRES, .shape = rasm::PointShape::SPHERE}),
                      rasm::Transform{glm::vec3(1.4f, 0.4f, -0.5f)});
        addEntity(engine, content, "box", assets.cube, createColorMaterial(engine, glm::vec3(0.3f, 0.4f, 0.65f), 0.5f),
                  rasm::Transform{glm::vec3(1.4f, 0.6f, -0.65f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.9f, 0.3f, 0.3f)});

        content.eye = glm::vec3(0.0f, 2.2f, 4.2f);
        content.center = glm::vec3(0.0f, 0.0f, -1.5f);
        content.fovY = 50.0f;
        // The middle sphere's centre (index 4), and the sheet's point nearest the camera (index 255 * 256): a point is
        // picked at its centre, which the hit's position is.
        content.probes.push_back({.name = "sphere", .point = glm::dvec3(1.4, 0.85, -0.5), .entity = sphereEntity, .feature = 4, .flags = rasm::PICK_HIT_POINT | rasm::PICK_HIT_FEATURE, .metres = 1e-3});
        content.probes.push_back({.name = "sheet corner", .point = glm::dvec3(sheet[255 * 256]) + glm::dvec3(0.0, 0.0, -2.0), .entity = sheetEntity, .feature = 255 * 256, .flags = rasm::PICK_HIT_POINT | rasm::PICK_HIT_FEATURE, .metres = 1e-3});
        return assets.cube.isValid() && viridis.isValid() && addSunsetLight(engine, assets, content, 0.6f, glm::vec3(0.5f, -0.7f, -0.4f), 2.0f);
    }

    // Adaptive points (PointSizeMode::ADAPTIVE): a sparse random sheet near the camera, grown to close, and two finer
    // sheets over two of its quarters, as an octree's children: the right one drawn whole, so the parent's points over it
    // keep their size (PointChunkLod::finer 1), the left one half faded in (drawn 0.5, finer 0.5).
    bool buildPointsAdaptive(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        uint64_t state = 7;
        const auto next = [&state]()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<float>(state >> 40) / static_cast<float>(1u << 24);
        };
        const auto makeSheet = [&next](uint32_t count, float x0, float x1, float z0, float z1, float y)
        {
            std::vector<glm::vec3> sheet;
            for (uint32_t i = 0; i < count; ++i)
                sheet.emplace_back(x0 + (x1 - x0) * next(), y + 0.02f * next(), z0 + (z1 - z0) * next());
            return sheet;
        };
        const rasm::PointMaterial adaptive{.size = 2.0f, .sizeMode = rasm::PointSizeMode::ADAPTIVE, .maxSize = 40.0f};
        rasm::PointMaterial parentStyle = adaptive;
        parentStyle.color = glm::vec4(0.95f, 0.55f, 0.2f, 1.0f);
        rasm::PointMaterial rightStyle = adaptive;
        rightStyle.color = glm::vec4(0.25f, 0.5f, 0.95f, 1.0f);
        rasm::PointMaterial leftStyle = adaptive;
        leftStyle.color = glm::vec4(0.3f, 0.8f, 0.35f, 1.0f);
        const std::vector<glm::vec3> parent = makeSheet(3000, -3.0f, 3.0f, -3.0f, 3.0f, 0.01f);
        const std::vector<glm::vec3> right = makeSheet(6000, 0.0f, 3.0f, 0.0f, 3.0f, 0.03f);
        const std::vector<glm::vec3> left = makeSheet(6000, -3.0f, 0.0f, 0.0f, 3.0f, 0.03f);
        const rasm::Transform at{glm::vec3(0.0f, 0.0f, -1.0f)};
        const rasm::EntityHandle parentEntity = addEntity(engine, content, "parent", engine.createMesh(rasm::buildPointCloudChunk({.positions = parent.data(), .count = 3000})),
                                                          createPointMaterial(engine, parentStyle), at);
        addEntity(engine, content, "right", engine.createMesh(rasm::buildPointCloudChunk({.positions = right.data(), .count = 6000})), createPointMaterial(engine, rightStyle), at);
        const rasm::EntityHandle leftEntity =
            addEntity(engine, content, "left", engine.createMesh(rasm::buildPointCloudChunk({.positions = left.data(), .count = 6000})), createPointMaterial(engine, leftStyle), at);
        // Octants of the parent's space: x >= 0 is 1, y >= 0 is 2, z >= 0 is 4; the sheets lie above y = 0.
        rasm::PointChunkLod parentLod;
        parentLod.finer[1 | 2 | 4] = 1.0f;
        parentLod.finer[2 | 4] = 0.5f;
        engine.setPointChunkLod(parentEntity, parentLod);
        engine.setPointChunkLod(leftEntity, {.drawn = 0.5f});
        content.eye = glm::vec3(0.0f, 2.4f, 3.4f);
        content.center = glm::vec3(0.0f, 0.0f, -1.2f);
        content.fovY = 60.0f;
        return addSunsetLight(engine, assets, content, 0.6f, glm::vec3(0.5f, -0.7f, -0.4f), 2.0f);
    }

    // Points of 3 to 5 pixels, which the compute path cuts to the triangles' shapes: a wavy sheet of round points of 5
    // pixels colored by height, a ball of SPHERE points of 4 (their depth per pixel) cutting into it, and a ring of
    // squares of 3.
    bool buildPointsLarge(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        std::vector<glm::vec3> sheet;
        std::vector<float> heights;
        for (int z = 0; z < 128; ++z)
        {
            for (int x = 0; x < 128; ++x)
            {
                const float px = static_cast<float>(x) / 127.0f * 6.0f - 3.0f;
                const float pz = static_cast<float>(z) / 127.0f * 6.0f - 3.0f;
                sheet.emplace_back(px, 0.25f * std::sin(2.0f * px) * std::cos(1.5f * pz), pz);
                heights.push_back(sheet.back().y);
            }
        }
        const rasm::TextureHandle viridis = engine.createColormap(rasm::ColormapPreset::VIRIDIS);
        addEntity(engine, content, "sheet", engine.createMesh(rasm::buildPointCloudChunk({.positions = sheet.data(), .scalars = heights.data(), .count = static_cast<uint32_t>(sheet.size())})),
                  createPointMaterial(engine, {.size = 5.0f, .colorMode = rasm::PointColorMode::SCALAR, .colormap = viridis, .range = {.min = -0.25f, .max = 0.25f}}),
                  rasm::Transform{glm::vec3(0.0f, 0.0f, -2.0f)});

        std::vector<glm::vec3> ball;
        for (uint32_t i = 0; i < 3000; ++i)
        {
            const float t = static_cast<float>(i) + 0.5f;
            const float y = 1.0f - 2.0f * t / 3000.0f;
            const float r = std::sqrt(1.0f - y * y);
            const float a = t * 2.39996f;
            ball.emplace_back(0.7f * r * std::cos(a), 0.7f * y, 0.7f * r * std::sin(a));
        }
        addEntity(engine, content, "ball", engine.createMesh(rasm::buildPointCloudChunk({.positions = ball.data(), .count = 3000})),
                  createPointMaterial(engine, {.color = glm::vec4(0.95f, 0.45f, 0.15f, 1.0f), .size = 4.0f, .shape = rasm::PointShape::SPHERE}), rasm::Transform{glm::vec3(0.6f, 0.2f, -1.2f)});

        std::vector<glm::vec3> ring;
        for (uint32_t i = 0; i < 4000; ++i)
        {
            const float a = static_cast<float>(i) / 4000.0f * 6.2831853f;
            ring.emplace_back(1.1f * std::cos(a), 0.15f * std::sin(5.0f * a), 1.1f * std::sin(a));
        }
        addEntity(engine, content, "ring", engine.createMesh(rasm::buildPointCloudChunk({.positions = ring.data(), .count = 4000})),
                  createPointMaterial(engine, {.color = glm::vec4(0.9f, 0.9f, 0.95f, 1.0f), .size = 3.0f, .shape = rasm::PointShape::SQUARE}), rasm::Transform{glm::vec3(-1.4f, 0.5f, -1.0f)});

        content.eye = glm::vec3(0.0f, 2.2f, 4.2f);
        content.center = glm::vec3(0.0f, 0.0f, -1.5f);
        content.fovY = 50.0f;
        return viridis.isValid() && addSunsetLight(engine, assets, content, 0.6f, glm::vec3(0.5f, -0.7f, -0.4f), 2.0f);
    }

    bool buildPointsOrtho(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        content.orthoHeight = 6.0f;
        return buildPoints(engine, assets, content);
    }

    // The glyph tests' scene: a swirl of 1024 arrows over a ground, sized and colored by their speed through viridis,
    // some of them behind a wall (Hi-Z culls them); a ring of RGB spheres, a second shape in the same draw; streamlines
    // colored by their speed through plasma, and one line with RGB points; pick probes on a sphere's front and a line.
    bool buildGlyphs(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle arrow = engine.createMesh(rasm::buildGlyphShape(rasm::GlyphShape::ARROW));
        const rasm::MeshHandle sphere = engine.createMesh(rasm::buildGlyphShape(rasm::GlyphShape::SPHERE));
        const auto swirl = [](const glm::vec3 &p)
        { return glm::vec3(-p.z, 0.35f + 0.25f * std::sin(2.0f * p.x), p.x); };
        std::vector<glm::vec3> positions;
        std::vector<glm::quat> orientations;
        std::vector<glm::vec3> scales;
        std::vector<float> speeds;
        for (int y = 0; y < 4; ++y)
        {
            for (int z = 0; z < 16; ++z)
            {
                for (int x = 0; x < 16; ++x)
                {
                    const glm::vec3 p(static_cast<float>(x) * 0.36f - 2.7f, static_cast<float>(y) * 0.35f + 0.1f, static_cast<float>(z) * 0.36f - 2.7f);
                    const glm::vec3 v = swirl(p);
                    positions.push_back(p);
                    orientations.push_back(rasm::getGlyphOrientation(v));
                    speeds.push_back(glm::length(v));
                    scales.push_back(glm::vec3(0.12f + 0.08f * speeds.back()));
                }
            }
        }
        const rasm::MaterialHandle bySpeed = engine.createMaterial(rasm::MaterialType::GLYPHS);
        rasm::Material values = engine.getMaterial(bySpeed);
        values.glyphs = {.colorMode = rasm::PointColorMode::SCALAR, .colormap = engine.createColormap(rasm::ColormapPreset::VIRIDIS), .range = {.min = 0.3f, .max = 4.0f}};
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.5f;
        engine.updateMaterial(bySpeed, values);
        addEntity(engine, content, "arrows",
                  engine.createMesh(rasm::buildGlyphChunk({.shape = arrow,
                                                           .positions = positions.data(),
                                                           .orientations = orientations.data(),
                                                           .scales = scales.data(),
                                                           .scalars = speeds.data(),
                                                           .count = static_cast<uint32_t>(positions.size())})),
                  bySpeed, rasm::Transform{glm::vec3(0.0f, 0.0f, -2.0f)});

        std::vector<glm::vec3> ring;
        std::vector<uint32_t> colors;
        for (uint32_t i = 0; i < 12; ++i)
        {
            const float a = static_cast<float>(i) / 12.0f * 6.2831853f;
            ring.emplace_back(0.7f * std::cos(a), 0.0f, 0.7f * std::sin(a));
            colors.push_back(rasm::packVertexColor(glm::vec4(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a), 0.9f - 0.6f * static_cast<float>(i % 2), 1.0f)));
        }
        const glm::vec3 ringScale[12] = {glm::vec3(0.3f), glm::vec3(0.3f), glm::vec3(0.3f), glm::vec3(0.3f), glm::vec3(0.3f), glm::vec3(0.3f),
                                         glm::vec3(0.3f), glm::vec3(0.3f), glm::vec3(0.3f, 0.6f, 0.3f), glm::vec3(0.3f), glm::vec3(0.3f), glm::vec3(0.3f)};
        const rasm::MaterialHandle own = engine.createMaterial(rasm::MaterialType::GLYPHS);
        values = engine.getMaterial(own);
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.35f;
        engine.updateMaterial(own, values);
        const rasm::EntityHandle spheres = addEntity(engine, content, "spheres",
                                                     engine.createMesh(rasm::buildGlyphChunk({.shape = sphere, .positions = ring.data(), .scales = ringScale, .colors = colors.data(), .count = 12})),
                                                     own, rasm::Transform{glm::vec3(1.6f, 1.6f, -0.8f)});

        addGround(engine, assets, content, 4.0f, glm::vec3(0.45f, 0.45f, 0.42f));
        addEntity(engine, content, "wall", assets.cube, createColorMaterial(engine, glm::vec3(0.7f, 0.35f, 0.25f), 0.6f),
                  rasm::Transform{glm::vec3(-1.2f, 0.6f, 0.4f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.6f, 1.2f, 0.1f)});

        // Streamlines of the swirl from a row of seeds, a scalar per point; and a line whose points have their own colors.
        std::vector<glm::dvec3> points;
        std::vector<float> lineSpeeds;
        std::vector<uint32_t> sizes;
        for (int seed = 0; seed < 6; ++seed)
        {
            glm::vec3 p(0.3f + 0.4f * static_cast<float>(seed), 1.55f, 0.0f);
            for (int i = 0; i < 60; ++i)
            {
                points.emplace_back(glm::dvec3(p) + glm::dvec3(0.0, 0.0, -2.0));
                lineSpeeds.push_back(glm::length(swirl(p)));
                const glm::vec3 v = swirl(p);
                p += 0.06f * glm::vec3(v.x, 0.0f, v.z) / glm::length(glm::vec2(v.x, v.z));
            }
            sizes.push_back(60);
        }
        rasm::VectorGeometry geometry;
        rasm::addVectorLines(geometry, glm::dvec3(0.0), points.data(), sizes.data(), static_cast<uint32_t>(sizes.size()), {}, {}, nullptr, nullptr, lineSpeeds.data());
        // A line without values in the same mesh keeps the material's color (white), not the colormap's.
        const glm::dvec3 plain[2] = {{-2.8, 1.55, -0.2}, {-0.8, 1.55, -0.2}};
        const uint32_t plainSize = 2;
        rasm::addVectorLines(geometry, glm::dvec3(0.0), plain, &plainSize, 1);
        const rasm::MaterialHandle lineStyle = engine.createMaterial(rasm::MaterialType::VECTOR);
        values = engine.getMaterial(lineStyle);
        values.vector.width = 3.0f;
        values.vector.colorMode = rasm::PointColorMode::SCALAR;
        values.vector.colormap = engine.createColormap(rasm::ColormapPreset::PLASMA);
        values.vector.range = {.min = 0.3f, .max = 4.0f};
        engine.updateMaterial(lineStyle, values);
        addEntity(engine, content, "streamlines", engine.createMesh(rasm::buildVectorMesh(std::move(geometry))), lineStyle, rasm::Transform{});

        const glm::dvec3 rgbLine[3] = {{-3.0, 0.02, 1.0}, {-1.5, 0.02, 1.6}, {0.0, 0.02, 1.0}};
        const uint32_t rgbColors[3] = {rasm::packVertexColor(glm::vec4(1.0f, 0.1f, 0.1f, 1.0f)), rasm::packVertexColor(glm::vec4(0.1f, 1.0f, 0.1f, 1.0f)),
                                       rasm::packVertexColor(glm::vec4(0.1f, 0.2f, 1.0f, 1.0f))};
        const uint32_t rgbSize = 3;
        const uint32_t rgbId = 7;
        rasm::VectorGeometry rgb;
        rasm::addVectorLines(rgb, glm::dvec3(0.0), rgbLine, &rgbSize, 1, {}, {}, &rgbId, rgbColors);
        const rasm::MaterialHandle rgbStyle = engine.createMaterial(rasm::MaterialType::VECTOR);
        values = engine.getMaterial(rgbStyle);
        values.vector.width = 6.0f;
        engine.updateMaterial(rgbStyle, values);
        const rasm::EntityHandle rgbEntity = addEntity(engine, content, "rgb line", engine.createMesh(rasm::buildVectorMesh(std::move(rgb))), rgbStyle, rasm::Transform{});

        content.eye = glm::vec3(0.5f, 3.2f, 4.6f);
        content.center = glm::vec3(0.0f, 0.4f, -1.6f);
        content.fovY = 50.0f;
        // The front of sphere 3 (a glyph's index in the desc), toward the eye; and the middle of the RGB line.
        const glm::vec3 sphereCentre = glm::vec3(1.6f, 1.6f, -0.8f) + ring[3];
        const glm::vec3 front = sphereCentre + 0.15f * glm::normalize(content.eye - sphereCentre);
        content.probes.push_back({.name = "sphere glyph", .point = glm::dvec3(front), .entity = spheres, .feature = 3, .flags = rasm::PICK_HIT_GLYPH | rasm::PICK_HIT_FEATURE, .metres = 0.01});
        content.probes.push_back({.name = "rgb line", .point = rgbLine[1], .entity = rgbEntity, .feature = rgbId, .flags = rasm::PICK_HIT_FEATURE, .pixels = 6.0});
        return arrow.isValid() && sphere.isValid() && addSunsetLight(engine, assets, content, 0.6f, glm::vec3(0.5f, -0.7f, -0.4f), 2.0f);
    }

    bool buildGlyphsOrtho(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        content.orthoHeight = 6.0f;
        return buildGlyphs(engine, assets, content);
    }

    // A see-through material: its color and alpha, and how its draws go over what is behind them.
    rasm::MaterialHandle createBlendMaterial(rasm::Engine &engine, rasm::MaterialType type, const glm::vec4 &color, rasm::BlendMethod method)
    {
        const rasm::MaterialHandle material = engine.createMaterial(type, {}, true);
        rasm::Material values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 4; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[static_cast<int>(i)];
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.4f;
        values.alphaMode = rasm::AlphaMode::BLEND;
        values.blendMethod = method;
        engine.updateMaterial(material, values);
        return material;
    }

    // Weighted blended transparency: three nested see-through spheres around a solid cube, six see-through slices in one
    // mesh (alpha and color by vertex), see-through arrows (a glyph chunk with a BLEND material), all weighted; an
    // additive glow; a sorted pane in front of them all, drawn over the weighted ones. Picks through the spheres: the
    // outer one with minAlpha 0, the inner one (alpha 0.55) with 0.5.
    bool buildWeighted(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        const rasm::MeshHandle sphere = engine.createMesh(rasm::buildGlyphShape(rasm::GlyphShape::SPHERE));
        const glm::vec3 centre(-0.9f, 1.2f, 0.0f);
        const float diameters[3] = {2.4f, 1.6f, 0.9f};
        const glm::vec4 colors[3] = {{0.25f, 0.55f, 0.95f, 0.25f}, {0.2f, 0.9f, 0.45f, 0.35f}, {0.98f, 0.55f, 0.15f, 0.55f}};
        rasm::EntityHandle spheres[3];
        for (uint32_t i = 0; i < 3; ++i)
            spheres[i] = addEntity(engine, content, "shell", sphere, createBlendMaterial(engine, rasm::MaterialType::PBR, colors[i], rasm::BlendMethod::WEIGHTED),
                                   rasm::Transform{centre, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(diameters[i])});
        addEntity(engine, content, "core", assets.cube, createColorMaterial(engine, glm::vec3(0.8f, 0.8f, 0.85f), 0.3f),
                  rasm::Transform{centre, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.35f)});

        // Slices across x, in one mesh: back to front from one side only, which sorting cannot fix.
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> sliceColors;
        for (uint32_t i = 0; i < 6; ++i)
        {
            const float x = 0.6f + 0.25f * static_cast<float>(i);
            const auto first = static_cast<uint32_t>(vertices.size());
            for (uint32_t corner = 0; corner < 4; ++corner)
            {
                rasm::Vertex vertex{};
                vertex.pos = glm::vec3(x, 0.2f + 1.6f * static_cast<float>(corner / 2), -0.8f + 1.6f * static_cast<float>(corner % 2));
                vertex.normal = glm::vec3(1.0f, 0.0f, 0.0f);
                vertices.push_back(vertex);
                const float t = static_cast<float>(i) / 5.0f;
                sliceColors.push_back(rasm::packVertexColor(glm::vec4(t, 0.4f + 0.3f * static_cast<float>(corner % 2), 1.0f - t, 0.3f + 0.2f * static_cast<float>(corner / 2))));
            }
            indices.insert(indices.end(), {first, first + 1, first + 3, first, first + 3, first + 2});
        }
        addEntity(engine, content, "slices", engine.createMesh(rasm::buildMesh(std::move(vertices), std::move(indices), std::move(sliceColors))),
                  createBlendMaterial(engine, rasm::MaterialType::UNLIT, glm::vec4(1.0f), rasm::BlendMethod::WEIGHTED), rasm::Transform{});

        // See-through arrows, always weighted.
        const rasm::MeshHandle arrow = engine.createMesh(rasm::buildGlyphShape(rasm::GlyphShape::ARROW));
        std::vector<glm::vec3> positions;
        std::vector<glm::quat> orientations;
        std::vector<float> scalars;
        for (int z = 0; z < 4; ++z)
        {
            for (int x = 0; x < 8; ++x)
            {
                positions.emplace_back(static_cast<float>(x) * 0.3f - 1.0f, 0.15f, static_cast<float>(z) * 0.3f + 1.3f);
                orientations.push_back(rasm::getGlyphOrientation(glm::vec3(1.0f, 0.6f, static_cast<float>(z) - 1.5f)));
                scalars.push_back(static_cast<float>(x) / 7.0f);
            }
        }
        const rasm::MaterialHandle arrows = engine.createMaterial(rasm::MaterialType::GLYPHS);
        rasm::Material values = engine.getMaterial(arrows);
        values.glyphs = {.colorMode = rasm::PointColorMode::SCALAR, .colormap = engine.createColormap(rasm::ColormapPreset::MAGMA), .range = {.min = 0.0f, .max = 1.0f}};
        values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 3] = 0.5f;
        values.alphaMode = rasm::AlphaMode::BLEND;
        engine.updateMaterial(arrows, values);
        addEntity(engine, content, "arrows",
                  engine.createMesh(rasm::buildGlyphChunk(
                      {.shape = arrow, .positions = positions.data(), .orientations = orientations.data(), .scalars = scalars.data(), .count = static_cast<uint32_t>(positions.size())})),
                  arrows, rasm::Transform{});

        addEntity(engine, content, "glow", sphere, createBlendMaterial(engine, rasm::MaterialType::UNLIT, glm::vec4(1.0f, 0.55f, 0.2f, 0.6f), rasm::BlendMethod::ADDITIVE),
                  rasm::Transform{glm::vec3(1.2f, 2.3f, 0.2f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.7f)});
        addEntity(engine, content, "pane", assets.cube, createBlendMaterial(engine, rasm::MaterialType::PBR, glm::vec4(0.9f, 0.2f, 0.25f, 0.4f), rasm::BlendMethod::SORTED),
                  rasm::Transform{glm::vec3(-0.2f, 0.9f, 2.2f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.9f, 0.6f, 0.02f)});
        addGround(engine, assets, content, 3.0f, glm::vec3(0.4f, 0.4f, 0.38f));

        content.eye = glm::vec3(0.6f, 2.4f, 5.2f);
        content.center = glm::vec3(0.0f, 0.9f, 0.0f);
        content.fovY = 45.0f;
        // Through the shells' centre from the eye: the outer shell's front with minAlpha 0, the inner one's with 0.5.
        const glm::vec3 toEye = glm::normalize(content.eye - centre);
        content.probes.push_back({.name = "outer shell", .point = glm::dvec3(centre + 0.5f * diameters[0] * toEye), .entity = spheres[0], .metres = 0.03, .minAlpha = 0.0f});
        content.probes.push_back({.name = "inner shell", .point = glm::dvec3(centre + 0.5f * diameters[2] * toEye), .entity = spheres[2], .metres = 0.03, .minAlpha = 0.5f});
        return addSunsetLight(engine, assets, content, 1.0f, glm::vec3(0.45f, -0.75f, -0.5f), 3.0f);
    }

    bool buildWeightedOrtho(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        content.orthoHeight = 4.5f;
        return buildWeighted(engine, assets, content);
    }

    // The picking scene through an orthographic box from the same eye, with a bounded grid just over its ground.
    bool buildOrtho(rasm::Engine &engine, const Assets &assets, Content &content)
    {
        content.orthoHeight = 6.5f;
        content.grid = ORTHO_GRID;
        return buildPicking(engine, assets, content);
    }

    // The tile_streaming test's frame: the streamer's update, and the tiles it hands out get their boxes at once.
    void updateTiles(rasm::Engine &engine, const Assets &assets, const Content &content, rasm::CameraHandle camera)
    {
        const rasm::View view = {.scene = content.scene, .camera = camera};
        content.tiles->setViews(&view, 1);
        content.tiles->update(content.cut.data(), static_cast<uint32_t>(content.cut.size()));
        uint32_t count = 0;
        const rasm::TileHandle *toLoad = content.tiles->getTilesToLoad(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            for (const TileBox &box : content.tileBoxes)
            {
                if (!(box.tile == toLoad[i]))
                    continue;
                const rasm::MaterialHandle material = createColorMaterial(engine, box.color, 0.8f);
                const rasm::EntityHandle entity = engine.createEntity("tile", assets.cube, material, box.transform);
                content.tiles->setTileContent(box.tile, {.entities = &entity, .entityCount = 1, .materials = &material, .materialCount = 1});
            }
        }
    }

    // clang-format off
    const ImageTest TESTS[] = {
        {.name = "helmet_ibl",    .build = buildHelmet},
        {.name = "sponza",        .build = buildSponza},
        {.name = "alpha_modes",   .build = buildAlphaModes},
        {.name = "msaa4",         .build = buildAlphaModes,    .samples = 4},
        {.name = "sponza_msaa4",  .build = buildSponza,        .samples = 4},
        {.name = "taa",           .build = buildHelmet,        .taa = true, .frames = 16},
        {.name = "bloom",         .build = buildEmissive,      .bloom = true},
        {.name = "many_lights",   .build = buildManyLights},
        {.name = "local_shadows", .build = buildLocalShadows},
        {.name = "ktx2",          .build = buildKtx2},
        {.name = "debug_normals", .build = buildHelmet,        .view = rasm::DebugView::NORMALS},
        {.name = "determinism",   .build = buildHelmet,        .threshold = 0, .fraction = 0.0, .sameAs = "helmet_ibl"},
        {.name = "skinning",      .build = buildSkinning,      .time = 1.25},
        {.name = "morph",         .build = buildMorph},
        {.name = "node_anim",     .build = buildNodeAnimation, .time = 0.3},
        {.name = "materials",     .build = buildMaterials},
        {.name = "views",         .build = buildViews,         .views = true},
        {.name = "views_window2", .build = buildViews,         .views = true, .secondWindow = true},
        {.name = "tile_streaming", .build = buildTiles},
        {.name = "terrain",       .build = buildTerrain},
        {.name = "vectors",       .build = buildVectors},
        {.name = "vectors_msaa4", .build = buildVectors,       .samples = 4},
        {.name = "vectors_taa",   .build = buildVectors,       .taa = true, .frames = 16},
        {.name = "labels",        .build = buildLabels},
        {.name = "labels_msaa4",  .build = buildLabels,        .samples = 4},
        {.name = "labels_taa",    .build = buildLabels,        .taa = true, .frames = 16},
        {.name = "draped",        .build = buildDraped},
        {.name = "draped_msaa4",  .build = buildDraped,        .samples = 4},
        {.name = "draped_taa",    .build = buildDraped,        .taa = true, .frames = 16},
        // Picks of the scene as drawn: with MSAA and TAA the answer is the pixel's centre without jitter, the same.
        {.name = "picking",       .build = buildPicking,       .time = 0.5, .picks = true},
        {.name = "picking_msaa4", .build = buildPicking,       .samples = 4, .time = 0.5, .picks = true},
        {.name = "picking_taa",   .build = buildPicking,       .taa = true, .frames = 16, .time = 0.5, .picks = true},
        // Orthographic views: the picking scene from the same eye (shadows, Hi-Z, terrain, skinning, vectors and labels in
        // pixels, picks), with a bounded grid just over its ground. The grid tests: levels from 1 cm cells to the horizon,
        // under perspective, an oblique orthographic box, MSAA and TAA.
        {.name = "ortho",         .build = buildOrtho,         .time = 0.5, .picks = true},
        {.name = "ortho_taa",     .build = buildOrtho,         .taa = true, .frames = 16, .time = 0.5, .picks = true},
        {.name = "grid",          .build = buildGrid},
        {.name = "grid_ortho",    .build = buildGridOrtho},
        {.name = "grid_msaa4",    .build = buildGrid,          .samples = 4},
        {.name = "grid_taa",      .build = buildGrid,          .taa = true, .frames = 16},
        // Point clouds: a triangle a point, cut to its shape, or the compute path for the small ones; LOD by prefix;
        // colormaps; picks of single points.
        {.name = "points",        .build = buildPoints,        .picks = true},
        {.name = "points_msaa4",  .build = buildPoints,        .samples = 4, .picks = true},
        {.name = "points_taa",    .build = buildPoints,        .taa = true, .frames = 16, .picks = true},
        {.name = "points_ortho",  .build = buildPointsOrtho,   .picks = true},
        // Instanced glyphs: each glyph culled and given its shape's LOD by a compute pass, drawn by one indirect draw; a
        // second shape in the same draw; data lines colored per point and through a colormap; picks of a glyph.
        {.name = "glyphs",        .build = buildGlyphs,        .picks = true},
        {.name = "glyphs_msaa4",  .build = buildGlyphs,        .samples = 4, .picks = true},
        {.name = "glyphs_taa",    .build = buildGlyphs,        .taa = true, .frames = 16, .picks = true},
        {.name = "glyphs_ortho",  .build = buildGlyphsOrtho,   .picks = true},
        // Weighted blended transparency: nested shells, slices in one mesh and see-through arrows in any order, an
        // additive glow, a sorted pane over them; picks through the shells by minAlpha.
        {.name = "weighted",      .build = buildWeighted,      .picks = true},
        {.name = "weighted_msaa4", .build = buildWeighted,     .samples = 4, .picks = true},
        {.name = "weighted_taa",  .build = buildWeighted,      .taa = true, .frames = 16, .picks = true},
        {.name = "weighted_ortho", .build = buildWeightedOrtho, .picks = true},
        // Adaptive points grown to close a sparse sheet, and an octree's parent and children (PointChunkLod).
        {.name = "points_adaptive", .build = buildPointsAdaptive},
        // Points of 3 to 5 pixels in each shape, by the compute path and by triangles only: the same pixels.
        {.name = "points_large",  .build = buildPointsLarge},
        {.name = "points_large_triangles", .build = buildPointsLarge, .computePoints = false, .sameAs = "points_large"},
        // The same points by triangles only, as a device without 64-bit atomics draws them: both paths take the same
        // pixels, so the image matches the one the compute path drew. Under MSAA a view without the compute path keeps
        // the points' triangles as they are, which leave out the samples of a pixel's edge past the square's left,
        // bottom and diagonal sides (0.7% of the pixels here), where the compute path covers them all.
        {.name = "points_triangles", .build = buildPoints,     .computePoints = false, .sameAs = "points", .picks = true},
        {.name = "points_msaa4_triangles", .build = buildPoints, .samples = 4, .computePoints = false, .fraction = 0.01, .sameAs = "points_msaa4", .picks = true},
        {.name = "points_taa_triangles", .build = buildPoints, .taa = true, .frames = 16, .computePoints = false, .sameAs = "points_taa", .picks = true},
        // The same images drawn by meshlets: the same triangles, so the same pixels. The mesh_ tests take the mesh shaders
        // where the device has them (lavapipe and the desktop GPUs do), else vertex pulling. lavapipe's mesh shaders round a
        // few positions differently from its vertex shaders: up to 3 levels on 0.2% of the pixels after TAA, and a few edge
        // pixels (3 of Sponza's 147456) that a triangle's neighbour covers. So they allow 3 levels, and 0.01% beyond them.
        {.name = "meshlets",      .build = buildHelmet,        .meshlets = rasm::MeshletPath::VERTEX_PULLING, .threshold = 0, .fraction = 0.0, .sameAs = "helmet_ibl"},
        {.name = "meshlet_sponza", .build = buildSponza,       .meshlets = rasm::MeshletPath::VERTEX_PULLING, .threshold = 0, .fraction = 0.0, .sameAs = "sponza"},
        {.name = "mesh_helmet",   .build = buildHelmet,        .meshlets = rasm::MeshletPath::MESH_SHADERS, .threshold = 3, .fraction = 0.0001, .sameAs = "helmet_ibl"},
        {.name = "mesh_sponza",   .build = buildSponza,        .meshlets = rasm::MeshletPath::MESH_SHADERS, .threshold = 3, .fraction = 0.0001, .sameAs = "sponza"},
        {.name = "mesh_msaa4",    .build = buildSponza,        .samples = 4, .meshlets = rasm::MeshletPath::MESH_SHADERS, .threshold = 3, .fraction = 0.0001, .sameAs = "sponza_msaa4"},
        {.name = "mesh_taa",      .build = buildHelmet,        .taa = true, .frames = 16, .meshlets = rasm::MeshletPath::MESH_SHADERS, .threshold = 3, .fraction = 0.0001, .sameAs = "taa"},
        {.name = "mesh_normals",  .build = buildHelmet,        .view = rasm::DebugView::NORMALS, .meshlets = rasm::MeshletPath::MESH_SHADERS, .threshold = 3, .fraction = 0.0001, .sameAs = "debug_normals"},
        // The same scenes moved 6370 km from the world's origin (Earth-centred coordinates of Paris): each view draws in
        // its camera's render space, so they look the same. Only the shadow maps' texel grids, which stay the world's,
        // move by part of a texel, so a few shadow edge pixels differ.
        {.name = "far_shadows",   .build = buildLocalShadows,  .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "local_shadows"},
        {.name = "far_skinning",  .build = buildSkinning,      .time = 1.25, .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "skinning"},
        {.name = "far_taa",       .build = buildHelmet,        .taa = true, .frames = 16, .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "taa"},
        {.name = "far_terrain",   .build = buildTerrain,       .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "terrain"},
        {.name = "far_labels",    .build = buildLabels,        .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "labels"},
        {.name = "far_picking",   .build = buildPicking,       .time = 0.5, .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "picking", .picks = true},
        {.name = "far_grid",      .build = buildGrid,          .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "grid"},
        {.name = "far_points",    .build = buildPoints,        .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "points", .picks = true},
        {.name = "far_glyphs",    .build = buildGlyphs,        .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.001, .sameAs = "glyphs", .picks = true},
        // The see-through shells' hashed shadows are noise per shadow texel, so the texel grid's move shifts more of them.
        {.name = "far_weighted",  .build = buildWeighted,      .origin = FAR_ORIGIN, .threshold = 8, .fraction = 0.003, .sameAs = "weighted", .picks = true},
    };
    // clang-format on

    bool parseOptions(int argc, char **argv, Options &options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const bool hasValue = i + 1 < argc;
            const bool takesValue = std::strcmp(argv[i], "--golden-dir") == 0 || std::strcmp(argv[i], "--out-dir") == 0 ||
                                    std::strcmp(argv[i], "--sponza") == 0 || std::strcmp(argv[i], "--scenes") == 0;
            if (takesValue && !hasValue)
            {
                std::printf("%s needs a value.\n", argv[i]);
                return false;
            }
            if (std::strcmp(argv[i], "--update") == 0)
                options.update = true;
            else if (std::strcmp(argv[i], "--list") == 0)
                options.list = true;
            else if (std::strcmp(argv[i], "--golden-dir") == 0 && hasValue)
                options.goldenDir = argv[++i];
            else if (std::strcmp(argv[i], "--out-dir") == 0 && hasValue)
                options.outDir = argv[++i];
            else if (std::strcmp(argv[i], "--sponza") == 0 && hasValue)
                options.sponza = argv[++i];
            else if (std::strcmp(argv[i], "--scenes") == 0 && hasValue)
            {
                const std::string list = argv[++i];
                for (size_t start = 0; start <= list.size();)
                {
                    const size_t end = std::min(list.find(',', start), list.size());
                    if (end > start)
                        options.scenes.push_back(list.substr(start, end - start));
                    start = end + 1;
                }
            }
            else
            {
                std::printf("Unknown option '%s'.\n", argv[i]);
                return false;
            }
        }
        for (const std::string &name : options.scenes)
            if (std::none_of(std::begin(TESTS), std::end(TESTS), [&](const ImageTest &test)
                             { return name == test.name; }))
            {
                std::printf("No test is called '%s' (--list shows them).\n", name.c_str());
                return false;
            }
        return true;
    }

    bool isSelected(const Options &options, const char *name)
    {
        return options.scenes.empty() || std::find(options.scenes.begin(), options.scenes.end(), name) != options.scenes.end();
    }

    // --update writes no golden for a test that compares with another test's image.
    bool isRun(const Options &options, const ImageTest &test)
    {
        return isSelected(options, test.name) && !(options.update && test.sameAs);
    }

    const ImageTest *findTest(const char *name)
    {
        for (const ImageTest &test : TESTS)
            if (std::strcmp(test.name, name) == 0)
                return &test;
        return nullptr;
    }

    Comparison compareImages(const rasm::ReadbackImage &actual, const rasm::ReadbackImage &expected, uint32_t threshold, rasm::ReadbackImage &diff)
    {
        Comparison result;
        result.sizeMatches = actual.width == expected.width && actual.height == expected.height && actual.pixels.size() == expected.pixels.size();
        if (!result.sizeMatches)
            return result;
        // Gray where the pixel is the same, orange where it is off by no more than the threshold, red where it differs.
        diff = actual;
        const size_t count = static_cast<size_t>(actual.width) * actual.height;
        for (size_t i = 0; i < count; ++i)
        {
            const uint8_t *a = actual.pixels.data() + i * 4;
            const uint8_t *e = expected.pixels.data() + i * 4;
            uint32_t pixelDiff = 0;
            for (int c = 0; c < 4; ++c)
                pixelDiff = std::max(pixelDiff, static_cast<uint32_t>(std::abs(static_cast<int>(a[c]) - static_cast<int>(e[c]))));
            result.maxDiff = std::max(result.maxDiff, pixelDiff);
            uint8_t *d = diff.pixels.data() + i * 4;
            const auto gray = static_cast<uint8_t>((e[0] + e[1] + e[2]) / 9);
            result.differing += pixelDiff > threshold ? 1 : 0;
            const uint8_t shown[3][4] = {{gray, gray, gray, 255}, {255, 160, 0, 255}, {255, 0, 0, 255}};
            std::memcpy(d, shown[pixelDiff > threshold ? 2 : (pixelDiff > 0 ? 1 : 0)], 4);
        }
        return result;
    }

    const Content *getContent(rasm::Engine &engine, const Assets &assets, rasm::CameraHandle camera, const ImageTest &test, std::vector<BuiltContent> &built)
    {
        for (const BuiltContent &entry : built)
            if (entry.build == test.build && entry.origin == test.origin)
                return entry.ok ? &entry.content : nullptr;
        BuiltContent entry{.build = test.build, .origin = test.origin};
        entry.content.scene = engine.createScene();
        engine.addCameraToScene(entry.content.scene, camera);
        entry.ok = entry.content.scene.isValid() && test.build(engine, assets, entry.content);
        if (entry.ok && test.origin != glm::dvec3(0.0))
            moveContent(engine, entry.content, test.origin);
        built.push_back(entry);
        return entry.ok ? &built.back().content : nullptr;
    }

    // Shadow passes, the scene passes, MSAA resolve or TAA, bloom, tone map: the graph an application would build.
    bool buildGraph(rasm::Engine &engine, const ImageTest &test, const Content &content, rasm::CameraHandle camera, rasm::RenderGraph &graph, rasm::RenderTargetHandle &sceneTarget)
    {
        sceneTarget = graph.createRenderTarget({
            .name = "scene_hdr",
            .swapchainScale = 1.0f,
            .colorFormats = {rasm::SCENE_COLOR_FORMAT, test.taa ? rasm::MOTION_FORMAT : rasm::Format::UNKNOWN},
            .depthFormat = rasm::Format::D32_SFLOAT,
            .samples = test.samples,
        });
        rasm::SceneShadow shadows[3];
        uint32_t shadowCount = 0;
        if (content.sun.isValid())
            shadows[shadowCount++] = engine.addShadowPasses(graph, content.scene, camera, rasm::ShadowDesc{.light = content.sun, .cascadeCount = 4, .size = 1024, .distance = content.sunDistance});
        for (uint32_t i = 0; i < content.localCount; ++i)
            shadows[shadowCount++] = engine.addShadowPasses(graph, content.scene, camera, rasm::ShadowDesc{.light = content.local[i], .size = 1024, .distance = 30.0f});
        for (uint32_t i = 0; i < shadowCount; ++i)
            if (shadows[i].mapCount == 0)
                return false;
        if (!engine.addScenePasses(graph, content.scene, camera, sceneTarget, shadows, shadowCount))
            return false;
        rasm::RenderTargetHandle resolved = sceneTarget;
        if (test.samples > 1)
            resolved = engine.addMsaaResolvePass(graph, sceneTarget, camera);
        if (test.taa)
            resolved = engine.addTaaPass(graph, sceneTarget, camera);
        if (!resolved.isValid())
            return false;
        const rasm::RenderTargetHandle bloom = test.bloom ? engine.addBloomPasses(graph, resolved, camera) : rasm::RenderTargetHandle{};
        return (!test.bloom || bloom.isValid()) && engine.addToneMapPass(graph, resolved, camera, bloom);
    }

    // The pixel a world point projects to in the test's frame, which the scene target fills.
    bool projectToPixel(rasm::Engine &engine, rasm::CameraHandle camera, const glm::dvec3 &world, uint32_t &x, uint32_t &y, glm::dmat4 &viewProjection)
    {
        viewProjection = glm::dmat4(engine.getProjectionMatrix(camera, static_cast<float>(WIDTH) / static_cast<float>(HEIGHT))) * engine.getViewMatrix(camera);
        const glm::dvec4 clip = viewProjection * glm::dvec4(world, 1.0);
        const double px = (clip.x / clip.w * 0.5 + 0.5) * WIDTH;
        const double py = (0.5 - clip.y / clip.w * 0.5) * HEIGHT;
        if (clip.w <= 0.0 || px < 0.0 || py < 0.0 || px >= WIDTH || py >= HEIGHT)
            return false;
        x = static_cast<uint32_t>(px);
        y = static_cast<uint32_t>(py);
        return true;
    }

    // Whether a probe's answer is right; prints a line when it is not.
    bool checkProbe(rasm::Engine &engine, const Content &content, rasm::CameraHandle camera, const ImageTest &test, const PickProbe &probe, const rasm::PickHit *hits, uint32_t count)
    {
        constexpr uint32_t CAPACITY = 64;
        const rasm::PickHit *hit = nullptr;
        for (uint32_t i = 0; i < std::min(count, CAPACITY) && hit == nullptr; ++i)
        {
            const bool same = hits[i].entity == probe.entity && (probe.feature == 0 || hits[i].feature == probe.feature) && (hits[i].flags & probe.flags) == probe.flags;
            if (same || probe.size == 1)
                hit = &hits[i];
        }
        const glm::dvec3 point = content.origin + probe.point;
        uint32_t x = 0;
        uint32_t y = 0;
        glm::dmat4 viewProjection(1.0);
        projectToPixel(engine, camera, point, x, y, viewProjection);
        if (hit == nullptr || hit->entity != probe.entity || (probe.feature != 0 && hit->feature != probe.feature) || (hit->flags & probe.flags) != probe.flags)
        {
            std::printf("  %s: pick %s: %s\n", test.name, probe.name, hit == nullptr ? "no hit" : "the wrong hit");
            return false;
        }
        double error = 0.0;
        double tolerance = 0.0;
        if (probe.plane != glm::dvec3(0.0))
        {
            // Where the ray through the hit pixel's centre meets the plane.
            const glm::dmat4 inverse = glm::inverse(viewProjection);
            const glm::dvec2 ndc((hit->x + 0.5) / WIDTH * 2.0 - 1.0, 1.0 - (hit->y + 0.5) / HEIGHT * 2.0);
            const glm::dvec4 a = inverse * glm::dvec4(ndc, 1.0, 1.0);
            const glm::dvec4 b = inverse * glm::dvec4(ndc, 0.5, 1.0);
            const glm::dvec3 from = glm::dvec3(a) / a.w;
            const glm::dvec3 direction = glm::dvec3(b) / b.w - from;
            const glm::dvec3 expected = from + glm::dot(point - from, probe.plane) / glm::dot(direction, probe.plane) * direction;
            error = glm::length(hit->position - expected);
            tolerance = 1e-4 * glm::length(expected - engine.getCameraTransform(camera).position) + 1e-5;
        }
        else
        {
            const double distance = glm::length(point - engine.getCameraTransform(camera).position);
            const double pixel = content.orthoHeight > 0.0f ? static_cast<double>(content.orthoHeight) / HEIGHT : 2.0 * distance * std::tan(glm::radians(static_cast<double>(content.fovY)) * 0.5) / HEIGHT;
            error = glm::length(hit->position - point);
            tolerance = probe.pixels * pixel + probe.metres;
        }
        if (error > tolerance)
        {
            std::printf("  %s: pick %s: off by %.6f m, more than %.6f\n", test.name, probe.name, error, tolerance);
            return false;
        }
        return true;
    }

    bool renderTest(rasm::Engine &engine, const Assets &assets, rasm::CameraHandle camera, const ImageTest &test, std::vector<BuiltContent> &built, rasm::ReadbackImage &image,
                    AllocationCount &steady, uint32_t &pickFailures)
    {
        const Content *content = getContent(engine, assets, camera, test, built);
        if (!content)
            return false;
        const float aspect = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT);
        const float halfHeight = 0.5f * content->orthoHeight;
        if (content->orthoHeight > 0.0f)
            engine.setCameraProjection(camera, rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC,
                                                                      .orthographic{.left = -halfHeight * aspect, .right = halfHeight * aspect, .bottom = -halfHeight, .top = halfHeight, .nearZ = 0.1f, .farZ = 200.0f}});
        else
            engine.setCameraProjection(camera, rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = content->fovY, .aspect = aspect, .nearZ = 0.1f}});
        rasm::Grid grid = content->grid;
        grid.origin += content->origin;
        engine.setCameraGrid(camera, grid);
        engine.setCameraTransform(camera, rasm::Transform{content->origin + glm::dvec3(content->eye), glm::quatLookAt(glm::normalize(content->center - content->eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)});
        engine.setCameraBloomStrength(camera, test.bloom ? 0.15f : 0.0f);
        engine.resetCameraHistory(camera);
        engine.setDebugView(test.view);
        engine.setMeshletPath(test.meshlets);
        engine.setComputePoints(test.computePoints);

        // A views test draws through Engine::render: the view texture first, then the split screen, the map, and the
        // security camera in the second window.
        const rasm::View views[] = {
            {.scene = content->scene, .camera = content->security, .output = {.target = content->viewTexture}},
            {.scene = content->scene, .camera = camera, .output = {.rect = {.x = 0.0f, .y = 0.0f, .width = 0.5f, .height = 1.0f}}},
            {.scene = content->scene, .camera = content->side, .output = {.rect = {.x = 0.5f, .y = 0.0f, .width = 0.5f, .height = 1.0f}}},
            {.scene = content->scene, .camera = content->map, .output = {.rect = {.x = 0.75f, .y = 0.0f, .width = 0.25f, .height = 0.44f}}, .bloom = rasm::ViewToggle::OFF},
            {.scene = content->scene, .camera = content->security, .output = {.window = content->window}},
        };
        rasm::RenderGraph graph = engine.createRenderGraph();
        rasm::RenderTargetHandle sceneTarget;
        // A picking test picks a 5 x 5 rect under the centre every frame, so the steady frames count its allocations too.
        rasm::PickHandle hover;
        auto frame = [&]()
        {
            engine.setAnimationTime(test.time);
            if (content->tiles)
                updateTiles(engine, assets, *content, camera);
            if (test.picks && hover.isValid() && engine.isPickReady(hover))
            {
                rasm::PickHit hits[4];
                uint32_t count = 0;
                engine.takePick(hover, hits, 4, count);
                hover = {};
            }
            if (test.picks && !hover.isValid())
                hover = engine.pick(content->scene, camera, sceneTarget, {.rect = {.x = WIDTH / 2 - 2, .y = HEIGHT / 2 - 2, .width = 5, .height = 5}});
            if (test.views)
                engine.render(views, 5);
            else
                graph.execute(camera);
        };
        if (!test.views && !buildGraph(engine, test, *content, camera, graph, sceneTarget))
        {
            engine.setDebugView(rasm::DebugView::NONE);
            engine.setMeshletPath(rasm::MeshletPath::OFF);
            return false;
        }
        // Every load is in place before the first frame, so frame N looks the same on every run.
        engine.waitForLoads();
        for (uint32_t i = 0; i < test.frames; ++i)
            frame();
        const rasm::ReadbackHandle copy = test.secondWindow ? engine.readFrame(content->window, rasm::ReadbackFormat::RGBA8_SRGB) : engine.readFrame(rasm::ReadbackFormat::RGBA8_SRGB);
        // The frame that copies keeps the same clock; the readback is ready once the GPU has finished it.
        for (uint32_t extra = 0; extra < 8 && copy.isValid() && !engine.isReadbackReady(copy); ++extra)
            frame();
        const bool taken = copy.isValid() && engine.takeReadback(copy, image);
        // Each probe's pick at the pixel its point projects to, all in one frame, read when the GPU is done with it.
        pickFailures = 0;
        if (test.picks && taken)
        {
            std::vector<rasm::PickHandle> picks;
            for (const PickProbe &probe : content->probes)
            {
                uint32_t x = 0;
                uint32_t y = 0;
                glm::dmat4 viewProjection(1.0);
                if (!projectToPixel(engine, camera, content->origin + probe.point, x, y, viewProjection))
                    std::printf("  %s: pick %s: its point is outside the frame\n", test.name, probe.name);
                const uint32_t half = probe.size / 2;
                picks.push_back(engine.pick(content->scene, camera, sceneTarget,
                                            {.rect = {.x = x - std::min(x, half), .y = y - std::min(y, half), .width = probe.size, .height = probe.size}, .minAlpha = probe.minAlpha, .skip = probe.skip}));
            }
            for (uint32_t extra = 0; extra < 8; ++extra)
                frame();
            for (size_t i = 0; i < picks.size(); ++i)
            {
                rasm::PickHit hits[64];
                uint32_t count = 0;
                const bool ok = engine.takePick(picks[i], hits, 64, count) && checkProbe(engine, *content, camera, test, content->probes[i], hits, count);
                pickFailures += ok ? 0 : 1;
            }
        }
        // A steady frame makes no heap allocation on the main thread.
        for (uint32_t i = 0; taken && i < SETTLE_FRAMES + STEADY_FRAMES; ++i)
        {
            if (i == SETTLE_FRAMES)
                startCountingAllocations();
            frame();
        }
        steady = taken ? stopCountingAllocations() : AllocationCount{};
        engine.setDebugView(rasm::DebugView::NONE);
        engine.setMeshletPath(rasm::MeshletPath::OFF);
        return taken;
    }

    std::string joinPath(const std::string &dir, const std::string &file)
    {
        return (std::filesystem::path(dir) / file).string();
    }

    bool writeFailure(const Options &options, const ImageTest &test, const rasm::ReadbackImage &actual, const rasm::ReadbackImage *expected, const rasm::ReadbackImage *diff)
    {
        const std::string dir = joinPath(options.outDir, test.name);
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        bool ok = rasm::writePng(joinPath(dir, "actual.png"), actual);
        if (expected)
            ok = rasm::writePng(joinPath(dir, "expected.png"), *expected) && ok;
        if (diff)
            ok = rasm::writePng(joinPath(dir, "diff.png"), *diff) && ok;
        return ok;
    }

    // Renders one test and compares it, printing its line. Returns whether it passed.
    bool runTest(rasm::Engine &engine, const Assets &assets, rasm::CameraHandle camera, const Options &options, const ImageTest &test, std::vector<BuiltContent> &built,
                 std::vector<Rendered> &rendered)
    {
        const auto start = std::chrono::steady_clock::now();
        rasm::ReadbackImage actual;
        AllocationCount steady;
        uint32_t pickFailures = 0;
        const bool renderedOk = renderTest(engine, assets, camera, test, built, actual, steady, pickFailures);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!renderedOk)
        {
            std::printf("%-14s %-6s %10s %8s %8.1f  (it did not render)\n", test.name, "FAIL", "-", "-", seconds);
            return false;
        }
        // A test whose pick checks failed fails (each printed its line), but its image is still written or compared.
        if (pickFailures > 0)
            std::printf("%-14s %-6s %10s %8s %8.1f  (%u pick checks failed)\n", test.name, "FAIL", "-", "-", seconds, pickFailures);
        // A test whose steady frames allocate fails, but its image is still written or compared.
        const bool allocated = steady.count > 0;
        if (allocated)
            std::printf("%-14s %-6s %10s %8s %8.1f  (%llu heap allocations, %llu bytes, in %u steady frames; the first of %llu bytes)\n", test.name, "FAIL", "-", "-", seconds,
                        static_cast<unsigned long long>(steady.count), static_cast<unsigned long long>(steady.bytes), STEADY_FRAMES,
                        static_cast<unsigned long long>(steady.firstSize));
        if (options.update)
        {
            std::error_code error;
            std::filesystem::create_directories(options.goldenDir, error);
            const bool written = rasm::writePng(joinPath(options.goldenDir, std::string(test.name) + ".png"), actual);
            std::printf("%-14s %-6s %10s %8s %8.1f\n", test.name, written ? "update" : "FAIL", "-", "-", seconds);
            rendered.push_back({test.name, std::move(actual)});
            return written && !allocated && pickFailures == 0;
        }

        // A test that compares with another test's image of this run renders that one first when it was not selected.
        rasm::ReadbackImage expected;
        uint32_t ignoredPicks = 0;
        if (test.sameAs)
        {
            auto found = std::find_if(rendered.begin(), rendered.end(), [&](const Rendered &r)
                                      { return r.name == test.sameAs; });
            if (found != rendered.end())
                expected = found->image;
            else if (AllocationCount ignored; renderTest(engine, assets, camera, *findTest(test.sameAs), built, expected, ignored, ignoredPicks))
                rendered.push_back({test.sameAs, expected});
        }
        else
        {
            const std::string golden = joinPath(options.goldenDir, std::string(test.name) + ".png");
            if (std::filesystem::exists(golden) && !rasm::readPng(golden, expected))
            {
                std::printf("%-14s %-6s %10s %8s %8.1f  (the golden does not decode: %s)\n", test.name, "FAIL", "-", "-", seconds, golden.c_str());
                writeFailure(options, test, actual, nullptr, nullptr);
                return false;
            }
        }
        if (expected.pixels.empty())
        {
            std::printf("%-14s %-6s %10s %8s %8.1f  (no golden: --update writes one)\n", test.name, "FAIL", "-", "-", seconds);
            writeFailure(options, test, actual, nullptr, nullptr);
            rendered.push_back({test.name, std::move(actual)});
            return false;
        }

        rasm::ReadbackImage diff;
        const Comparison comparison = compareImages(actual, expected, test.threshold, diff);
        const double total = static_cast<double>(actual.width) * actual.height;
        const double share = comparison.sizeMatches ? static_cast<double>(comparison.differing) / total : 1.0;
        const bool pass = comparison.sizeMatches && share <= test.fraction;
        char differing[32];
        std::snprintf(differing, sizeof(differing), "%.4f%%", share * 100.0);
        std::printf("%-14s %-6s %10s %8u %8.1f%s\n", test.name, pass ? "pass" : "FAIL", differing, comparison.maxDiff, seconds, comparison.sizeMatches ? "" : "  (the sizes differ)");
        if (!pass)
            writeFailure(options, test, actual, &expected, comparison.sizeMatches ? &diff : nullptr);
        rendered.push_back({test.name, std::move(actual)});
        return pass && !allocated && pickFailures == 0;
    }

    // One engine for every test, so the pipelines are compiled once, from source: no disk caches, so every run takes the
    // same path. Counts the tests that ran and failed.
    bool runTests(const Options &options, uint32_t &run, uint32_t &failed)
    {
        const rasm::EngineConfig config = {
            .appName = "rasm_image_tests",
            .platform = rasm::PlatformType::HEADLESS,
            .windowWidth = WIDTH,
            .windowHeight = HEIGHT,
            .enableValidation = true,
            .preferredBackend = rasm::Backend::VULKAN,
            .assetRoot = RASM_IMAGE_TESTS_ASSET_ROOT,
            .shaderDir = RASM_IMAGE_TESTS_SHADER_DIR,
            .cacheDir = ""};
        rasm::Engine engine(config);
        if (!engine.running())
            return false;
        const rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                                     .perspective{.fovY = 40.0f, .aspect = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT), .nearZ = 0.1f}},
                                                              rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
        const Assets assets = {.sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr"), .cube = createCube(engine), .sponza = options.sponza};
        if (!camera.isValid() || !assets.sunset.isValid() || !assets.cube.isValid())
            return false;

        std::vector<BuiltContent> built;
        std::vector<Rendered> rendered;
        for (size_t i = 0; i < std::size(TESTS); ++i)
        {
            const ImageTest &test = TESTS[i];
            if (!isRun(options, test))
                continue;
            ++run;
            failed += runTest(engine, assets, camera, options, test, built, rendered) ? 0 : 1;
            std::fflush(stdout);
            // A scene no later test draws goes, so the tests stay under MAX_SCENES; its content (a tile streamer's
            // tiles too) goes before it.
            const bool needed = std::any_of(std::begin(TESTS) + static_cast<std::ptrdiff_t>(i) + 1, std::end(TESTS), [&](const ImageTest &later)
                                            { return isRun(options, later) && later.build == test.build && later.origin == test.origin; });
            for (auto entry = built.begin(); !needed && entry != built.end(); ++entry)
            {
                if (entry->build != test.build || entry->origin != test.origin)
                    continue;
                const rasm::SceneHandle scene = entry->content.scene;
                built.erase(entry);
                engine.destroyScene(scene);
                break;
            }
        }
        return true;
    }
}

int main(int argc, char **argv)
{
    Options options;
    if (!parseOptions(argc, argv, options))
        return 2;
    if (options.list)
    {
        for (const ImageTest &test : TESTS)
            std::printf("%s\n", test.name);
        return 0;
    }

    uint32_t run = 0;
    uint32_t failed = 0;
    if (!allocationCountingWorks())
        std::printf("The steady frames' allocations are not counted on this platform or build.\n");
    std::printf("%-14s %-6s %10s %8s %8s\n", "test", "result", "differing", "max diff", "seconds");
    const bool any = std::any_of(std::begin(TESTS), std::end(TESTS), [&](const ImageTest &test)
                                 { return isRun(options, test); });
    if (any && !runTests(options, run, failed))
        return 2;
    std::printf("%u of %u tests passed.%s\n", run - failed, run, failed > 0 ? (" Failures are in " + options.outDir + ".").c_str() : "");
    return failed > 0 ? 1 : 0;
}
