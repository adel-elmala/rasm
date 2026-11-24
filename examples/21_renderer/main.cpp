#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/renderer.h"

#include "exampleArgs.h"

#include <cmath>
#include <cstdint>
#include <vector>

// The renderer alone, as a map or tile server would use it: a rasm::Renderer with no window, no SDL, no file loaders
// and no asset folder. Everything it draws is made here: a terrain tile from a height function (createMesh with a
// color per vertex), a map grid texture (createTexture, then fillTexture of its pixels), a few boxes for buildings,
// one material each, and a sun. It renders two views of the tile into an offscreen image, a 3D view and a top-down
// map in the corner, and writes frame --frames (60) to --out (frame.png). The renderer has no window to show, so it
// always runs like --headless.

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr uint32_t TERRAIN_CELLS = 128;
    constexpr float TERRAIN_SIZE = 40.0f;
    constexpr uint32_t GRID_TEXTURE_SIZE = 256;

    float terrainHeight(float x, float z)
    {
        return 2.2f * std::sin(x * 0.18f) * std::cos(z * 0.15f) + 0.9f * std::sin(x * 0.47f + z * 0.31f) + 0.25f * std::cos(z * 0.9f - x * 0.2f);
    }

    // Water low, then grass, rock and snow, as a map colors its heights.
    glm::vec4 terrainColor(float height)
    {
        if (height < -1.6f)
            return {0.08f, 0.22f, 0.45f, 1.0f};
        if (height < 1.2f)
            return {0.18f, 0.42f, 0.14f, 1.0f};
        if (height < 2.4f)
            return {0.38f, 0.34f, 0.30f, 1.0f};
        return {0.92f, 0.92f, 0.95f, 1.0f};
    }

    // A grid of cells, the normals from the height function's slope.
    rasm::MeshHandle createTerrain(rasm::Renderer &renderer)
    {
        std::vector<rasm::Vertex> vertices;
        std::vector<uint32_t> colors;
        std::vector<uint32_t> indices;
        const float step = TERRAIN_SIZE / static_cast<float>(TERRAIN_CELLS);
        for (uint32_t row = 0; row <= TERRAIN_CELLS; ++row)
        {
            for (uint32_t column = 0; column <= TERRAIN_CELLS; ++column)
            {
                const float x = -0.5f * TERRAIN_SIZE + static_cast<float>(column) * step;
                const float z = -0.5f * TERRAIN_SIZE + static_cast<float>(row) * step;
                const float height = terrainHeight(x, z);
                const float slopeX = (terrainHeight(x + 0.01f, z) - terrainHeight(x - 0.01f, z)) / 0.02f;
                const float slopeZ = (terrainHeight(x, z + 0.01f) - terrainHeight(x, z - 0.01f)) / 0.02f;
                const glm::vec2 uv(static_cast<float>(column) / TERRAIN_CELLS, static_cast<float>(row) / TERRAIN_CELLS);
                vertices.push_back({.pos = {x, height, z}, .normal = glm::normalize(glm::vec3(-slopeX, 1.0f, -slopeZ)), .uv = {uv, uv, uv}, .tangent = glm::vec4(0.0f)});
                colors.push_back(rasm::packVertexColor(terrainColor(height)));
            }
        }
        const uint32_t stride = TERRAIN_CELLS + 1;
        for (uint32_t row = 0; row < TERRAIN_CELLS; ++row)
        {
            for (uint32_t column = 0; column < TERRAIN_CELLS; ++column)
            {
                const uint32_t corner = row * stride + column;
                for (uint32_t index : {corner, corner + stride, corner + 1, corner + 1, corner + stride, corner + stride + 1})
                    indices.push_back(index);
            }
        }
        return renderer.createMesh(std::move(vertices), std::move(indices), std::move(colors));
    }

    rasm::MeshHandle createBox(rasm::Renderer &renderer)
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
                vertices.push_back({.pos = corner * 0.5f, .normal = n, .uv = {glm::vec2(0.0f), glm::vec2(0.0f), glm::vec2(0.0f)}, .tangent = glm::vec4(0.0f)});
            for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                indices.push_back(first + index);
        }
        return renderer.createMesh(std::move(vertices), std::move(indices));
    }

    // White with a dark line every 32 texels: the map's grid, multiplied by the terrain's colors.
    rasm::TextureHandle createGridTexture(rasm::Renderer &renderer)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(GRID_TEXTURE_SIZE) * GRID_TEXTURE_SIZE * 4);
        for (uint32_t y = 0; y < GRID_TEXTURE_SIZE; ++y)
        {
            for (uint32_t x = 0; x < GRID_TEXTURE_SIZE; ++x)
            {
                const bool line = x % 32 < 2 || y % 32 < 2;
                const uint8_t value = line ? 90 : 255;
                uint8_t *texel = &pixels[(static_cast<size_t>(y) * GRID_TEXTURE_SIZE + x) * 4];
                texel[0] = value;
                texel[1] = value;
                texel[2] = value;
                texel[3] = 255;
            }
        }
        const rasm::TextureHandle texture = renderer.createTexture({.name = "map grid", .width = GRID_TEXTURE_SIZE, .height = GRID_TEXTURE_SIZE, .format = rasm::Format::R8G8B8A8_SRGB});
        if (!texture.isValid() || !renderer.getRenderContext().fillTexture(texture, pixels.data()))
            return {};
        return texture;
    }

    rasm::MaterialHandle createColorMaterial(rasm::Renderer &renderer, std::vector<rasm::TextureHandle> textures, glm::vec4 color, float roughness)
    {
        const rasm::MaterialHandle material = renderer.createMaterial(rasm::MaterialType::PBR, std::move(textures));
        rasm::Material values = renderer.getMaterial(material);
        for (uint32_t i = 0; i < 4; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[static_cast<glm::length_t>(i)];
        values.params[static_cast<uint32_t>(rasm::PbrParam::METALLIC)] = 0.0f;
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        renderer.updateMaterial(material, values);
        return material;
    }

    rasm::Transform lookAt(glm::vec3 eye, glm::vec3 target)
    {
        return {eye, glm::quatLookAt(glm::normalize(target - eye), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f)};
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    args.headless = true;

    // No nativeWindow: the renderer draws into offscreen images of windowWidth x windowHeight pixels.
    rasm::RendererConfig config = {
        .appName = "example_21",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .antiAliasing = rasm::AntiAliasing::MSAA_4X};
    applyExampleArgs(args, config);
    rasm::Renderer renderer(config);
    if (!renderer.running())
        return 1;

    const rasm::SceneHandle scene = renderer.createScene();
    const rasm::TextureHandle grid = createGridTexture(renderer);
    const rasm::MeshHandle terrain = createTerrain(renderer);
    const rasm::MeshHandle box = createBox(renderer);
    if (!grid.isValid() || !terrain.isValid() || !box.isValid())
        return 1;
    const rasm::MaterialHandle ground = createColorMaterial(renderer, {grid}, glm::vec4(1.0f), 0.9f);
    const rasm::MaterialHandle roof = createColorMaterial(renderer, {}, glm::vec4(0.75f, 0.32f, 0.22f, 1.0f), 0.6f);
    renderer.addEntityToScene(scene, renderer.createEntity("terrain", terrain, ground));

    // A small town on the grass: boxes of a few sizes, standing on the terrain.
    for (int i = 0; i < 24; ++i)
    {
        const float x = -6.0f + 3.1f * static_cast<float>(i % 6) + 0.7f * std::sin(static_cast<float>(i) * 1.7f);
        const float z = 2.0f + 2.6f * static_cast<float>(i / 6) + 0.6f * std::cos(static_cast<float>(i) * 2.3f);
        const float height = 0.8f + 0.9f * static_cast<float>((i * 7) % 4);
        const glm::vec3 size(1.2f, height, 1.2f);
        const glm::vec3 position(x, terrainHeight(x, z) + 0.5f * height - 0.2f, z);
        renderer.addEntityToScene(scene, renderer.createEntity("building", box, roof, rasm::Transform{position, glm::angleAxis(static_cast<float>(i) * 0.4f, glm::vec3(0.0f, 1.0f, 0.0f)), size}));
    }

    const rasm::LightHandle sun = renderer.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.95f, 0.85f), glm::vec3(0.0f), glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f)), 3.0f, 0.0f);
    renderer.addLightToScene(scene, sun);

    const float aspect = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT);
    const rasm::CameraHandle camera = renderer.createCamera({.type = rasm::CameraProjection::Type::PERSPECTIVE, .perspective = {.fovY = 50.0f, .aspect = aspect, .nearZ = 0.1f}}, rasm::Transform{});
    const float half = 0.5f * TERRAIN_SIZE;
    const rasm::CameraHandle map = renderer.createCamera({.type = rasm::CameraProjection::Type::ORTHOGRAPHIC, .orthographic = {.left = -half, .right = half, .bottom = -half, .top = half, .nearZ = 0.1f, .farZ = 100.0f}},
                                                         rasm::Transform{glm::vec3(0.0f, 50.0f, 0.0f), glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(1.0f)});
    renderer.addCameraToScene(scene, camera);
    renderer.addCameraToScene(scene, map);

    // The 3D view fills the image; the map is a square in the top-right corner, drawn over it.
    const float mapHeight = 0.36f;
    const rasm::View views[2] = {
        {.scene = scene, .camera = camera},
        {.scene = scene, .camera = map, .output = {.rect = {.x = 1.0f - mapHeight / aspect - 0.02f, .y = 0.03f, .width = mapHeight / aspect, .height = mapHeight}}, .antiAliasing = rasm::AntiAliasing::NONE},
    };

    // The camera circles the town, by frame and not by clock, so frame N is the same on every run.
    for (uint32_t frame = 0; exampleRunning(renderer, args); ++frame)
    {
        const float angle = 0.6f + 0.01f * static_cast<float>(frame);
        renderer.setCameraTransform(camera, lookAt(glm::vec3(24.0f * std::sin(angle), 15.0f, 24.0f * std::cos(angle)), glm::vec3(0.0f, 0.0f, 4.0f)));
        renderer.render(views, 2);
    }
    return exampleExit(args);
}
