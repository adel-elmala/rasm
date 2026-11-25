#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"
#include "exampleBench.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// CAD views: a bracket in four views, as a CAD program shows a part. A perspective view (TAA) that orbits, and three
// orthographic ones (top, front, side; 4x MSAA) that pan and zoom. Each view's camera has a reference grid of its own
// (Renderer::setCameraGrid) in the plane behind the part, with the world's axes in red (X), green (Y) and blue (Z);
// dimension lines and their labels are vector features, a few pixels wide in every view, drawn as overlays so the part
// never hides a dimension.
// - Perspective view: drag to orbit (a turntable around +Y), right or middle drag (or Shift + drag) to pan, wheel to
//   zoom. A drag grabs the pointer (Renderer::setMouseGrab), so it goes on past the window's edge.
// - Orthographic views: drag to pan (each view's CameraButtons map the left button to PAN), wheel to zoom at the
//   pointer, which stays on the same point of the part.
// A headless run (or --check) scripts an orbit, a zoom in the top view and a pan in the front view, then checks that the
// zoom kept the point under the pointer, and picks the part in two orthographic views. A failed check exits with 1.
//   --far      the part, its grids and its cameras 6 000 km from the world's origin
//   --no-grid  without the grids (with --bench: the grids' cost is the Transparent pass's difference)
//   --bench    the GPU time per pass (exampleBench.h), at 1920 x 1080

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr float ORTHO_HALF_WIDTH = 6.4f;
    constexpr uint32_t ZOOM_FRAME = 20;
    constexpr uint32_t PAN_FRAME = 24;
    constexpr uint32_t PICK_FRAME = 32;
    constexpr uint32_t CHECK_DEADLINE = 60;
    constexpr uint32_t MAX_HITS = 16;
    constexpr glm::dvec3 FAR_ORIGIN = glm::dvec3(4.1e6, 320.0, -4.7e6);
    constexpr glm::vec4 Y_AXIS_COLOR = glm::vec4(0.15f, 0.7f, 0.15f, 1.0f);

    enum ViewIndex : uint32_t
    {
        PERSPECTIVE,
        TOP,
        FRONT,
        SIDE,
        VIEW_COUNT,
    };

    constexpr rasm::ViewRect RECTS[VIEW_COUNT] = {{.x = 0.0f, .y = 0.0f, .width = 0.5f, .height = 0.5f},
                                                  {.x = 0.5f, .y = 0.0f, .width = 0.5f, .height = 0.5f},
                                                  {.x = 0.0f, .y = 0.5f, .width = 0.5f, .height = 0.5f},
                                                  {.x = 0.5f, .y = 0.5f, .width = 0.5f, .height = 0.5f}};

    // A view's camera with its own rig, input and projection, which the rig's zoom changes.
    struct ViewCamera
    {
        rasm::CameraHandle camera;
        rasm::CameraRig rig;
        rasm::CameraInputState input;
        rasm::CameraProjection projection;
    };

    struct Part
    {
        rasm::SceneHandle scene;
        rasm::EntityHandle base;
        rasm::EntityHandle beam;
        rasm::EntityHandle upright;
        glm::dvec3 beamTop;
        glm::dvec3 uprightSide;
    };
}

namespace
{
    rasm::MeshHandle createCube(rasm::Engine &engine)
    {
        const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const glm::vec3 &n : normals)
        {
            const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 v = glm::cross(n, u);
            const glm::vec3 corners[4] = {n - u - v, n + u - v, n + u + v, n - u + v};
            const auto first = static_cast<uint32_t>(vertices.size());
            for (const glm::vec3 &corner : corners)
                vertices.push_back({.pos = corner * 0.5f, .normal = n, .tangent = glm::vec4(0.0f)});
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(first + index);
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec3 &color, float metallic, float roughness)
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

