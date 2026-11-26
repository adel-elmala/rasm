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
#include <unordered_map>
#include <vector>

// Order-independent transparency: dense see-through data drawn with weighted blended transparency
// (BlendMethod::WEIGHTED), which needs no order, so nothing pops while the camera moves. A scalar field of a few
// blobs, 4 000 km from the world's origin, is shown three ways side by side:
//   - three nested iso-surfaces of the field (marching tetrahedra), each double-sided and see-through,
//   - 48 volume slices of the field in ONE mesh, alpha by the field: a draw that sorting cannot order inside itself,
//   - a see-through CAD assembly: a housing, inner boxes and bearings around an opaque shaft,
// and see-through arrows (a glyph chunk with a BLEND material, always weighted) along the field's gradient. The left
// view is perspective with TAA, the right one an orthographic front view of the assembly with 4x MSAA.
// - Drag to orbit (left view) or pan (right view), wheel to zoom.
// A headless run (or --check) orbits the left view and picks the right view's centre three times: with minAlpha 0 the
// housing (alpha 0.2) is hit, with 0.3 the inner box (alpha 0.35), with 0.9 the opaque shaft. A failed check exits with 1.
//   --sorted        the meshes sorted (BlendMethod::SORTED) instead of weighted: compare the slices from behind
//   --additive      the slices add their light (BlendMethod::ADDITIVE): a density, X-ray view
//   --no-glyphs     no arrows
//   --glyphs N      about N arrows (8 000 by default)
//   --scale S       the whole scene S times larger (1 by default): the weights do not change with the scale
//   --still         no scripted orbit
//   --behind        the left view starts behind the scene, where sorted slices blend in the wrong order
//   --swap-aa       the left view with 4x MSAA, the right one with TAA
//   --at-origin     the scene at the world's origin instead
//   --range-test    a see-through ball flies in and out of both views, behind the scene (implies --still)
//   --probe-sorted  that ball sorted, so the weights' span does not see it: the run to compare with
//   --dump A B P    writes frames A to B as P_<frame>.png (with --headless)
//   --bench         the GPU time per pass (exampleBench.h), at 1920 x 1080

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr double FIELD_SIZE = 16.0; // metres: the iso-surfaces' box, and the slices' box beside it
    constexpr uint32_t ISO_CELLS = 48;  // the marching grid's cells along each axis
    constexpr uint32_t SLICES = 48;
    constexpr uint32_t SLICE_CELLS = 64;
    constexpr uint32_t PICK_FRAME = 24;
    constexpr uint32_t CHECK_DEADLINE = 60;
    constexpr uint32_t PICKS = 3;
    constexpr glm::dvec3 FAR_ORIGIN = glm::dvec3(2.9e6, 140.0, -2.8e6);

    enum ViewIndex : uint32_t
    {
        PERSPECTIVE,
        FRONT,
        VIEW_COUNT,
    };

    constexpr rasm::ViewRect RECTS[VIEW_COUNT] = {{.x = 0.0f, .y = 0.0f, .width = 0.5f, .height = 1.0f}, {.x = 0.5f, .y = 0.0f, .width = 0.5f, .height = 1.0f}};

    struct ViewCamera
    {
        rasm::CameraHandle camera;
        rasm::CameraRig rig;
        rasm::CameraInputState input;
        rasm::CameraProjection projection;
    };

    struct Options
    {
        bool sorted = false;
        bool additive = false;
        bool glyphs = true;
        uint32_t glyphCount = 8000;
        double scale = 1.0;
        bool still = false;
        bool swapAa = false;
        bool atOrigin = false;
        bool rangeTest = false;
        bool behind = false;
        bool probeSorted = false;
        uint32_t dumpFirst = 0;
        uint32_t dumpLast = 0;
        const char *dumpPrefix = nullptr;
    };

    struct Blob
    {
        glm::dvec3 center; // in the field's unit box, -1 to 1
        double radius;
        double weight;
    };

    constexpr Blob BLOBS[] = {
        {{-0.35, -0.2, 0.1}, 0.42, 1.0},
        {{0.4, 0.15, -0.2}, 0.36, 0.9},
        {{0.05, 0.45, 0.35}, 0.3, 0.8},
        {{0.1, -0.5, -0.45}, 0.28, 0.7},
        {{-0.5, 0.4, -0.4}, 0.22, 0.6},
    };

    // A see-through material and the order its draws need.
    struct Style
    {
        glm::vec4 color;
        float roughness;
        bool unlit;
    };
}

