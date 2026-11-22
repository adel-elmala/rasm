#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

#include "exampleArgs.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Shadow, forward, post and tone-map passes in one render graph, with no hand-written barrier: every pass declares
// what it reads and writes, and the graph orders the passes and moves the shadow cascades between depth attachment
// and sampled. The sun's shadow is four cascades that follow the camera: sharp near it, and still there far away.

namespace
{
    // Must match PushConstants in the inset shader below.
    struct InsetPushConstants
    {
        uint32_t cascades[rasm::MAX_SHADOW_CASCADES];
    };
}

namespace
{
    // The inset shader below draws one tile per cascade, for MAX_SHADOW_CASCADES cascades.
    constexpr uint32_t CASCADE_COUNT = 4;
    static_assert(CASCADE_COUNT == rasm::MAX_SHADOW_CASCADES, "the inset shader draws MAX_SHADOW_CASCADES tiles");

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

    rasm::MaterialHandle createColorMaterial(rasm::Engine &engine, const glm::vec3 &color, float roughness)
    {
        auto material = engine.createMaterial(rasm::MaterialType::PBR);
        auto values = engine.getMaterial(material);
        for (uint32_t i = 0; i < 3; ++i)
            values.params[static_cast<uint32_t>(rasm::PbrParam::ALBEDO) + i] = color[i];
        values.params[static_cast<uint32_t>(rasm::PbrParam::ROUGHNESS)] = roughness;
        engine.updateMaterial(material, values);
        return material;
    }

    rasm::Transform makeTransform(const glm::vec3 &position, const glm::vec3 &scale)
    {
        return rasm::Transform{position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), scale};
    }
}

