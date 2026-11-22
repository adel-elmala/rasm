#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <cstdint>
#include <cstdio>
#include <vector>

// Streaming textures through the bindless set. A wall of tiles, each with its own material and texture; every frame a
// few tiles get a new texture (createTexture, fillTexture, updateMaterial) and their old one is destroyed
// (destroyTexture). The old texture and its bindless slot are freed once the GPU has finished the frames that may still
// sample them, and new textures reuse the slots. Far more textures pass through the wall than the set has slots
// (MAX_BINDLESS_TEXTURES): without the reuse the set would be full after MAX_BINDLESS_TEXTURES textures.

namespace
{
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;
    constexpr uint32_t TILES_X = 16;
    constexpr uint32_t TILES_Y = 9;
    constexpr uint32_t TILE_COUNT = TILES_X * TILES_Y;
    constexpr uint32_t TEXTURES_PER_FRAME = 24;
    constexpr uint32_t TEXTURE_SIZE = 64;

    struct Tile
    {
        rasm::EntityHandle entity;
        rasm::MaterialHandle material;
        rasm::TextureHandle texture;
        uint32_t version = 0;
    };

    // A unit square in the xy plane, facing +z.
    rasm::MeshHandle createQuad(rasm::Engine &engine)
    {
        const glm::vec3 normal(0.0f, 0.0f, 1.0f);
        std::vector<rasm::Vertex> vertices;
        for (const glm::vec2 corner : {glm::vec2(-0.5f, -0.5f), glm::vec2(0.5f, -0.5f), glm::vec2(0.5f, 0.5f), glm::vec2(-0.5f, 0.5f)})
        {
            const glm::vec2 uv(corner.x + 0.5f, 0.5f - corner.y);
            vertices.push_back({.pos = glm::vec3(corner, 0.0f), .normal = normal, .uv = {uv, uv, uv}, .tangent = glm::vec4(0.0f)});
        }
        return engine.createMesh(std::move(vertices), {0, 1, 2, 0, 2, 3});
    }

    uint32_t hash(uint32_t value)
    {
        value ^= value >> 16;
        value *= 0x7feb352dU;
        value ^= value >> 15;
        value *= 0x846ca68bU;
        value ^= value >> 16;
        return value;
    }

    // Two colors from the tile and its version, as stripes whose angle changes with the version: every new texture
    // looks different from the one it replaces.
    rasm::TextureHandle createTileTexture(rasm::Engine &engine, uint32_t tile, uint32_t version, std::vector<uint8_t> &pixels)
    {
        const uint32_t seed = hash(tile * 7919u + version * 104729u + 1u);
        const uint8_t a[3] = {static_cast<uint8_t>(seed), static_cast<uint8_t>(seed >> 8), static_cast<uint8_t>(seed >> 16)};
        const uint8_t b[3] = {static_cast<uint8_t>(255 - a[1]), static_cast<uint8_t>(255 - a[2]), static_cast<uint8_t>(255 - a[0])};
        const uint32_t direction = version % 3;
        pixels.resize(TEXTURE_SIZE * TEXTURE_SIZE * 4);
        for (uint32_t y = 0; y < TEXTURE_SIZE; ++y)
        {
            for (uint32_t x = 0; x < TEXTURE_SIZE; ++x)
            {
                const uint32_t band = direction == 0 ? x : direction == 1 ? y
                                                                          : x + y;
                const uint8_t *color = (band / 8) % 2 == 0 ? a : b;
                uint8_t *pixel = &pixels[(y * TEXTURE_SIZE + x) * 4];
                pixel[0] = color[0];
                pixel[1] = color[1];
                pixel[2] = color[2];
                pixel[3] = 255;
            }
        }
        const rasm::TextureHandle texture = engine.createTexture({.name = "Tile Texture", .width = TEXTURE_SIZE, .height = TEXTURE_SIZE, .format = rasm::Format::R8G8B8A8_SRGB, .usage = rasm::TextureUsage::SAMPLED, .mipLevels = 1});
        if (texture.isValid() && !engine.getRenderContext().fillTexture(texture, pixels.data()))
        {
            engine.destroyTexture(texture);
            return {};
        }
        return texture;
    }

    // The tile's material samples the new texture from the next frame on; the old one is freed once no frame uses it.
    bool replaceTexture(rasm::Engine &engine, uint32_t index, Tile &tile, std::vector<uint8_t> &pixels)
    {
        const rasm::TextureHandle texture = createTileTexture(engine, index, tile.version + 1, pixels);
        if (!texture.isValid())
            return false;
        rasm::Material values = engine.getMaterial(tile.material);
        values.textures[static_cast<uint32_t>(rasm::PbrSlot::ALBEDO)] = texture;
        engine.updateMaterial(tile.material, values);
        engine.destroyTexture(tile.texture);
        tile.texture = texture;
        ++tile.version;
        return true;
    }
}

int main(int argc, char **argv)
{
    ExampleArgs args = parseExampleArgs(argc, argv);
    rasm::EngineConfig config = {
        .appName = "example_17",
        .windowWidth = WIDTH,
        .windowHeight = HEIGHT,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};

    applyExampleArgs(args, config);
    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    rasm::SceneHandle scene = engine.createScene();
    const rasm::MeshHandle quad = createQuad(engine);
    if (!quad.isValid())
        return 1;

    std::vector<uint8_t> pixels;
    std::vector<Tile> tiles(TILE_COUNT);
    for (uint32_t i = 0; i < TILE_COUNT; ++i)
    {
        Tile &tile = tiles[i];
        tile.texture = createTileTexture(engine, i, 0, pixels);
        tile.material = engine.createMaterial(rasm::MaterialType::PBR, {tile.texture});
        if (!tile.texture.isValid() || !tile.material.isValid())
            return 1;
        rasm::Material values = engine.getMaterial(tile.material);
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = 0.8f;
        engine.updateMaterial(tile.material, values);
        const glm::vec3 position((static_cast<float>(i % TILES_X) - (TILES_X - 1) * 0.5f) * 1.1f, (static_cast<float>(i / TILES_X) - (TILES_Y - 1) * 0.5f) * 1.1f, 0.0f);
        tile.entity = engine.createEntity("tile", quad, tile.material, rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
        engine.addEntityToScene(scene, tile.entity);
    }

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f, .aspect = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT), .nearZ = 0.1f}},
                                                    rasm::Transform{glm::vec3(0.0f, 0.0f, 13.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    engine.addCameraToScene(scene, camera);
    engine.addLightToScene(scene, engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f), glm::vec3(0.0f), glm::normalize(glm::vec3(0.2f, -0.3f, -1.0f)), 3.0f, 0.0f));

    if (args.waitForLoads)
        engine.waitForLoads();

    // The tiles to renew go round the wall, so each frame renews the next TEXTURES_PER_FRAME of them.
    uint32_t next = 0;
    uint64_t streamed = TILE_COUNT;
    while (exampleRunning(engine, args))
    {
        for (uint32_t i = 0; i < TEXTURES_PER_FRAME; ++i)
        {
            const uint32_t index = (next + i * 7) % TILE_COUNT;
            if (!replaceTexture(engine, index, tiles[index], pixels))
            {
                std::fprintf(stderr, "error: tile %u got no new texture after %llu textures.\n", index, static_cast<unsigned long long>(streamed));
                return 1;
            }
            ++streamed;
        }
        next = (next + 1) % TILE_COUNT;
        engine.render(scene, camera);
    }

    std::printf("Streamed %llu textures through %u bindless slots.\n", static_cast<unsigned long long>(streamed), rasm::MAX_BINDLESS_TEXTURES);
    return exampleExit(args);
}