namespace
{
    double field(const glm::dvec3 &p)
    {
        double sum = 0.0;
        for (const Blob &blob : BLOBS)
        {
            const glm::dvec3 d = p - blob.center;
            sum += blob.weight * std::exp(-glm::dot(d, d) / (2.0 * blob.radius * blob.radius));
        }
        return sum;
    }

    glm::dvec3 gradient(const glm::dvec3 &p)
    {
        glm::dvec3 sum(0.0);
        for (const Blob &blob : BLOBS)
        {
            const glm::dvec3 d = p - blob.center;
            const double r2 = blob.radius * blob.radius;
            sum += -d / r2 * blob.weight * std::exp(-glm::dot(d, d) / (2.0 * r2));
        }
        return sum;
    }

    // Viridis by five stops, in sRGB; the result is linear.
    glm::vec3 viridis(float t)
    {
        constexpr glm::vec3 STOPS[5] = {{0.267f, 0.005f, 0.329f}, {0.231f, 0.322f, 0.545f}, {0.129f, 0.569f, 0.549f}, {0.369f, 0.788f, 0.384f}, {0.992f, 0.906f, 0.145f}};
        const float x = std::clamp(t, 0.0f, 1.0f) * 4.0f;
        const auto i = std::min(static_cast<uint32_t>(x), 3u);
        const glm::vec3 srgb = glm::mix(STOPS[i], STOPS[i + 1], x - static_cast<float>(i));
        return glm::pow(srgb, glm::vec3(2.2f));
    }