int main(int argc, char **argv)
{
    const ExampleArgs args = parseExampleArgs(argc, argv);
    rasm::EngineConfig config = {
        .appName = "example_6",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR,
        .ui = true};

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    rasm::SceneHandle scene = engine.createScene();

    rasm::MeshHandle cube = createCube(engine);
    rasm::MeshHandle bunnyMesh = engine.loadMesh("models/obj/bunny/bunny.obj");
    if (!cube.isValid() || !bunnyMesh.isValid())
        return 1;

    const rasm::MaterialHandle ground = createColorMaterial(engine, glm::vec3(0.75f), 0.9f);
    const rasm::MaterialHandle warm = createColorMaterial(engine, glm::vec3(0.9f, 0.5f, 0.25f), 0.5f);
    const rasm::MaterialHandle cool = createColorMaterial(engine, glm::vec3(0.25f, 0.5f, 0.9f), 0.4f);

    engine.addEntityToScene(scene, engine.createEntity("ground", cube, ground, makeTransform(glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(120.0f, 0.5f, 120.0f))));
    // The bunny's vertices are centered on its origin and reach 247.6 below it, so it is lifted to stand on the ground.
    engine.addEntityToScene(scene, engine.createEntity("bunny", bunnyMesh, warm, makeTransform(glm::vec3(0.0f, 247.6f * 0.01f, 0.0f), glm::vec3(0.01f))));
    for (int i = 0; i < 4; ++i)
    {
        const float angle = glm::radians(45.0f + 90.0f * static_cast<float>(i));
        const float height = 1.5f + static_cast<float>(i);
        const glm::vec3 position(std::cos(angle) * 6.0f, height * 0.5f, std::sin(angle) * 6.0f);
        engine.addEntityToScene(scene, engine.createEntity("pillar", cube, cool, makeTransform(position, glm::vec3(1.0f, height, 1.0f))));
    }
    // Rows of pillars far behind the bunny, so the far cascades have shadows to show.
    for (int row = 0; row < 4; ++row)
    {
        for (int column = -3; column <= 3; ++column)
        {
            const glm::vec3 position(static_cast<float>(column) * 6.0f, 2.0f, -14.0f - static_cast<float>(row) * 10.0f);
            engine.addEntityToScene(scene, engine.createEntity("far pillar", cube, cool, makeTransform(position, glm::vec3(1.0f, 4.0f, 1.0f))));
        }
    }
    rasm::EntityHandle spinner = engine.createEntity("spinner", cube, warm, makeTransform(glm::vec3(3.0f, 4.0f, 3.0f), glm::vec3(1.5f)));
    engine.addEntityToScene(scene, spinner);

    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f}},
                                                    rasm::Transform{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)});
    engine.setCameraTransform(camera, rasm::Transform{glm::vec3(0.0f, 12.0f, 22.0f),
                                                      glm::quatLookAt(glm::normalize(glm::vec3(0.0f, -12.0f, -22.0f)), glm::vec3(0.0f, 1.0f, 0.0f)),
                                                      glm::vec3(1.0f)});
    engine.setCameraController(camera, rasm::CameraController{
                                           .type = rasm::CameraController::Type::TRACK_BALL,
                                           .trackBall = {.radius = glm::length(glm::vec3(0.0f, 12.0f, 22.0f)),
                                                         .center = glm::vec3(0.0f),
                                                         .orbitSensitivity = 1.0f},
                                           .easingSpeed = 6.0f});
    engine.addCameraToScene(scene, camera);

    const glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.35f));
    const rasm::LightHandle sun = engine.createLight(rasm::LightType::DIRECTIONAL, glm::vec3(1.0f, 0.95f, 0.85f), glm::vec3(0.0f), lightDirection, 4.0f, 0.0f);
    engine.addLightToScene(scene, sun);

    if (!camera.isValid() || !sun.isValid())
        return 1;

    rasm::RenderGraph graph = engine.createRenderGraph();

    const rasm::RenderTargetHandle sceneTarget = graph.createRenderTarget({
        .name = "scene_rt",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT},
        .depthFormat = rasm::Format::D32_SFLOAT,
        .clearColor = glm::vec4(0.35f, 0.45f, 0.6f, 1.0f),
    });
    const rasm::RenderTargetHandle postTarget = graph.createRenderTarget({
        .name = "post_rt",
        .swapchainScale = 1.0f,
        .colorFormats = {rasm::SCENE_COLOR_FORMAT},
        .depthFormat = rasm::Format::D32_SFLOAT,
    });

    // 1. Shadow: the scene from the sun, depth only, once per cascade. Each cascade covers a slice of the camera's
    //    view, out to 60 units from it.
    // 2. Forward: the scene from the camera, shaded with those cascades, which darken the sun. Its passes read the
    //    cascades, so they run after.
    const rasm::SceneShadow shadow = engine.addShadowPasses(graph, scene, camera, rasm::ShadowDesc{.light = sun, .cascadeCount = CASCADE_COUNT, .size = 2048, .distance = 60.0f, .splitLambda = 0.7f});
    if (shadow.mapCount == 0 || !engine.addScenePasses(graph, scene, camera, sceneTarget, &shadow, 1))
        return 1;

    // 3. Post: a vignette over the forward image. It stays in HDR.
    auto postPipeline = engine.createPipeline("post", R"(
        struct VSOutput
        {
            float4 Pos : SV_POSITION;
            float2 UV : TEXCOORD0;
        };

        [shader("vertex")]
        VSOutput vertMain(uint vertexID : SV_VertexID)
        {
            VSOutput output;
            output.UV = float2((vertexID << 1) & 2, vertexID & 2);
            // Clip space is Y-up and UV v = 0 is the image's top row.
            output.Pos = float4(output.UV.x * 2.0 - 1.0, 1.0 - output.UV.y * 2.0, 0.0, 1.0);
            return output;
        }

        [[vk::binding(0, 0)]]
        Sampler2D gTextures[];

        struct PushConstants
        {
            uint sceneColor;
        };

        [[vk::push_constant]]
        ConstantBuffer<PushConstants> pc;

        [shader("fragment")]
        float4 fragMain(VSOutput input) : SV_TARGET
        {
            float3 color = gTextures[pc.sceneColor].Sample(input.UV).rgb;
            float2 centered = input.UV - 0.5;
            float vignette = saturate(1.0 - dot(centered, centered) * 1.2);
            return float4(color * vignette, 1.0);
        }
    )",
                                              postTarget);
    if (!postPipeline.isValid())
        return 1;

    graph.addPass(
        "Post",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(sceneTarget);
            builder.write(postTarget);
        },
        [&](rasm::RenderContext &ctx)
        {
            auto commandBuffer = engine.getCurrentFrameResources().commandBuffer;
            const uint32_t sceneColor = engine.getBindlessTextureIndex(engine.getRenderTargetTexture(sceneTarget));
            ctx.bindPipeline(commandBuffer, postPipeline);
            ctx.bindDescriptorSet(commandBuffer, postPipeline, engine.getBindlessDescriptorSet(), 0);
            ctx.pushConstants(commandBuffer, postPipeline, rasm::ShaderType::VERTEX_FRAGMENT, &sceneColor, sizeof(sceneColor), 0);
            ctx.draw(commandBuffer, 3);
        });

    // 4. Tone map: the post image to the display, into the swapchain.
    if (!engine.addToneMapPass(graph, postTarget, camera))
        return 1;

    // 5. Inset: the cascades in a row in the top-left corner, nearest first, over the tone-mapped image, so it shows
    //    their raw depths.
    auto insetPipeline = engine.createPipeline("inset", R"(
        [shader("vertex")]
        float4 vertMain(uint vertexID : SV_VertexID) : SV_POSITION
        {
            float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
            return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
        }

        [[vk::binding(0, 0)]]
        Sampler2D gTextures[];

        struct PushConstants
        {
            uint cascades[4];
        };

        [[vk::push_constant]]
        ConstantBuffer<PushConstants> pc;

        static const float INSET_SIZE = 160.0;
        static const float GAP = 8.0;

        [shader("fragment")]
        float4 fragMain(float4 pixel : SV_POSITION) : SV_TARGET
        {
            float2 position = (pixel.xy - 16.0) / (INSET_SIZE + GAP);
            if (position.x < 0.0 || position.x >= 4.0)
                discard;
            int cascade = int(position.x);
            float2 inset = (position - float2(cascade, 0.0)) * (INSET_SIZE + GAP) / INSET_SIZE;
            if (any(inset < 0.0) || any(inset > 1.0))
                discard;
            // Reversed-Z: near the light is white. Each cascade's index is a constant element of the push constants.
            float depth = 0.0;
            [unroll]
            for (int i = 0; i < 4; ++i)
            {
                if (i == cascade)
                    depth = gTextures[pc.cascades[i]].SampleLevel(inset, 0.0).r;
            }
            return float4(depth.xxx, 1.0);
        }
    )");
    if (!insetPipeline.isValid())
        return 1;

    graph.addPass(
        "Inset",
        [&](rasm::PassBuilder &builder)
        {
            for (uint32_t i = 0; i < CASCADE_COUNT; ++i)
                builder.readDepth(shadow.maps[i]);
            builder.writeSwapchain();
        },
        [&](rasm::RenderContext &ctx)
        {
            auto commandBuffer = engine.getCurrentFrameResources().commandBuffer;
            InsetPushConstants pushConstants = {};
            for (uint32_t i = 0; i < CASCADE_COUNT; ++i)
                pushConstants.cascades[i] = engine.getBindlessTextureIndex(engine.getRenderTargetDepthTexture(shadow.maps[i]));
            ctx.bindPipeline(commandBuffer, insetPipeline);
            ctx.bindDescriptorSet(commandBuffer, insetPipeline, engine.getBindlessDescriptorSet(), 0);
            ctx.pushConstants(commandBuffer, insetPipeline, rasm::ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
            ctx.draw(commandBuffer, 3);
        });

    // The RASM window (F1) over the finished image.
    engine.addUiPass(graph);
    graph.compile();

    if (args.waitForLoads)
        engine.waitForLoads();

    // Animation advances per frame, not per second, so a given frame renders the same on every run.
    while (engine.running())
    {
        engine.updateTransform(spinner, engine.getTransform(spinner).rotate(glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f)), glm::radians(1.0f)));
        graph.execute(camera);
    }

    return 0;
}
