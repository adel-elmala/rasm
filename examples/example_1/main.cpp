// Future API sketch (not implemented yet):
#include "rasm/rasm.h"

int main()
{
    rasm::EngineConfig config = {
        .appName = "example_1",
        .windowWidth = 1920,
        .windowHeight = 1080,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN};

    rasm::Engine engine(config);

    // set up the scene
    rasm::SceneHandle scene = engine.createScene();

    // Create a camera
    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraType::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f,
                                                                                        .farZ = 32.0f}},
                                                    rasm::Transform{
                                                        glm::vec3(0.0f, 0.0f, -1.0f),      // position
                                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                        glm::vec3(1.0f)                    // scale
                                                    });

    // Load resources
    rasm::MeshHandle bunnyMesh = engine.loadMesh("assets/models/bunny/bunny.obj");

    rasm::TextureHandle albedoTexture = engine.loadTexture("assets/models/bunny/bunny-atlas.jpg");

    if (!bunnyMesh.isValid() || !albedoTexture.isValid())
    {
        return 1;
    }

    // Create a material
    std::vector<rasm::TextureHandle> textures = { albedoTexture };
    rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::BASIC, textures);

    // Create entities in the scene
    rasm::EntityHandle bunny = engine.createEntity("bunny", bunnyMesh, material, rasm::Transform{
                                                                                     glm::vec3(0.0f, 0.0f, -10.0f),     // position
                                                                                     glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                                     glm::vec3(0.005f)                  // scale
                                                                                 });

    // Create a directional light
    rasm::LightHandle light = engine.createLight(rasm::LightType::DIRECTIONAL,  // type
                                                 glm::vec3(1.0f, 1.0f, 1.0f),   // color
                                                 1.0f,                          // intensity
                                                 glm::vec3(0.0f, 10.0f, 0.0f),  // position
                                                 glm::vec3(0.0f, -1.0f, 0.0f)); // direction

    engine.addEntityToScene(scene, bunny);
    engine.addCameraToScene(scene, camera);
    engine.addLightToScene(scene, light);

    rasm::RenderGraph graph = engine.createRenderGraph();

    rasm::RenderTargetHandle rtHandle{};
    // Standard forward rendering pass.
    graph.addPass(
        "Forward",
        [&config, &rtHandle, &engine](rasm::PassBuilder &builder)
        {
            auto rtDesc = rasm::ResourceDesc{};
            rtDesc.name = "offscreen_rt";
            rtDesc.type = rasm::ResourceType::RENDER_TARGET;
            rtDesc.renderTarget.width = config.windowWidth;
            rtDesc.renderTarget.height = config.windowHeight;
            rtDesc.renderTarget.colorFormat = engine.getRenderContext().getSwapchainImageFormat();
            rtDesc.renderTarget.depthFormat = rasm::Format::D32_SFLOAT_S8_UINT;

            rtHandle = builder.createRenderTarget(rtDesc);
            builder.write(rtHandle);
        },

        [&engine, &scene, &camera, &rtHandle](rasm::RenderContext &ctx)
        {
            auto compiledScene = engine.compileScene(scene);

            auto &currentFrame = engine.getCurrentFrameResources();
            auto commandBuffer = currentFrame.commandBuffer;
            auto shaderDataBuffer = currentFrame.shaderDataBuffer;

            auto camTransform = engine.getCameraTransform(camera);
            auto camPos = camTransform.position;

            struct ShaderData
            {
                glm::mat4 projection;
                glm::mat4 view;
                glm::mat4 model;
                uint32_t textureIndex;
            };

            ShaderData shaderData{};
            shaderData.projection = getProjectionMatrix(engine.getCameraProjection(camera));
            shaderData.view = glm::translate(glm::mat4(1.0f), camPos);

            auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

            auto shaderDataIdx = 0; // TODO: make a new API for allocating and managing per-object shader data
            for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
            {
                auto pipeline = compiledScene.materialToPipeline[materialHandle];
                ctx.bindPipeline(commandBuffer, pipeline);
                ctx.bindDescriptorSet(commandBuffer, pipeline, engine.getBindlessDescriptorSet(), 0);

                auto mat = engine.getMaterial(materialHandle);
                shaderData.textureIndex = engine.getBindlessTextureIndex(mat.textures[0]); // TODO: handle cases where material has more than 1 texture

                for (const auto &entityHandle : entitySet)
                {
                    auto modelTransform = engine.getTransform(entityHandle);

                    shaderData.model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                                       glm::mat4_cast(modelTransform.rotation) *
                                       glm::scale(glm::mat4(1.0f), modelTransform.scale);

                    auto baseOffset = sizeof(ShaderData) * shaderDataIdx;
                    ctx.fillBuffer(shaderDataBuffer, &shaderData, sizeof(shaderData), baseOffset);

                    auto bufferHandle = compiledScene.meshData[entityHandle];
                    auto desc = ctx.getResourceDesc(bufferHandle);
                    ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                    ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.offset, rasm::Format::U32_UINT);
                    auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                    ctx.pushConstants(commandBuffer, pipeline, rasm::ShaderType::VERTEX_FRAGMENT, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
                    ctx.drawIndexed(commandBuffer, static_cast<uint32_t>(desc.buffer.vertexIndexBuffer.indexCount), 1, 0, 0, 0);
                    shaderDataIdx++;
                }
            }
        });

    // set up overlay mesh and shader
     std::vector<rasm::Vertex> vertices = {
                {.pos = glm::vec3(-1.0f, -1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(0.0f, 0.0f)},
                {.pos = glm::vec3(1.0f, -1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(1.0f, 0.0f)},
                {.pos = glm::vec3(1.0f, 1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(1.0f, 1.0f)},
                {.pos = glm::vec3(-1.0f, 1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(0.0f, 1.0f)}};

    std::vector<uint32_t> indices = {
        0, 1, 2, // first triangle
        2, 3, 0  // second triangle
    };

    auto overlayMesh = engine.createMesh(vertices, indices);

    auto overLayShader = engine.createShader(R"(
        struct VSInput
        {
            float3 Pos;
            float3 Normal;
            float2 UV;
        };

        struct ShaderData
        {
            float4x4 projection;
            float4x4 view;
            float4x4 model;
            uint32_t textureIndex;
        };

        struct VSOutput
        {
            float4 Pos : SV_POSITION;
            float3 Normal : NORMAL;
            float2 UV : TEXCOORD0;
        };

        [shader("vertex")]
        VSOutput vertMain(VSInput input, uniform ShaderData *shaderData)
        {
            VSOutput output;
            float4x4 modelMat = shaderData->model;
            float4x4 viewMat = shaderData->view;
            float4x4 projectionMat = shaderData->projection;

            // output.Pos = mul(projectionMat, mul(viewMat, mul(modelMat, float4(input.Pos.xyz, 1.0))));
            output.Pos = float4(input.Pos.xyz, 1.0);
            output.Normal = input.Normal;
            output.UV = input.UV;

            return output;
        }

        [[vk::binding(0, 0)]]
        Sampler2D gTextures[];


        [shader("fragment")]
        float4 fragMain(VSOutput input, uniform ShaderData *shaderData) : SV_TARGET
        {
            float3 color = gTextures[shaderData->textureIndex].Sample(input.UV).rgb;
            if (color.x == 1.0 && color.y == 0.0 && color.z == 0.0) 
                color = float3(0.0, 1.0, 0.0);
            return float4(color, 1.0);
        }
    )");

    auto overlayMaterial = engine.createMaterial(rasm::MaterialType::SHADER, {}, overLayShader);
    auto overlayRect = engine.createEntity("overlayRect", overlayMesh, overlayMaterial);

    auto overlayScene = engine.createScene();
    engine.addCameraToScene(overlayScene, camera);
    engine.addEntityToScene(overlayScene, overlayRect);


    graph.addPass(
        "overlay",
        [&rtHandle](rasm::PassBuilder &builder)
        {
            builder.read(rtHandle);
        },

        [&engine, &overlayScene, &camera, &rtHandle](rasm::RenderContext &ctx)
        {
            auto compiledScene = engine.compileScene(overlayScene);

            auto &currentFrame = engine.getCurrentFrameResources();
            auto commandBuffer = currentFrame.commandBuffer;
            auto shaderDataBuffer = currentFrame.shaderDataBuffer;

            auto camTransform = engine.getCameraTransform(camera);
            auto camPos = camTransform.position;

            struct ShaderData
            {
                glm::mat4 projection;
                glm::mat4 view;
                glm::mat4 model;
                uint32_t textureIndex;
            };

            ShaderData shaderData{};
            shaderData.projection = getProjectionMatrix(engine.getCameraProjection(camera));
            shaderData.view = glm::translate(glm::mat4(1.0f), camPos);

            auto rt_color_attachment = engine.getRenderTargetAttachments(rtHandle).color;
            shaderData.textureIndex = engine.getBindlessTextureIndex(rt_color_attachment); // TODO: handle cases where material has more than 1 texture

            auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

            auto shaderDataIdx = 3;
            for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
            {
                auto pipeline = compiledScene.materialToPipeline[materialHandle];
                ctx.bindPipeline(commandBuffer, pipeline);
                ctx.bindDescriptorSet(commandBuffer, pipeline, engine.getBindlessDescriptorSet(), 0);

                for (const auto &entityHandle : entitySet)
                {
                    auto modelTransform = engine.getTransform(entityHandle);

                    shaderData.model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                                       glm::mat4_cast(modelTransform.rotation) *
                                       glm::scale(glm::mat4(1.0f), modelTransform.scale);

                    auto baseOffset = sizeof(ShaderData) * shaderDataIdx;
                    ctx.fillBuffer(shaderDataBuffer, &shaderData, sizeof(shaderData), baseOffset);

                    auto bufferHandle = compiledScene.meshData[entityHandle];
                    auto desc = ctx.getResourceDesc(bufferHandle);
                    ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                    ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.offset, rasm::Format::U32_UINT);
                    auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                    ctx.pushConstants(commandBuffer, pipeline, rasm::ShaderType::VERTEX_FRAGMENT, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
                    ctx.drawIndexed(commandBuffer, static_cast<uint32_t>(desc.buffer.vertexIndexBuffer.indexCount), 1, 0, 0, 0);
                    shaderDataIdx++;
                }
            }
        });

    graph.compile();

    while (engine.running())
    {
        graph.execute();
    }

    return 0;
}