    // One iso-surface of the field over the unit box, by marching tetrahedra: each cell is six tetrahedra around its
    // main diagonal, and each tetrahedron the level cuts gives one or two triangles. Vertices on the same grid edge are
    // shared, so the mesh is welded and simplifies into LODs. Normals point down the field, out of the blobs.
    rasm::MeshBuild buildIsoSurface(double level, double size, uint32_t &triangles)
    {
        constexpr uint32_t N = ISO_CELLS;
        constexpr uint32_t CORNERS[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
        constexpr uint32_t TETS[6][4] = {{0, 5, 1, 6}, {0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6}, {0, 7, 4, 6}, {0, 4, 5, 6}};
        auto gridPoint = [](uint32_t x, uint32_t y, uint32_t z)
        { return glm::dvec3(x, y, z) / static_cast<double>(N) * 2.0 - 1.0; };
        std::vector<double> values(static_cast<size_t>(N + 1) * (N + 1) * (N + 1));
        for (uint32_t z = 0; z <= N; ++z)
            for (uint32_t y = 0; y <= N; ++y)
                for (uint32_t x = 0; x <= N; ++x)
                    values[(static_cast<size_t>(z) * (N + 1) + y) * (N + 1) + x] = field(gridPoint(x, y, z));

        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::unordered_map<uint64_t, uint32_t> edges;
        auto edgeVertex = [&](uint32_t a, uint32_t b, double va, double vb, const glm::dvec3 &pa, const glm::dvec3 &pb)
        {
            const uint64_t key = a < b ? (static_cast<uint64_t>(a) << 32 | b) : (static_cast<uint64_t>(b) << 32 | a);
            const auto found = edges.find(key);
            if (found != edges.end())
                return found->second;
            const double t = (level - va) / (vb - va);
            const glm::dvec3 p = glm::mix(pa, pb, t);
            const glm::dvec3 g = gradient(p);
            rasm::Vertex vertex{};
            vertex.pos = glm::vec3(p * (0.5 * size));
            vertex.normal = glm::vec3(-g / std::max(glm::length(g), 1e-9));
            const auto index = static_cast<uint32_t>(vertices.size());
            vertices.push_back(vertex);
            edges.emplace(key, index);
            return index;
        };
        for (uint32_t z = 0; z < N; ++z)
        {
            for (uint32_t y = 0; y < N; ++y)
            {
                for (uint32_t x = 0; x < N; ++x)
                {
                    uint32_t ids[8];
                    double v[8];
                    glm::dvec3 p[8];
                    for (uint32_t c = 0; c < 8; ++c)
                    {
                        const uint32_t cx = x + CORNERS[c][0];
                        const uint32_t cy = y + CORNERS[c][1];
                        const uint32_t cz = z + CORNERS[c][2];
                        ids[c] = (cz * (N + 1) + cy) * (N + 1) + cx;
                        v[c] = values[ids[c]];
                        p[c] = gridPoint(cx, cy, cz);
                    }
                    for (const auto &tet : TETS)
                    {
                        uint32_t inside[4];
                        uint32_t outside[4];
                        uint32_t in = 0;
                        uint32_t out = 0;
                        for (uint32_t k : tet)
                        {
                            if (v[k] > level)
                                inside[in++] = k;
                            else
                                outside[out++] = k;
                        }
                        auto cut = [&](uint32_t a, uint32_t b)
                        { return edgeVertex(ids[a], ids[b], v[a], v[b], p[a], p[b]); };
                        if (in == 1 || in == 3)
                        {
                            const uint32_t lone = in == 1 ? inside[0] : outside[0];
                            const uint32_t *others = in == 1 ? outside : inside;
                            indices.insert(indices.end(), {cut(lone, others[0]), cut(lone, others[1]), cut(lone, others[2])});
                        }
                        else if (in == 2)
                        {
                            const uint32_t a = cut(inside[0], outside[0]);
                            const uint32_t b = cut(inside[0], outside[1]);
                            const uint32_t c = cut(inside[1], outside[1]);
                            const uint32_t d = cut(inside[1], outside[0]);
                            indices.insert(indices.end(), {a, b, c, a, c, d});
                        }
                    }
                }
            }
        }
        triangles += static_cast<uint32_t>(indices.size() / 3);
        return rasm::buildMesh(std::move(vertices), std::move(indices));
    }

    // SLICES planes across the box along z, each a grid with the field's color and an alpha that grows with it, all in
    // one mesh: the draw cannot be sorted inside itself.
    rasm::MeshBuild buildSlices(double size)
    {
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> colors;
        constexpr uint32_t G = SLICE_CELLS;
        for (uint32_t s = 0; s < SLICES; ++s)
        {
            const double z = (s + 0.5) / SLICES * 2.0 - 1.0;
            const auto first = static_cast<uint32_t>(vertices.size());
            for (uint32_t j = 0; j <= G; ++j)
            {
                for (uint32_t i = 0; i <= G; ++i)
                {
                    const glm::dvec3 p(static_cast<double>(i) / G * 2.0 - 1.0, static_cast<double>(j) / G * 2.0 - 1.0, z);
                    const auto value = static_cast<float>(field(p));
                    rasm::Vertex vertex{};
                    vertex.pos = glm::vec3(p * (0.5 * size));
                    vertex.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                    vertices.push_back(vertex);
                    const float alpha = std::clamp((value - 0.06f) * 0.5f, 0.0f, 0.4f);
                    colors.push_back(rasm::packVertexColor(glm::vec4(viridis(value), alpha)));
                }
            }
            for (uint32_t j = 0; j < G; ++j)
            {
                for (uint32_t i = 0; i < G; ++i)
                {
                    const uint32_t a = first + j * (G + 1) + i;
                    indices.insert(indices.end(), {a, a + 1, a + G + 2, a, a + G + 2, a + G + 1});
                }
            }
        }
        return rasm::buildMesh(std::move(vertices), std::move(indices), std::move(colors));
    }

    rasm::MaterialHandle makeMaterial(rasm::Engine &engine, const Style &style, rasm::BlendMethod method)
    {
        const rasm::MaterialHandle handle = engine.createMaterial(style.unlit ? rasm::MaterialType::UNLIT : rasm::MaterialType::PBR, {}, true);
        rasm::Material material = engine.getMaterial(handle);
        for (uint32_t i = 0; i < 4; ++i)
            material.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = style.color[static_cast<int>(i)];
        material.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = style.roughness;
        material.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = 0.0f;
        material.alphaMode = style.color.a < 1.0f ? rasm::AlphaMode::BLEND : rasm::AlphaMode::NONE;
        material.blendMethod = method;
        engine.updateMaterial(handle, material);
        return handle;
    }

    rasm::CameraProjection orthographic(float halfWidth, float aspect, float depth)
    {
        const float halfHeight = halfWidth / aspect;
        return {.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic{.left = -halfWidth, .right = halfWidth, .bottom = -halfHeight, .top = halfHeight, .nearZ = 0.0f, .farZ = depth}};
    }

    ViewCamera createViewCamera(rasm::Engine &engine, rasm::SceneHandle scene, const rasm::CameraProjection &projection, const glm::dvec3 &center, const glm::dvec3 &direction, double distance, float orbit)
    {
        ViewCamera view;
        view.projection = projection;
        const glm::dvec3 eye = center - direction * distance;
        const rasm::Transform transform{eye, glm::quatLookAt(glm::normalize(glm::vec3(center - eye)), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)};
        view.camera = engine.createCamera(projection, transform);
        engine.setCameraController(view.camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
        engine.addCameraToScene(scene, view.camera);
        view.rig = rasm::createCameraRig(
            rasm::CameraController{.type = rasm::CameraController::Type::TRACK_BALL, .trackBall = {.radius = static_cast<float>(distance), .center = center, .orbitSensitivity = orbit}, .easingSpeed = 0.0f},
            transform);
        return view;
    }

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

    Options parseOptions(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const bool value = i + 1 < argc;
            if (value && std::strcmp(argv[i], "--glyphs") == 0)
                options.glyphCount = static_cast<uint32_t>(std::clamp(std::strtoul(argv[++i], nullptr, 10), 1ul, 65536ul));
            else if (value && std::strcmp(argv[i], "--scale") == 0)
                options.scale = std::max(1e-3, std::strtod(argv[++i], nullptr));
            else if (i + 3 < argc && std::strcmp(argv[i], "--dump") == 0)
            {
                options.dumpFirst = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
                options.dumpLast = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
                options.dumpPrefix = argv[++i];
            }
            options.sorted = options.sorted || std::strcmp(argv[i], "--sorted") == 0;
            options.additive = options.additive || std::strcmp(argv[i], "--additive") == 0;
            options.glyphs = options.glyphs && std::strcmp(argv[i], "--no-glyphs") != 0;
            options.still = options.still || std::strcmp(argv[i], "--still") == 0;
            options.swapAa = options.swapAa || std::strcmp(argv[i], "--swap-aa") == 0;
            options.atOrigin = options.atOrigin || std::strcmp(argv[i], "--at-origin") == 0;
            options.rangeTest = options.rangeTest || std::strcmp(argv[i], "--range-test") == 0;
            options.behind = options.behind || std::strcmp(argv[i], "--behind") == 0;
            options.probeSorted = options.probeSorted || std::strcmp(argv[i], "--probe-sorted") == 0;
        }
        options.still = options.still || options.rangeTest;
        return options;
    }

    // Arrows on a grid over the iso-surfaces' box, down the gradient (toward the blobs), sized and colored by its length.
    rasm::MeshBuild buildArrows(rasm::MeshHandle shape, uint32_t count, double size, float &maxSlope)
    {
        const auto n = static_cast<uint32_t>(std::max(2.0, std::cbrt(static_cast<double>(count))));
        std::vector<glm::vec3> positions;
        std::vector<glm::quat> orientations;
        std::vector<glm::vec3> scales;
        std::vector<float> slopes;
        maxSlope = 0.0f;
        for (uint32_t z = 0; z < n; ++z)
            for (uint32_t y = 0; y < n; ++y)
                for (uint32_t x = 0; x < n; ++x)
                {
                    // Anywhere in its cell, the same every run: a regular lattice of small arrows lines up into stripes.
                    uint32_t hash = x * 73856093u ^ y * 19349663u ^ z * 83492791u;
                    glm::dvec3 jitter(0.0);
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        hash = hash * 1664525u + 1013904223u;
                        jitter[axis] = static_cast<double>(hash >> 8) / 16777216.0 - 0.5;
                    }
                    const glm::dvec3 p = (glm::dvec3(x, y, z) + 0.5 + jitter) / static_cast<double>(n) * 2.0 - 1.0;
                    const glm::dvec3 g = gradient(p);
                    const auto slope = static_cast<float>(glm::length(g));
                    maxSlope = std::max(maxSlope, slope);
                    positions.push_back(glm::vec3(p * (0.5 * size)));
                    orientations.push_back(rasm::getGlyphOrientation(glm::vec3(-g)));
                    slopes.push_back(slope);
                }
        const float cell = static_cast<float>(size / n);
        for (float slope : slopes)
            scales.push_back(glm::vec3(cell * (0.35f + 0.9f * slope / std::max(maxSlope, 1e-6f))));
        return rasm::buildGlyphChunk(
            {.shape = shape, .positions = positions.data(), .orientations = orientations.data(), .scales = scales.data(), .scalars = slopes.data(), .count = static_cast<uint32_t>(positions.size())});
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    const Options options = parseOptions(argc, argv);
    // Sorted, the nested parts share a centre, so their order (and the pick's answer) is arbitrary: no check.
    bool check = args.headless && !options.rangeTest && !options.sorted && options.dumpPrefix == nullptr;
    for (int i = 1; i < argc; ++i)
        check = check || std::strcmp(argv[i], "--check") == 0;

    rasm::EngineConfig config = {
        .appName = "RASM: order-independent transparency",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};
    applyExampleArgs(args, config);
    ExampleBench bench{.on = hasBenchFlag(argc, argv), .warmup = getBenchWarmup(argc, argv)};
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

    const double s = options.scale;
    const glm::dvec3 origin = options.atOrigin ? glm::dvec3(0.0) : FAR_ORIGIN;
    const rasm::BlendMethod method = options.sorted ? rasm::BlendMethod::SORTED : rasm::BlendMethod::WEIGHTED;
    const rasm::SceneHandle scene = engine.createScene();
    auto place = [&](const char *name, rasm::MeshHandle mesh, rasm::MaterialHandle material, const glm::dvec3 &at, const glm::vec3 &scale)
    {
        const rasm::EntityHandle entity = engine.createEntity(name, mesh, material, rasm::Transform{origin + at * s, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), scale * static_cast<float>(s)});
        engine.addEntityToScene(scene, entity);
        return entity;
    };