    rasm::MaterialHandle createVectorMaterial(rasm::Engine &engine, const rasm::VectorMaterial &style)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::VECTOR);
        rasm::Material values = engine.getMaterial(material);
        values.vector = style;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::EntityHandle addBox(rasm::Engine &engine, Part &part, rasm::MeshHandle cube, rasm::MaterialHandle material, const glm::dvec3 &center, const glm::vec3 &size)
    {
        const rasm::EntityHandle entity = engine.createEntity("box", cube, material, rasm::Transform{center, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), size});
        engine.addEntityToScene(part.scene, entity);
        return entity;
    }

    // A dimension: a line between two points, a tick at each end across it, and its length as a label above its middle.
    void addDimension(rasm::VectorGeometry &lines, rasm::VectorGeometry &text, const rasm::FontAtlas &atlas, const glm::dvec3 &origin, const glm::dvec3 &a, const glm::dvec3 &b,
                      const glm::dvec3 &across, std::vector<std::string> &names)
    {
        const glm::dvec3 tick = 0.25 * across;
        const glm::dvec3 points[6] = {a, b, a - tick, a + tick, b - tick, b + tick};
        const uint32_t sizes[3] = {2, 2, 2};
        rasm::addVectorLines(lines, origin, points, sizes, 3);
        char length[32];
        std::snprintf(length, sizeof(length), "%.2f m", glm::length(b - a));
        names.emplace_back(length);
        const rasm::LabelDesc label = {.position = 0.5 * (a + b) + 0.4 * across, .text = names.back().c_str(), .priority = 1.0f, .anchor = rasm::LabelAnchor::BOTTOM};
        rasm::addVectorLabels(text, origin, atlas, &label, 1);
    }

    // The bracket: a base plate, two uprights and a beam across their tops, with its dimensions.
    bool buildPart(rasm::Engine &engine, Part &part, const glm::dvec3 &origin)
    {
        part.scene = engine.createScene();
        const rasm::MeshHandle cube = createCube(engine);
        const rasm::MaterialHandle steel = createColorMaterial(engine, glm::vec3(0.62f, 0.64f, 0.68f), 1.0f, 0.35f);
        const rasm::MaterialHandle anodized = createColorMaterial(engine, glm::vec3(0.12f, 0.32f, 0.6f), 0.6f, 0.45f);
        part.base = addBox(engine, part, cube, steel, origin + glm::dvec3(0.0, 0.2, 0.0), glm::vec3(6.0f, 0.4f, 3.0f));
        part.upright = addBox(engine, part, cube, anodized, origin + glm::dvec3(2.6, 1.9, 0.0), glm::vec3(0.4f, 3.0f, 2.0f));
        addBox(engine, part, cube, anodized, origin + glm::dvec3(-2.6, 1.9, 0.0), glm::vec3(0.4f, 3.0f, 2.0f));
        part.beam = addBox(engine, part, cube, steel, origin + glm::dvec3(0.0, 3.6, 0.0), glm::vec3(5.6f, 0.4f, 0.8f));
        for (int i = 0; i < 4; ++i)
            addBox(engine, part, cube, anodized, origin + glm::dvec3(i < 2 ? -2.0 : 2.0, 0.5, (i % 2) == 0 ? -1.0 : 1.0), glm::vec3(0.3f, 0.2f, 0.3f));
        part.beamTop = origin + glm::dvec3(0.8, 3.8, 0.1);
        part.uprightSide = origin + glm::dvec3(2.8, 1.4, 0.4);

        rasm::FontAtlas atlas;
        if (!engine.loadFont("fonts/Roboto-Medium.ttf", atlas))
            return false;
        rasm::VectorGeometry lines;
        rasm::VectorGeometry text;
        std::vector<std::string> names;
        names.reserve(3);
        addDimension(lines, text, atlas, origin, origin + glm::dvec3(-3.0, 4.6, 0.0), origin + glm::dvec3(3.0, 4.6, 0.0), glm::dvec3(0.0, 1.0, 0.0), names);
        addDimension(lines, text, atlas, origin, origin + glm::dvec3(3.6, 0.0, 1.5), origin + glm::dvec3(3.6, 3.8, 1.5), glm::dvec3(1.0, 0.0, 0.0), names);
        addDimension(lines, text, atlas, origin, origin + glm::dvec3(-3.0, 0.0, 2.2), origin + glm::dvec3(3.0, 0.0, 2.2), glm::dvec3(0.0, 0.0, 1.0), names);
        const rasm::MaterialHandle lineStyle = createVectorMaterial(engine, {.color = glm::vec4(0.95f, 0.55f, 0.1f, 1.0f), .width = 1.5f, .drawOrder = 10, .depth = rasm::VectorDepth::OVERLAY});
        const rasm::MaterialHandle textStyle = createVectorMaterial(engine, {.color = glm::vec4(0.95f, 0.6f, 0.15f, 1.0f), .width = 15.0f, .outlineColor = glm::vec4(0.02f, 0.02f, 0.03f, 1.0f), .outlineWidth = 3.0f, .drawOrder = 20, .depth = rasm::VectorDepth::OVERLAY, .font = engine.createFont(atlas)});
        engine.addEntityToScene(part.scene, engine.createEntity("dimensions", engine.createMesh(rasm::buildVectorMesh(std::move(lines))), lineStyle, rasm::Transform{origin}));
        engine.addEntityToScene(part.scene, engine.createEntity("dimension labels", engine.createMesh(rasm::buildVectorMesh(std::move(text))), textStyle, rasm::Transform{origin}));
        return true;
    }

    // A grid in the plane through point with the given normal; turn takes the grid's +Y to it. Its axes are the world's,
    // so each line gets the color of the world axis it follows.
    rasm::Grid makeGrid(const glm::dvec3 &point, const glm::quat &turn, const glm::vec4 &xAxis, const glm::vec4 &zAxis, float fade)
    {
        return {.enabled = true, .origin = point, .orientation = turn, .cellSize = 0.1, .xAxisColor = xAxis, .zAxisColor = zAxis, .fadeDistance = fade};
    }

    rasm::Transform lookAt(const glm::dvec3 &eye, const glm::dvec3 &center, const glm::vec3 &up)
    {
        return rasm::Transform{eye, glm::quatLookAt(glm::normalize(glm::vec3(center - eye)), up), glm::vec3(1.0f)};
    }

    rasm::CameraProjection orthographic(float halfWidth, float aspect)
    {
        const float halfHeight = halfWidth / aspect;
        return {.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic{.left = -halfWidth, .right = halfWidth, .bottom = -halfHeight, .top = halfHeight, .nearZ = 0.1f, .farZ = 100.0f}};
    }

    // A trackball around center at distance, looking from direction (toward center), with up as the view's up.
    ViewCamera createViewCamera(rasm::Engine &engine, Part &part, const rasm::CameraProjection &projection, const glm::dvec3 &center, const glm::dvec3 &direction, double distance,
                                const glm::vec3 &up, float orbit)
    {
        ViewCamera view;
        view.projection = projection;
        const rasm::Transform transform = lookAt(center - direction * distance, center, up);
        view.camera = engine.createCamera(projection, transform);
        // The engine's own controller stays off: each view runs its rig on its own input below.
        engine.setCameraController(view.camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        engine.addCameraToScene(part.scene, view.camera);
        view.rig = rasm::createCameraRig(
            rasm::CameraController{.type = rasm::CameraController::Type::TRACK_BALL, .trackBall = {.radius = static_cast<float>(distance), .center = center, .orbitSensitivity = orbit}, .easingSpeed = 0.0f},
            transform);
        return view;
    }

    // One frame of a view's camera: its rig takes the frame's events in its own rect, and may change its projection. An
    // orthographic box first takes the view's aspect, as the view draws it, so the zoom and the pan map the pointer
    // right after the window changed shape (D49).
    void updateViewCamera(rasm::Engine &engine, ViewCamera &view, const rasm::ViewRect &rect)
    {
        const rasm::Window &window = engine.getMainWindow();
        const rasm::ViewPixels pixels = rasm::getViewPixels(rect, window.pixelWidth, window.pixelHeight);
        if (view.projection.type == rasm::CameraProjection::Type::ORTHOGRAPHIC && pixels.width > 0 && pixels.height > 0)
        {
            rasm::CameraProjection::Orthographic &box = view.projection.orthographic;
            const float halfHeight = 0.5f * (box.right - box.left) * static_cast<float>(pixels.height) / static_cast<float>(pixels.width);
            const float middle = 0.5f * (box.bottom + box.top);
            box.bottom = middle - halfHeight;
            box.top = middle + halfHeight;
        }
        const rasm::InputArea area = rasm::getInputArea(window, rect);
        const rasm::UiInputCapture capture = engine.getUiInputCapture();
        uint32_t count = 0;
        const rasm::InputEvent *events = engine.getInputEvents(count);
        for (uint32_t i = 0; i < count; ++i)
            rasm::addCameraInputEvent(view.input, events[i], area, capture);
        rasm::updateCameraRig(view.rig, view.projection, rasm::takeCameraInput(view.input, capture, engine.getDeltaTime()));
        engine.setCameraTransform(view.camera, view.rig.transform);
        engine.setCameraProjection(view.camera, view.projection);
    }

    // The point of the view plane under a pixel centre of an orthographic view, in doubles.
    glm::dvec3 getOrthographicPoint(rasm::Engine &engine, rasm::CameraHandle camera, const rasm::ViewPixels &view, double x, double y)
    {
        const glm::dmat4 viewProjection = glm::dmat4(engine.getProjectionMatrix(camera, static_cast<float>(view.width) / static_cast<float>(view.height))) * engine.getViewMatrix(camera);
        const glm::dvec4 point = glm::inverse(viewProjection) * glm::dvec4(x / view.width * 2.0 - 1.0, 1.0 - y / view.height * 2.0, 0.5, 1.0);
        return glm::dvec3(point) / point.w;
    }

    bool projectToPixel(rasm::Engine &engine, rasm::CameraHandle camera, const rasm::ViewPixels &view, const glm::dvec3 &world, uint32_t &x, uint32_t &y)
    {
        const glm::dmat4 viewProjection = glm::dmat4(engine.getProjectionMatrix(camera, static_cast<float>(view.width) / static_cast<float>(view.height))) * engine.getViewMatrix(camera);
        const glm::dvec4 clip = viewProjection * glm::dvec4(world, 1.0);
        const double px = (clip.x / clip.w * 0.5 + 0.5) * view.width;
        const double py = (0.5 - clip.y / clip.w * 0.5) * view.height;
        if (clip.w <= 0.0 || px < 0.0 || py < 0.0 || px >= view.width || py >= view.height)
            return false;
        x = static_cast<uint32_t>(px);
        y = static_cast<uint32_t>(py);
        return true;
    }

    // A pick's check: its nearest hit is the entity, within tolerance of the point along the view's depth only (the
    // pixel's centre is up to half a pixel off the point across it).
    bool checkPick(const char *name, const rasm::PickHit *hits, uint32_t count, rasm::EntityHandle entity, const glm::dvec3 &point, const glm::dvec3 &depthAxis)
    {
        const bool hit = count > 0 && hits[0].entity == entity;
        const double error = hit ? std::abs(glm::dot(hits[0].position - point, depthAxis)) : 0.0;
        const bool ok = hit && error <= 1e-3;
        std::printf("cad check %-22s %s: %s, off by %.6f m along the view\n", name, ok ? "ok  " : "FAIL", hit ? "the right entity" : (count == 0 ? "no hit" : "another entity"), error);
        return ok;
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    bool far = false;
    bool grids = true;
    bool check = args.headless;
    for (int i = 1; i < argc; ++i)
    {
        far = far || std::strcmp(argv[i], "--far") == 0;
        grids = grids && std::strcmp(argv[i], "--no-grid") != 0;
        check = check || std::strcmp(argv[i], "--check") == 0;
    }
    rasm::EngineConfig config = {
        .appName = "RASM: CAD views",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};
    applyExampleArgs(args, config);
    ExampleBench bench{.on = hasBenchFlag(argc, argv)};
    if (bench.on)
    {
        config.enableValidation = false;
        config.ui = false;
        config.showStats = false;
        config.windowWidth = 1920;
        config.windowHeight = 1080;
        check = false;
    }
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    const glm::dvec3 origin = far ? FAR_ORIGIN : glm::dvec3(0.0);
    Part part;
    if (!buildPart(engine, part, origin))
        return 1;
    const rasm::EnvironmentHandle sunset = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    engine.setSceneEnvironment(part.scene, {.environment = sunset, .intensity = 0.25f});
    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.95f, 0.9f), glm::dvec3(0.0), glm::normalize(glm::vec3(-0.4f, -0.8f, -0.5f)), 3.0f, 0.0f);
    engine.addLightToScene(part.scene, sun);

    const float aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight);
    const glm::dvec3 center = origin + glm::dvec3(0.0, 1.9, 0.0);
    ViewCamera views[VIEW_COUNT] = {
        createViewCamera(engine, part, {.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 40.0f, .aspect = aspect, .nearZ = 0.1f}}, center,
                         glm::normalize(glm::dvec3(-0.6, -0.45, -0.75)), 16.0, glm::vec3(0.0f, 1.0f, 0.0f), 1.0f),
        createViewCamera(engine, part, orthographic(ORTHO_HALF_WIDTH, aspect), center, glm::dvec3(0.0, -1.0, 0.0), 20.0, glm::vec3(0.0f, 0.0f, -1.0f), 0.0f),
        createViewCamera(engine, part, orthographic(ORTHO_HALF_WIDTH, aspect), center, glm::dvec3(0.0, 0.0, -1.0), 20.0, glm::vec3(0.0f, 1.0f, 0.0f), 0.0f),
        createViewCamera(engine, part, orthographic(ORTHO_HALF_WIDTH, aspect), center, glm::dvec3(-1.0, 0.0, 0.0), 20.0, glm::vec3(0.0f, 1.0f, 0.0f), 0.0f),
    };
    // A CAD convention: in a flat view the left button pans; the wheel zooms.
    for (uint32_t i = TOP; i < VIEW_COUNT; ++i)
        views[i].input.buttons.left = rasm::CameraDrag::PAN;
    if (grids)
    {
        const glm::vec4 red = rasm::Grid{}.xAxisColor;
        const glm::vec4 blue = rasm::Grid{}.zAxisColor;
        // The ground for the perspective and top views; the plane behind the part for the front and side views.
        engine.setCameraGrid(views[PERSPECTIVE].camera, makeGrid(origin, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), red, blue, 60.0f));
        engine.setCameraGrid(views[TOP].camera, makeGrid(origin, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), red, blue, 0.0f));
        engine.setCameraGrid(views[FRONT].camera, makeGrid(origin + glm::dvec3(0.0, 0.0, -3.0), glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)), red, Y_AXIS_COLOR, 0.0f));
        engine.setCameraGrid(views[SIDE].camera, makeGrid(origin + glm::dvec3(-4.0, 0.0, 0.0), glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f)), Y_AXIS_COLOR, blue, 0.0f));
    }
    rasm::View list[VIEW_COUNT];
    for (uint32_t i = 0; i < VIEW_COUNT; ++i)
        list[i] = {.scene = part.scene, .camera = views[i].camera, .output = {.rect = RECTS[i]}, .antiAliasing = i == PERSPECTIVE ? rasm::AntiAliasing::TAA : rasm::AntiAliasing::MSAA_4X};

    if (args.waitForLoads || check)
        engine.waitForLoads();

    // The script, in window points: an orbit in the perspective view, a zoom at a point of the top view, a pan of the
    // front view.
    const rasm::Window &window = engine.getMainWindow();
    const auto pointsWide = static_cast<float>(window.width);
    const auto pointsHigh = static_cast<float>(window.height);
    const float zoomX = 0.5f * pointsWide * (1.0f + 0.62f);
    const float zoomY = 0.5f * pointsHigh * 0.32f;
    glm::dvec3 pointBeforeZoom(0.0);
    uint32_t failures = 0;
    uint32_t checksLeft = check ? 4 : 0;
    glm::dvec3 centerBeforePan(0.0);
    rasm::PickHandle topPick;
    rasm::PickHandle sidePick;
    uint32_t frame = 0;
    while (exampleRunning(engine, args))
    {
        ++frame;
        if (check)
        {
            const float quarterWidth = 0.25f * pointsWide;
            const float quarterHeight = 0.25f * pointsHigh;
            if (frame == 4)
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = true, .x = quarterWidth, .y = quarterHeight});
            if (frame >= 5 && frame <= 12)
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = quarterWidth + 12.0f * static_cast<float>(frame - 4), .y = quarterHeight + 3.0f * static_cast<float>(frame - 4)});
            if (frame == 13)
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = false, .x = quarterWidth + 96.0f, .y = quarterHeight + 24.0f});
            if (frame == ZOOM_FRAME)
            {
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = zoomX, .y = zoomY});
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_WHEEL, .y = 4.0f});
            }
            if (frame == PAN_FRAME)
            {
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = quarterWidth, .y = 3.0f * quarterHeight});
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = true, .x = quarterWidth, .y = 3.0f * quarterHeight});
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = quarterWidth - 60.0f, .y = 3.0f * quarterHeight + 20.0f});
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = false, .x = quarterWidth - 60.0f, .y = 3.0f * quarterHeight + 20.0f});
            }
        }
        engine.pollEvents();
        const rasm::ViewPixels topPixels = rasm::getViewPixels(RECTS[TOP], window.pixelWidth, window.pixelHeight);
        const rasm::ViewPixels sidePixels = rasm::getViewPixels(RECTS[SIDE], window.pixelWidth, window.pixelHeight);
        // The zoom's pixel, in the top view's pixels.
        const double zoomPixelX = (zoomX - RECTS[TOP].x * pointsWide) * static_cast<double>(window.pixelWidth) / pointsWide;
        const double zoomPixelY = (zoomY - RECTS[TOP].y * pointsHigh) * static_cast<double>(window.pixelHeight) / pointsHigh;
        if (check && frame == ZOOM_FRAME)
            pointBeforeZoom = getOrthographicPoint(engine, views[TOP].camera, topPixels, zoomPixelX, zoomPixelY);
        if (check && frame == PAN_FRAME)
            centerBeforePan = views[FRONT].rig.controller.trackBall.center;
        bool dragging = false;
        for (uint32_t i = 0; i < VIEW_COUNT; ++i)
        {
            updateViewCamera(engine, views[i], RECTS[i]);
            dragging = dragging || views[i].input.dragActive;
        }
        // While a view's drag runs, the pointer is grabbed: it hides and the drag goes on past the window's edge.
        engine.setMouseGrab(dragging);
        if (check && frame == ZOOM_FRAME)
        {
            // Across the view: the point under the pointer is where it was, to a thousandth of a pixel.
            const glm::dvec3 after = getOrthographicPoint(engine, views[TOP].camera, topPixels, zoomPixelX, zoomPixelY);
            const double pixel = 2.0 * views[TOP].projection.orthographic.right / topPixels.width;
            const double error = glm::length(glm::dvec2(after.x - pointBeforeZoom.x, after.z - pointBeforeZoom.z));
            const bool zoomed = views[TOP].projection.orthographic.right < 0.7f * ORTHO_HALF_WIDTH;
            const bool ok = zoomed && error <= 1e-3 * pixel;
            std::printf("cad check %-22s %s: half width %.3f, the point moved %.2e m (a pixel is %.4f m)\n", "zoom at the pointer", ok ? "ok  " : "FAIL", views[TOP].projection.orthographic.right, error, pixel);
            failures += ok ? 0 : 1;
            --checksLeft;
        }
        if (check && frame == PAN_FRAME)
        {
            // The drag of 60 points left and 20 down within one frame: the scene follows it, so the center moves right
            // and up by as many world units as those points cover in the view.
            const rasm::CameraProjection::Orthographic &box = views[FRONT].projection.orthographic;
            const double perPoint = static_cast<double>(box.top - box.bottom) / (RECTS[FRONT].height * pointsHigh);
            const glm::dvec3 moved = views[FRONT].rig.controller.trackBall.center - centerBeforePan;
            const double error = glm::length(moved - glm::dvec3(60.0 * perPoint, 20.0 * perPoint, 0.0));
            const bool ok = error <= 1e-3 * perPoint;
            std::printf("cad check %-22s %s: the center moved (%.4f, %.4f) m, off by %.2e m\n", "pan with the pointer", ok ? "ok  " : "FAIL", moved.x, moved.y, error);
            failures += ok ? 0 : 1;
            --checksLeft;
        }
        if (check && frame == PICK_FRAME)
        {
            uint32_t x = 0;
            uint32_t y = 0;
            if (projectToPixel(engine, views[TOP].camera, topPixels, part.beamTop, x, y))
                topPick = engine.pick(list[TOP], {.rect = {.x = x, .y = y, .width = 1, .height = 1}});
            if (projectToPixel(engine, views[SIDE].camera, sidePixels, part.uprightSide, x, y))
                sidePick = engine.pick(list[SIDE], {.rect = {.x = x, .y = y, .width = 1, .height = 1}});
            if (!topPick.isValid() || !sidePick.isValid())
            {
                std::printf("cad check %-22s FAIL: a probe is outside its view\n", "picks");
                failures += 2;
                checksLeft -= 2;
            }
        }
        engine.render(list, VIEW_COUNT);
        addBenchFrame(bench, engine.getFrameStats());

        rasm::PickHit hits[MAX_HITS];
        uint32_t hitCount = 0;
        if (topPick.isValid() && engine.isPickReady(topPick))
        {
            failures += engine.takePick(topPick, hits, MAX_HITS, hitCount) && checkPick("top view: beam", hits, hitCount, part.beam, part.beamTop, glm::dvec3(0.0, 1.0, 0.0)) ? 0 : 1;
            topPick = {};
            --checksLeft;
        }
        if (sidePick.isValid() && engine.isPickReady(sidePick))
        {
            failures += engine.takePick(sidePick, hits, MAX_HITS, hitCount) && checkPick("side view: upright", hits, hitCount, part.upright, part.uprightSide, glm::dvec3(1.0, 0.0, 0.0)) ? 0 : 1;
            sidePick = {};
            --checksLeft;
        }
        if (check && frame == CHECK_DEADLINE && checksLeft > 0)
        {
            std::printf("cad check: %u checks got no answer by frame %u\n", checksLeft, CHECK_DEADLINE);
            failures += checksLeft;
            checksLeft = 0;
        }
    }
    printBench(bench, config, engine.getRenderContext().hasMeshShaders());
    if (check && checksLeft > 0)
    {
        std::printf("cad check: %u checks got no answer before the run ended\n", checksLeft);
        failures += checksLeft;
    }
    if (check)
        std::printf("cad checks: %s (%u failed)\n", failures == 0 ? "all passed" : "FAILED", failures);
    return failures > 0 ? 1 : exampleExit(args);
}
