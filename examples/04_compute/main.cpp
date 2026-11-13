#include "rasm/rasm.h"

#include <cstdint>

namespace
{
    constexpr uint32_t PARTICLE_COUNT = 4096;
    constexpr uint32_t GROUP_SIZE = 64;        // numthreads of compMain
    constexpr float FRAME_TIME = 1.0f / 60.0f; // time advances per frame, so frame N looks the same on every run

    struct AnimatePushConstants
    {
        uint64_t positions;
        uint32_t count;
        float time;
    };

    struct DrawPushConstants
    {
        uint64_t positions;
        float aspect;
        uint32_t count;
    };
}

int main()
{
    rasm::EngineConfig config = {
        .appName = "example_4",
        .windowWidth = 1280,
        .windowHeight = 720,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN,
        .assetRoot = RASM_EXAMPLE_ASSET_ROOT,
        .shaderDir = RASM_EXAMPLE_SHADER_DIR};

    rasm::Engine engine(config);
    if (!engine.running())
        return 1;

    // The frame needs a camera for input handling, even though nothing here reads it.
    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraProjection::Type::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f}},
                                                    rasm::Transform{});

    rasm::BufferHandle particles = engine.createBuffer({.name = "Particle Positions",
                                                        .usage = rasm::BufferUsage::STORAGE,
                                                        .size = static_cast<uint64_t>(PARTICLE_COUNT) * 2 * sizeof(float)});
    if (!particles.isValid())
        return 1;
    const uint64_t particlesAddress = engine.getRenderContext().getBufferDeviceAddress(particles);

    auto animatePipeline = engine.createComputePipeline("animate", R"(
        struct PushConstants
        {
            float2* positions;
            uint count;
            float time;
        };

        [[vk::push_constant]]
        ConstantBuffer<PushConstants> pc;

        // A sunflower spiral that turns and breathes.
        [shader("compute")]
        [numthreads(64, 1, 1)]
        void compMain(uint3 id : SV_DispatchThreadID)
        {
            uint i = id.x;
            if (i >= pc.count)
                return;

            float radius = sqrt((i + 0.5) / pc.count) * (0.85 + 0.1 * sin(pc.time * 2.0 - i * 0.004));
            float angle = i * 2.39996 + pc.time * 0.3;
            pc.positions[i] = float2(cos(angle), sin(angle)) * radius;
        }
    )");

    // Three vertices per particle, read from the buffer the compute pass wrote.
    auto drawPipeline = engine.createPipeline("particles", R"(
        struct PushConstants
        {
            float2* positions;
            float aspect;
            uint count;
        };

        [[vk::push_constant]]
        ConstantBuffer<PushConstants> pc;

        struct VSOutput
        {
            float4 pos : SV_POSITION;
            float3 color : COLOR0;
        };

        [shader("vertex")]
        VSOutput vertMain(uint vertexID : SV_VertexID)
        {
            uint particle = vertexID / 3;
            uint corner = vertexID % 3;
            float angle = corner * 2.0943951;
            float2 offset = float2(cos(angle), sin(angle)) * 0.008;

            float2 center = pc.positions[particle];
            VSOutput output;
            output.pos = float4((center.x + offset.x) / pc.aspect, center.y + offset.y, 0.0, 1.0);
            float t = float(particle) / pc.count;
            output.color = 0.5 + 0.5 * cos(6.2831853 * (t + float3(0.0, 0.33, 0.67)));
            return output;
        }

        [shader("fragment")]
        float4 fragMain(VSOutput input) : SV_TARGET
        {
            return float4(input.color, 1.0);
        }
    )");

    if (!animatePipeline.isValid() || !drawPipeline.isValid())
        return 1;

    uint32_t frame = 0;
    rasm::RenderGraph graph = engine.createRenderGraph();

    graph.addComputePass(
        "Animate",
        [&particles](rasm::PassBuilder &builder)
        {
            builder.write(particles);
        },

        [&engine, &animatePipeline, &frame, particlesAddress](rasm::RenderContext &ctx)
        {
            auto commandBuffer = engine.getCurrentFrameResources().commandBuffer;
            const AnimatePushConstants pushConstants = {.positions = particlesAddress, .count = PARTICLE_COUNT, .time = frame * FRAME_TIME};

            // The graph puts barriers around the pass, so the previous frame's draw is done reading the
            // buffer (both frame slots share it) and this frame's draw sees the new positions.
            ctx.bindPipeline(commandBuffer, animatePipeline);
            ctx.pushConstants(commandBuffer, animatePipeline, rasm::ShaderType::COMPUTE, &pushConstants, sizeof(pushConstants));
            ctx.dispatch(commandBuffer, (PARTICLE_COUNT + GROUP_SIZE - 1) / GROUP_SIZE);
        });

    graph.addPass(
        "Draw",
        [&particles](rasm::PassBuilder &builder)
        {
            builder.read(particles);
        },

        [&engine, &drawPipeline, particlesAddress](rasm::RenderContext &ctx)
        {
            auto commandBuffer = engine.getCurrentFrameResources().commandBuffer;
            const auto &window = engine.getMainWindow();
            const DrawPushConstants pushConstants = {.positions = particlesAddress,
                                                     .aspect = static_cast<float>(window.pixelWidth) / static_cast<float>(window.pixelHeight),
                                                     .count = PARTICLE_COUNT};

            ctx.bindPipeline(commandBuffer, drawPipeline);
            ctx.pushConstants(commandBuffer, drawPipeline, rasm::ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants));
            ctx.draw(commandBuffer, PARTICLE_COUNT * 3);
        });

    graph.compile();

    while (engine.running())
    {
        graph.execute(camera);
        ++frame;
    }

    return 0;
}