    // The iso-surfaces, outer to inner, at the left; the slices in the middle; the assembly at the right.
    const glm::dvec3 isoCenter(-FIELD_SIZE * 1.2, 0.5 * FIELD_SIZE + 1.0, 0.0);
    const glm::dvec3 sliceCenter(0.0, 0.5 * FIELD_SIZE + 1.0, 0.0);
    const glm::dvec3 cadCenter(FIELD_SIZE * 1.2, 0.5 * FIELD_SIZE + 1.0, 0.0);
    const double levels[3] = {0.22, 0.45, 0.72};
    const Style isoStyles[3] = {{{0.25f, 0.55f, 0.95f, 0.18f}, 0.3f, false}, {{0.2f, 0.9f, 0.55f, 0.3f}, 0.35f, false}, {{0.98f, 0.55f, 0.15f, 0.5f}, 0.4f, false}};
    uint32_t isoTriangles = 0;
    for (uint32_t i = 0; i < 3; ++i)
    {
        rasm::MeshBuild build = buildIsoSurface(levels[i], FIELD_SIZE, isoTriangles);
        place("iso-surface", engine.createMesh(std::move(build)), makeMaterial(engine, isoStyles[i], method), isoCenter, glm::vec3(1.0f));
    }
    const rasm::MaterialHandle sliceMaterial = makeMaterial(engine, {{1.0f, 1.0f, 1.0f, 0.999f}, 1.0f, true}, options.additive ? rasm::BlendMethod::ADDITIVE : method);
    place("slices", engine.createMesh(buildSlices(FIELD_SIZE)), sliceMaterial, sliceCenter, glm::vec3(1.0f));

