#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"
#include "exampleExport.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Offscreen export at any size (Renderer::exportView). A scene with every kind of thing that depends on where a pixel
// is on screen: a glTF helmet with emissive parts and bloom, a ground, see-through panels (weighted blended), a road
// and markers sized in pixels with labels, a point cloud and a reference grid. The application's view keeps rendering
// while the export draws its tiles, one sample a frame, after it.
//
// By default it checks the tiles: it exports the view twice at 1920 x 1088, cut in tiles of 256 pixels and in one
// tile, and compares the two images (exit code 1 when they differ by more than the check allows). Flags, beside those
// of exampleArgs.h:
//   --export <file>        export to a file instead (.png: PNG8 unless --format, .exr: EXR_HALF unless --format)
//   --size WxH             the export's size (1920x1088 for the check)
//   --tile N               the tiles' size (256 for the check, else 2048)
//   --samples N            jittered frames per pixel (16)
//   --scale S              ExportDesc::pixelScale: lines, text, points and the grid S times wider (1)
//   --format F             png8, png16, exr-half or exr-float
//   --hdr                  the scene's linear HDR color instead of tone mapped (EXR only)
//   --transparent          no sky: alpha is what covers each pixel
//   --msaa                 the view's 4x MSAA in each sample too
//   --ortho                an orthographic camera
//   --far                  the scene 6,000 km from the world's origin
//   --check-dir <folder>   where the check writes export_tiled.png, export_single.png and export_diff.png (beside --out)

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr double FAR_DISTANCE = 6.0e6;
    // The check: tiles must match the single render but in a few pixels, by a few levels.
    constexpr uint32_t CHECK_THRESHOLD = 3;
    constexpr double CHECK_FRACTION = 0.001;

    struct Options
    {
        std::string path;
        uint32_t width = 1920;
        uint32_t height = 1088;
        uint32_t tile = 0;
        uint32_t samples = 16;
        float scale = 1.0f;
        rasm::ExportFormat format = rasm::ExportFormat::PNG8;
        bool formatGiven = false;
        bool hdr = false;
        bool transparent = false;
        bool msaa = false;
        bool ortho = false;
        bool far = false;
        std::string checkDir;
    };

    Options parseOptions(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const bool more = i + 1 < argc;
            if (std::strcmp(argv[i], "--export") == 0 && more)
                options.path = argv[++i];
            else if (std::strcmp(argv[i], "--size") == 0 && more)
            {
                char *end = nullptr;
                const unsigned long width = std::strtoul(argv[++i], &end, 10);
                const unsigned long height = *end == 'x' ? std::strtoul(end + 1, nullptr, 10) : 0;
                if (width > 0 && height > 0)
                {
                    options.width = static_cast<uint32_t>(width);
                    options.height = static_cast<uint32_t>(height);
                }
            }
            else if (std::strcmp(argv[i], "--tile") == 0 && more)
                options.tile = static_cast<uint32_t>(std::max(0, std::atoi(argv[++i])));
            else if (std::strcmp(argv[i], "--samples") == 0 && more)
                options.samples = static_cast<uint32_t>(std::max(1, std::atoi(argv[++i])));
            else if (std::strcmp(argv[i], "--scale") == 0 && more)
                options.scale = static_cast<float>(std::atof(argv[++i]));
            else if (std::strcmp(argv[i], "--format") == 0 && more)
            {
                const std::string format = argv[++i];
                options.formatGiven = true;
                options.format = rasm::ExportFormat::PNG8;
                if (format == "png16")
                    options.format = rasm::ExportFormat::PNG16;
                if (format == "exr-half")
                    options.format = rasm::ExportFormat::EXR_HALF;
                if (format == "exr-float")
                    options.format = rasm::ExportFormat::EXR_FLOAT;
            }
            else if (std::strcmp(argv[i], "--hdr") == 0)
                options.hdr = true;
            else if (std::strcmp(argv[i], "--transparent") == 0)
                options.transparent = true;
            else if (std::strcmp(argv[i], "--msaa") == 0)
                options.msaa = true;
            else if (std::strcmp(argv[i], "--ortho") == 0)
                options.ortho = true;
            else if (std::strcmp(argv[i], "--far") == 0)
                options.far = true;
            else if (std::strcmp(argv[i], "--check-dir") == 0 && more)
                options.checkDir = argv[++i];
        }
        if (!options.formatGiven && options.path.size() > 4 && options.path.substr(options.path.size() - 4) == ".exr")
            options.format = rasm::ExportFormat::EXR_HALF;
        return options;
    }

    rasm::MeshHandle createQuad(rasm::Engine &engine, float halfWidth, float halfHeight)
    {
        const glm::vec3 normal(0.0f, 0.0f, 1.0f);
        std::vector<rasm::Vertex> vertices;
        for (const glm::vec2 corner : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(-1.0f, 1.0f)})
        {
            const glm::vec2 uv = corner * 0.5f + 0.5f;
            vertices.push_back({.pos = glm::vec3(corner.x * halfWidth, corner.y * halfHeight, 0.0f), .normal = normal, .uv = {uv, uv, uv}, .tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)});
        }
        return engine.createMesh(std::move(vertices), {0, 1, 2, 0, 2, 3});
    }

    // A box of half size h: four vertices a face, so each face has its own normal.
    rasm::MeshHandle createBox(rasm::Engine &engine, glm::vec3 h)
    {
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> indices;
        for (int axis = 0; axis < 3; ++axis)
        {
            for (const float side : {-1.0f, 1.0f})
            {
                glm::vec3 normal(0.0f);
                normal[axis] = side;
                const glm::vec3 u = axis == 1 ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::vec3 v = glm::cross(normal, u);
                const auto first = static_cast<uint32_t>(vertices.size());
                for (const glm::vec2 corner : {glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(-1.0f, 1.0f)})
                {
                    const glm::vec3 position = (normal + corner.x * u + corner.y * v) * h;
                    vertices.push_back({.pos = position, .normal = normal, .uv = {corner, corner, corner}, .tangent = glm::vec4(u, 1.0f)});
                }
                indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
            }
        }
        return engine.createMesh(std::move(vertices), std::move(indices));
    }

    rasm::MaterialHandle createPbr(rasm::Engine &engine, glm::vec4 color, float roughness, glm::vec3 emissive = glm::vec3(0.0f), rasm::BlendMethod blend = rasm::BlendMethod::SORTED)
    {
        const rasm::MaterialHandle handle = engine.createMaterial(rasm::MaterialType::PBR, {}, color.a < 1.0f);
        rasm::Material material = engine.getMaterial(handle);
        for (uint32_t i = 0; i < 4; ++i)
            material.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[static_cast<int>(i)];
        material.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = 0.0f;
        material.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        for (uint32_t i = 0; i < 3; ++i)
            material.params[static_cast<uint32_t>(rasm::PbrParam::EMISSIVE_FACTOR) + i] = emissive[static_cast<int>(i)];
        material.params[static_cast<uint32_t>(rasm::PbrParam::EMISSIVE_STRENGTH)] = 1.0f;
        material.alphaMode = color.a < 1.0f ? rasm::AlphaMode::BLEND : rasm::AlphaMode::NONE;
        material.blendMethod = blend;
        engine.updateMaterial(handle, material);
        return handle;
    }

    rasm::MaterialHandle createVector(rasm::Engine &engine, const rasm::VectorMaterial &style)
    {
        const rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::VECTOR);
        rasm::Material values = engine.getMaterial(material);
        values.vector = style;
        engine.updateMaterial(material, values);
        return material;
    }

    void addEntity(rasm::Engine &engine, rasm::SceneHandle scene, const char *name, rasm::MeshHandle mesh, rasm::MaterialHandle material, const glm::dvec3 &position,
                   glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        engine.addEntityToScene(scene, engine.createEntity(name, mesh, material, rasm::Transform{position, rotation, glm::vec3(1.0f)}));
    }

    // The scene around origin: everything a tile border could cut.
    bool buildScene(rasm::Engine &engine, rasm::SceneHandle scene, const glm::dvec3 &origin)
    {
        const rasm::EnvironmentHandle sky = engine.loadEnvironment("environments/venice_sunset_2k.hdr");
        const rasm::MeshHandle helmet = engine.loadGLTF("models/gltf/DamagedHelmet/DamagedHelmet.gltf");
        if (!sky.isValid() || !helmet.isValid())
            return false;
        engine.setSceneEnvironment(scene, {.environment = sky, .intensity = 1.0f, .rotation = 1.15f});
        const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.93f, 0.85f), glm::dvec3(0.0), glm::normalize(glm::vec3(-0.5f, -0.8f, -0.3f)), 2.5f, 0.0f);
        engine.addLightToScene(scene, sun);

        addEntity(engine, scene, "helmet", helmet, rasm::MaterialHandle{}, origin + glm::dvec3(0.0, 0.2, 0.0));
        const rasm::MaterialHandle stone = createPbr(engine, glm::vec4(0.32f, 0.31f, 0.29f, 1.0f), 0.85f);
        addEntity(engine, scene, "ground", createBox(engine, glm::vec3(40.0f, 0.5f, 40.0f)), stone, origin + glm::dvec3(0.0, -1.5, 0.0));
        // Bright bars: their bloom crosses tile borders.
        const rasm::MeshHandle bar = createBox(engine, glm::vec3(0.06f, 1.2f, 0.06f));
        for (int i = 0; i < 7; ++i)
        {
            const float hue = static_cast<float>(i) / 7.0f;
            const glm::vec3 emissive = glm::vec3(0.5f + 0.5f * std::cos(6.283f * hue), 0.5f + 0.5f * std::cos(6.283f * (hue - 0.33f)), 0.5f + 0.5f * std::cos(6.283f * (hue - 0.67f))) * 12.0f;
            addEntity(engine, scene, "bar", bar, createPbr(engine, glm::vec4(0.1f, 0.1f, 0.1f, 1.0f), 0.5f, emissive), origin + glm::dvec3(-4.5 + 1.5 * i, 0.2, -3.0));
        }
        // See-through panels, weighted: their weights span the export's depths, the same in every tile.
        const rasm::MeshHandle panel = createQuad(engine, 1.1f, 0.8f);
        const glm::vec4 glass[3] = {glm::vec4(0.9f, 0.2f, 0.2f, 0.35f), glm::vec4(0.2f, 0.8f, 0.3f, 0.35f), glm::vec4(0.2f, 0.4f, 0.95f, 0.35f)};
        for (int i = 0; i < 3; ++i)
            addEntity(engine, scene, "glass", panel, createPbr(engine, glass[i], 0.1f, glm::vec3(0.0f), rasm::BlendMethod::WEIGHTED), origin + glm::dvec3(2.2 + 0.45 * i, 0.0, 1.2 - 0.6 * i),
                      glm::angleAxis(glm::radians(-35.0f), glm::vec3(0.0f, 1.0f, 0.0f)));

        // A road around the helmet and markers with labels, sized in pixels.
        rasm::FontAtlas atlas;
        if (!engine.loadFont("fonts/Roboto-Medium.ttf", atlas))
            return false;
        const rasm::FontHandle font = engine.createFont(atlas);
        std::vector<glm::dvec3> ring;
        for (int i = 0; i <= 96; ++i)
        {
            const double angle = 6.283185307 * i / 96.0;
            ring.push_back(origin + glm::dvec3(5.5 * std::cos(angle), -0.99, 4.0 * std::sin(angle)));
        }
        rasm::VectorGeometry road;
        const auto ringSize = static_cast<uint32_t>(ring.size());
        rasm::addVectorLines(road, origin, ring.data(), &ringSize, 1, {.join = rasm::LineJoin::ROUND, .cap = rasm::LineCap::ROUND});
        const rasm::MaterialHandle roadStyle = createVector(engine, {.color = glm::vec4(1.0f, 0.78f, 0.3f, 1.0f), .width = 5.0f, .outlineColor = glm::vec4(0.35f, 0.2f, 0.05f, 1.0f), .outlineWidth = 1.2f});
        addEntity(engine, scene, "road", engine.createMesh(rasm::buildVectorMesh(std::move(road))), roadStyle, origin);
        const char *names[8] = {"East gate", "Fountain", "North gate", "Old mill", "West gate", "Harbour", "South gate", "Market"};
        std::vector<glm::dvec3> markers;
        std::vector<rasm::LabelDesc> labels;
        for (int i = 0; i < 8; ++i)
        {
            const double angle = 6.283185307 * i / 8.0 + 0.2;
            markers.push_back(origin + glm::dvec3(5.5 * std::cos(angle), -0.99, 4.0 * std::sin(angle)));
            labels.push_back({.position = markers.back() + glm::dvec3(0.0, 0.35, 0.0), .text = names[i], .priority = 1.0f, .anchor = rasm::LabelAnchor::BOTTOM, .featureId = static_cast<uint32_t>(i)});
        }
        rasm::VectorGeometry points;
        rasm::addVectorPoints(points, origin, markers.data(), static_cast<uint32_t>(markers.size()));
        const rasm::MaterialHandle markerStyle = createVector(engine, {.color = glm::vec4(0.85f, 0.1f, 0.15f, 1.0f), .width = 12.0f, .outlineColor = glm::vec4(1.0f), .outlineWidth = 2.0f, .drawOrder = 10});
        addEntity(engine, scene, "markers", engine.createMesh(rasm::buildVectorMesh(std::move(points))), markerStyle, origin);
        rasm::VectorGeometry text;
        rasm::addVectorLabels(text, origin, atlas, labels.data(), static_cast<uint32_t>(labels.size()));
        const rasm::MaterialHandle textStyle =
            createVector(engine, {.color = glm::vec4(0.08f, 0.07f, 0.06f, 1.0f), .width = 18.0f, .outlineColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.95f), .outlineWidth = 2.5f, .drawOrder = 40, .font = font});
        addEntity(engine, scene, "labels", engine.createMesh(rasm::buildVectorMesh(std::move(text))), textStyle, origin);

        // A point cloud: a spiral of 40 000 small points, colored by height.
        std::vector<glm::vec3> positions;
        std::vector<uint32_t> colors;
        for (int i = 0; i < 40000; ++i)
        {
            const float t = static_cast<float>(i) / 40000.0f;
            const float angle = t * 90.0f;
            const float radius = 0.4f + 0.9f * t;
            positions.emplace_back(radius * std::cos(angle), 2.6f * t - 1.0f, radius * std::sin(angle));
            colors.push_back(rasm::packVertexColor(glm::vec4(0.2f + 0.8f * t, 0.9f - 0.6f * t, 1.0f - t, 1.0f)));
        }
        const rasm::MaterialHandle cloudStyle = engine.createMaterial(rasm::MaterialType::POINTS);
        rasm::Material cloud = engine.getMaterial(cloudStyle);
        cloud.points = {.size = 2.5f, .shape = rasm::PointShape::ROUND, .density = 0.0f};
        engine.updateMaterial(cloudStyle, cloud);
        addEntity(engine, scene, "cloud",
                  engine.createMesh(rasm::buildPointCloudChunk({.positions = positions.data(), .colors = colors.data(), .count = static_cast<uint32_t>(positions.size())})), cloudStyle,
                  origin + glm::dvec3(-3.2, 0.0, 1.0));
        return true;
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    const Options options = parseOptions(argc, argv);

    rasm::EngineConfig config = {
        .appName = "example_32",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .bloom = true,
        .antiAliasing = options.msaa ? rasm::AntiAliasing::MSAA_4X : rasm::AntiAliasing::TAA};
    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    const glm::dvec3 origin = options.far ? glm::dvec3(FAR_DISTANCE, 0.0, 0.0) : glm::dvec3(0.0);
    const rasm::SceneHandle scene = engine.createScene();
    if (!buildScene(engine, scene, origin))
        return 1;
    const float aspect = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT);
    const rasm::CameraProjection projection =
        options.ortho ? rasm::CameraProjection{.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic{.left = -7.0f * aspect, .right = 7.0f * aspect, .bottom = -7.0f, .top = 7.0f, .nearZ = 0.1f, .farZ = 200.0f}}
                      : rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective{.fovY = 50.0f, .aspect = aspect, .nearZ = 0.1f}};
    const glm::dvec3 eye = origin + glm::dvec3(2.5, 4.5, 11.0);
    const glm::quat look = glm::quatLookAt(glm::normalize(glm::vec3(origin - eye) + glm::vec3(0.0f, 0.5f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f));
    const rasm::CameraHandle camera = engine.createCamera(projection, rasm::Transform{eye, look, glm::vec3(1.0f)});
    engine.setCameraController(camera, rasm::CameraController{.type = rasm::CameraController::Type::NONE});
    engine.setCameraBloomStrength(camera, 0.06f);
    engine.setCameraGrid(camera, {.enabled = true, .origin = origin + glm::dvec3(0.0, -0.999, 0.0), .cellSize = 0.5, .extent = glm::vec2(9.0f), .fadeDistance = 40.0f});
    engine.addCameraToScene(scene, camera);
    engine.setAnimationTime(0.0);
    const rasm::View view{.scene = scene, .camera = camera};

    engine.waitForLoads();
    // A few frames, so the application's own view has its pipelines before the exports start.
    for (int i = 0; i < 8 && engine.running(); ++i)
        engine.render(&view, 1);

    rasm::ExportDesc desc{.width = options.width,
                          .height = options.height,
                          .path = options.path,
                          .format = options.format,
                          .samples = options.samples,
                          .pixelScale = options.scale,
                          .toneMap = !options.hdr,
                          .transparent = options.transparent,
                          .msaa = options.msaa,
                          .tileSize = options.tile};
    int status = 0;
    if (!options.path.empty())
    {
        rasm::ReadbackImage image;
        double seconds = 0.0;
        const bool done = runExport(engine, &view, 1, engine.exportView(view, desc), "export", {}, image, seconds);
        std::printf("export %u x %u to %s: %s in %.2f s\n", desc.width, desc.height, options.path.c_str(), done ? "written" : "FAILED", seconds);
        status = done ? 0 : 1;
    }
    else
    {
        // The check: the same export in tiles of 256 pixels and in one tile.
        const std::filesystem::path folder = !options.checkDir.empty() ? std::filesystem::path(options.checkDir) : std::filesystem::path(args.out).parent_path();
        desc.format = rasm::ExportFormat::PNG8;
        desc.tileSize = options.tile > 0 ? options.tile : 256;
        rasm::ReadbackImage tiled;
        rasm::ReadbackImage single;
        double tiledSeconds = 0.0;
        double singleSeconds = 0.0;
        bool done = runExport(engine, &view, 1, engine.exportView(view, desc), "tiled", {}, tiled, tiledSeconds);
        desc.tileSize = 16384;
        done = done && runExport(engine, &view, 1, engine.exportView(view, desc), "single", {}, single, singleSeconds);
        if (!done)
            return 1;
        rasm::ReadbackImage diffImage;
        const ExportDiff diff = compareExports(tiled, single, CHECK_THRESHOLD, &diffImage);
        printExportDiff("tiled vs single", diff, CHECK_THRESHOLD);
        std::printf("  tiled %.2f s, single %.2f s\n", tiledSeconds, singleSeconds);
        rasm::writePng((folder / "export_tiled.png").string(), tiled);
        rasm::writePng((folder / "export_single.png").string(), single);
        rasm::writePng((folder / "export_diff.png").string(), diffImage);
        const bool same = static_cast<double>(diff.over) <= CHECK_FRACTION * static_cast<double>(diff.pixels);
        std::printf("check: %s\n", same ? "the tiles match the single render" : "FAILED: the tiles differ from the single render");
        status = same ? 0 : 1;
    }

    while (exampleRunning(engine, args))
        engine.render(&view, 1);
    return status != 0 ? status : exampleExit(args);
}