    const rasm::MeshHandle cube = engine.createMesh(rasm::buildGlyphShape(rasm::GlyphShape::CUBE));
    const rasm::MeshHandle ball = engine.createMesh(rasm::buildGlyphShape(rasm::GlyphShape::SPHERE));
    const rasm::MaterialHandle housing = makeMaterial(engine, {{0.6f, 0.75f, 0.9f, 0.2f}, 0.2f, false}, method);
    const rasm::MaterialHandle inner = makeMaterial(engine, {{0.95f, 0.35f, 0.3f, 0.35f}, 0.35f, false}, method);
    const rasm::MaterialHandle bearing = makeMaterial(engine, {{0.95f, 0.85f, 0.3f, 0.55f}, 0.25f, false}, method);
    const rasm::MaterialHandle steel = makeMaterial(engine, {{0.55f, 0.56f, 0.6f, 1.0f}, 0.3f, false}, method);
    const rasm::EntityHandle housingEntity = place("housing", cube, housing, cadCenter, glm::vec3(14.0f, 10.0f, 10.0f));
    const rasm::EntityHandle innerEntity = place("inner box", cube, inner, cadCenter, glm::vec3(9.0f, 6.0f, 6.0f));
    place("inner box", cube, inner, cadCenter + glm::dvec3(0.0, 0.0, 0.0), glm::vec3(4.0f, 8.0f, 4.0f));
    for (int i = -1; i <= 1; i += 2)
        for (int j = -1; j <= 1; j += 2)
            place("bearing", ball, bearing, cadCenter + glm::dvec3(5.0 * i, 3.0 * j, 3.0), glm::vec3(2.5f));
    const rasm::EntityHandle shaftEntity = place("shaft", cube, steel, cadCenter, glm::vec3(18.0f, 1.2f, 1.2f));

    // The ground and a solid core inside the iso-surfaces, which the see-through draws are depth tested against.
    const rasm::MaterialHandle ground = makeMaterial(engine, {{0.12f, 0.13f, 0.15f, 1.0f}, 0.8f, false}, method);
    place("ground", cube, ground, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(72.0f, 1.0f, 30.0f));
    place("core", ball, steel, isoCenter + glm::dvec3(BLOBS[0].center) * (0.5 * FIELD_SIZE), glm::vec3(1.6f));

    float maxSlope = 0.0f;
    if (options.glyphs)
    {
        const rasm::MeshHandle arrow = engine.createMesh(rasm::buildGlyphShape(rasm::GlyphShape::ARROW));
        rasm::MeshBuild arrows = buildArrows(arrow, options.glyphCount, FIELD_SIZE, maxSlope);
        const rasm::MaterialHandle glyphMaterial = engine.createMaterial(rasm::MaterialType::GLYPHS);
        rasm::Material glyphStyle = engine.getMaterial(glyphMaterial);
        glyphStyle.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + 3] = 0.45f;
        glyphStyle.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.45f;
        glyphStyle.alphaMode = rasm::AlphaMode::BLEND;
        glyphStyle.glyphs = {.colorMode = rasm::PointColorMode::SCALAR, .colormap = engine.createColormap(rasm::ColormapPreset::MAGMA), .range = {.min = 0.0f, .max = maxSlope}};
        engine.updateMaterial(glyphMaterial, glyphStyle);
        place("arrows", engine.createMesh(std::move(arrows)), glyphMaterial, isoCenter, glm::vec3(1.0f));
    }
    // The range test's ball: see-through, far behind the scene and above the parts as both views see them, so it never
    // covers them on screen, and it flies in and out of both views: their weights' range grows and shrinks.
    rasm::EntityHandle probe;
    if (options.rangeTest)
        probe = place("probe", ball, makeMaterial(engine, {{0.9f, 0.9f, 0.9f, 0.4f}, 0.5f, false}, options.probeSorted ? rasm::BlendMethod::SORTED : method), glm::dvec3(-200.0, 19.5, -160.0),
                      glm::vec3(5.0f));

    const rasm::EnvironmentHandle sky = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
    engine.setSceneEnvironment(scene, {.environment = sky, .intensity = 0.6f});
    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.95f, 0.9f), glm::dvec3(0.0), glm::normalize(glm::vec3(-0.4f, -0.8f, -0.5f)), 3.0f, 0.0f);
    engine.addLightToScene(scene, sun);

    std::printf("oit: 3 iso-surfaces (%u triangles), %u slices of %u x %u cells in one mesh, a see-through assembly, %s arrows; meshes %s%s\n", isoTriangles, SLICES, SLICE_CELLS, SLICE_CELLS,
                options.glyphs ? std::to_string(options.glyphCount).c_str() : "no", options.sorted ? "sorted" : "weighted", options.additive ? ", slices additive" : "");

    const float aspect = 0.5f * static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight);
    const glm::dvec3 center = origin + glm::dvec3(0.0, 0.5 * FIELD_SIZE, 0.0) * s;
    ViewCamera views[VIEW_COUNT] = {
        createViewCamera(engine, scene, {.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 50.0f, .aspect = aspect, .nearZ = static_cast<float>(0.1 * s)}}, center,
                         glm::normalize(glm::dvec3(-0.3, -0.38, options.behind ? 0.87 : -0.87)), 64.0 * s, 1.0f),
        createViewCamera(engine, scene, orthographic(static_cast<float>(12.0 * s), aspect, static_cast<float>(400.0 * s)), origin + cadCenter * s, glm::dvec3(0.0, 0.0, -1.0), 100.0 * s, 0.0f),
    };
    views[FRONT].input.buttons.left = rasm::CameraDrag::PAN;
    rasm::Grid floor;
    floor.enabled = true;
    floor.origin = origin;
    floor.cellSize = 2.0 * s;
    floor.extent = glm::vec2(static_cast<float>(80.0 * s), static_cast<float>(36.0 * s));
    floor.fadeDistance = static_cast<float>(300.0 * s);
    engine.setCameraGrid(views[PERSPECTIVE].camera, floor);
    rasm::View list[VIEW_COUNT];
    const rasm::AntiAliasing perspectiveAa = options.swapAa ? rasm::AntiAliasing::MSAA_4X : rasm::AntiAliasing::TAA;
    const rasm::AntiAliasing frontAa = options.swapAa ? rasm::AntiAliasing::TAA : rasm::AntiAliasing::MSAA_4X;
    for (uint32_t i = 0; i < VIEW_COUNT; ++i)
        list[i] = {.scene = scene, .camera = views[i].camera, .output = {.rect = RECTS[i]}, .antiAliasing = i == PERSPECTIVE ? perspectiveAa : frontAa};

    if (args.waitForLoads || check)
        engine.waitForLoads();

    const rasm::Window &window = engine.getMainWindow();
    const float quarterWidth = 0.25f * static_cast<float>(window.width);
    const float halfHeight = 0.5f * static_cast<float>(window.height);
    // The right view's centre: the housing in front, the inner box, then the shaft.
    const float minAlphas[PICKS] = {0.0f, 0.3f, 0.9f};
    const rasm::EntityHandle expected[PICKS] = {housingEntity, innerEntity, shaftEntity};
    const char *names[PICKS] = {"housing", "inner box", "shaft"};
    rasm::PickHandle picks[PICKS];
    uint32_t failures = 0;
    uint32_t checksLeft = check ? PICKS : 0;
    uint32_t frame = 0;
    while (exampleRunning(engine, args))
    {
        ++frame;
        if ((check || bench.on || args.headless) && !options.still && frame > 2)
        {
            if (frame == 3)
                engine.addInputEvent({.type = rasm::InputEventType::MOUSE_BUTTON, .down = true, .x = quarterWidth, .y = halfHeight});
            engine.addInputEvent({.type = rasm::InputEventType::MOUSE_MOVE, .x = quarterWidth + 2.0f * static_cast<float>(frame - 2), .y = halfHeight});
        }
        if (probe.isValid())
        {
            // Across the views' edges and back, every 4 s.
            const double t = static_cast<double>(frame) / 240.0 * 6.283185307179586;
            engine.updateTransform(probe, rasm::Transform{origin + glm::dvec3(-60.0 + 110.0 * std::sin(t), 19.5, -160.0) * s, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(static_cast<float>(5.0 * s))});
        }
        engine.pollEvents();
        for (ViewCamera &view : views)
            updateViewCamera(engine, view, RECTS[&view - views]);
        engine.setMouseGrab(views[PERSPECTIVE].input.dragActive || views[FRONT].input.dragActive);
        if (check && frame == PICK_FRAME)
        {
            const rasm::ViewPixels pixels = rasm::getViewPixels(RECTS[FRONT], window.pixelWidth, window.pixelHeight);
            for (uint32_t i = 0; i < PICKS; ++i)
                picks[i] = engine.pick(list[FRONT], {.rect = {.x = pixels.width / 2, .y = pixels.height / 2, .width = 1, .height = 1}, .minAlpha = minAlphas[i], .skip = rasm::PICK_SKIP_GLYPHS});
        }
        const bool dump = options.dumpPrefix != nullptr && frame >= options.dumpFirst && frame <= options.dumpLast;
        const rasm::ReadbackHandle readback = dump ? engine.readFrame() : rasm::ReadbackHandle{};
        engine.render(list, VIEW_COUNT);
        addBenchFrame(bench, engine.getFrameStats());
        rasm::ReadbackImage image;
        if (dump && engine.takeReadback(readback, image))
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s_%04u.png", options.dumpPrefix, frame);
            rasm::writePng(path, image);
        }

        for (uint32_t i = 0; i < PICKS; ++i)
        {
            if (!picks[i].isValid() || !engine.isPickReady(picks[i]))
                continue;
            rasm::PickHit hit;
            uint32_t hitCount = 0;
            const bool ok = engine.takePick(picks[i], &hit, 1, hitCount) && hitCount == 1 && hit.entity == expected[i];
            std::printf("oit check pick minAlpha %.1f %s: %s\n", static_cast<double>(minAlphas[i]), ok ? "ok  " : "FAIL", hitCount == 0 ? "no hit" : (ok ? names[i] : "another entity"));
            failures += ok ? 0 : 1;
            picks[i] = {};
            --checksLeft;
        }
        if (check && frame == CHECK_DEADLINE && checksLeft > 0)
        {
            std::printf("oit check: %u picks got no answer by frame %u\n", checksLeft, CHECK_DEADLINE);
            failures += checksLeft;
            checksLeft = 0;
        }
    }
    printBench(bench, config, engine.getRenderContext().hasMeshShaders());
    if (check && checksLeft > 0)
    {
        std::printf("oit check: %u picks got no answer before the run ended\n", checksLeft);
        failures += checksLeft;
    }
    if (failures > 0)
        return 1;
    return exampleExit(args);
}
