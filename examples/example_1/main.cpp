// Future API sketch (not implemented yet):
#include "rasm/rasm.h"

int main()
{
    rasm::EngineConfig config = {
        .appName = "example_1",
        .windowWidth = 1920,
        .windowHeight = 1080,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN };

    rasm::Engine engine(config);

    // set up the scene
    rasm::SceneHandle scene = engine.createScene();

    // Create a camera
    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{ .type = rasm::CameraType::PERSPECTIVE,
                                                                           .perspective{ .fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f,
                                                                                        .farZ = 32.0f } },
        rasm::Transform{
            glm::vec3(0.0f, 0.0f, -1.0f),      // position
            glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
            glm::vec3(1.0f)                    // scale
        });

    // Load resources
    rasm::MeshHandle bunnyMesh = engine.loadMesh("assets/models/bunny.obj");

    rasm::TextureHandle albedoTexture = engine.loadTexture("assets/textures/bunny-atlas.jpg");
    rasm::TextureHandle normalTexture = engine.loadTexture("assets/textures/test1.jpg");

    // Create a material
    rasm::TextureHandle textures[] = { albedoTexture, normalTexture };
    rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::BASIC, textures);

    // Create entities in the scene
    rasm::EntityHandle bunny = engine.createEntity("bunny", bunnyMesh, material, rasm::Transform{
                                                                                     glm::vec3(0.0f, 0.0f, -10.0f),     // position
                                                                                     glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                                     glm::vec3(0.05f)                   // scale
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

    rasm::ResourceHandle rtHandle{};
    // Standard forward rendering pass.
    graph.addPass(
        "Forward",
        [&config, &rtHandle, &engine](rasm::PassBuilder& builder)
        {
            auto rtDesc = rasm::ResourceDesc{};
            rtDesc.name = "offscreen_rt";
            rtDesc.type = rasm::ResourceType::RENDER_TARGET;
            rtDesc.renderTarget.width = config.windowWidth;
            rtDesc.renderTarget.height = config.windowHeight;
            rtDesc.renderTarget.colorFormat = engine.getRenderContext().getSwapchainImageFormat();
            rtDesc.renderTarget.depthFormat = rasm::Format::D24_UNORM_S8_UINT;

            rtHandle = builder.createResource(rtDesc);
            builder.write(rtHandle);
        },

        [&engine, &scene, &camera, &rtHandle](rasm::RenderContext& ctx)
        {
            // TODO: This is a temporary workaround to get the render target handle from the resource handle.
            rasm::RenderTargetHandle renderTargetHandle{ rtHandle.index, rtHandle.generation };
            engine.setRenderTarget(renderTargetHandle);

            auto compiledScene = engine.compileScene(scene);

            auto& currentFrame = engine.getCurrentFrameResources();
            auto commandBuffer = currentFrame.commandBuffer;
            auto shaderDataBuffer = currentFrame.shaderDataBuffer;

            auto camTransform = engine.getCameraTransform(camera);
            auto camPos = camTransform.position;

            auto projection = getProjectionMatrix(engine.getCameraProjection(camera));
            auto view = glm::translate(glm::mat4(1.0f), camPos);

            auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

            auto shaderDataIdx = 0;
            for (const auto& [materialHandle, entitySet] : compiledScene.materialToMeshes)
            {
                auto pipeline = compiledScene.materialToPipeline[materialHandle];
                ctx.bindPipeline(commandBuffer, pipeline);
                for (const auto& entityHandle : entitySet)
                {
                    auto modelTransform = engine.getTransform(entityHandle);

                    auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                        glm::mat4_cast(modelTransform.rotation) *
                        glm::scale(glm::mat4(1.0f), modelTransform.scale);

                    auto baseOffset = (sizeof(projection) + sizeof(view) + sizeof(model)) * shaderDataIdx;
                    ctx.fillBuffer(shaderDataBuffer, &projection, sizeof(projection), baseOffset);
                    ctx.fillBuffer(shaderDataBuffer, &view, sizeof(view), sizeof(projection) + baseOffset);
                    ctx.fillBuffer(shaderDataBuffer, &model, sizeof(model), sizeof(projection) + sizeof(view) + baseOffset);

                    auto bufferHandle = compiledScene.meshData[entityHandle];
                    auto desc = ctx.getResourceDesc(bufferHandle);
                    ctx.bindDescriptorSet(commandBuffer, pipeline, engine.getBindlessDescriptorSet(), 0);
                    ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                    ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.vertexIndexBuffer.offset, rasm::Format::U32_UINT);
                    auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                    ctx.pushConstants(commandBuffer, pipeline, rasm::ShaderType::VERTEX, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
                    ctx.drawIndexed(commandBuffer, static_cast<uint32_t>(desc.buffer.vertexIndexBuffer.indexCount), 1, 0, 0, 0);
                    shaderDataIdx++;
                }
            }
        });

    auto overlayScene = engine.createScene();
    engine.addCameraToScene(overlayScene, camera);

    graph.addPass(
        "overlay",
        [&engine, &overlayScene, &rtHandle](rasm::PassBuilder& builder)
        {
            builder.read(rtHandle);
            std::vector<rasm::Vertex> vertices = {
                { .pos = glm::vec3(-1.0f, -1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(0.0f, 0.0f) },
                { .pos = glm::vec3(1.0f, -1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(1.0f, 0.0f) },
                { .pos = glm::vec3(1.0f, 1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(1.0f, 1.0f) },
                { .pos = glm::vec3(-1.0f, 1.0f, 0.0f), .normal = glm::vec3(0.0f, 0.0f, 1.0f), .uv = glm::vec2(0.0f, 1.0f) } };

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
                float4 fragMain(VSOutput input) : SV_TARGET
                {
                    float3 color = gTextures[6].Sample(input.UV).rgb;
                    return float4(color, 1.0);
                }
            )");

            auto overlayMaterial = engine.createMaterial(rasm::MaterialType::SHADER, nullptr, overLayShader);
            auto overlayRect = engine.createEntity("overlayRect", overlayMesh, overlayMaterial);

            engine.addEntityToScene(overlayScene, overlayRect);
        },

        [&engine, &overlayScene, &camera](rasm::RenderContext& ctx)
        {
            engine.resetRenderTarget();

            auto compiledScene = engine.compileScene(overlayScene);

            auto& currentFrame = engine.getCurrentFrameResources();
            auto commandBuffer = currentFrame.commandBuffer;
            auto shaderDataBuffer = currentFrame.shaderDataBuffer;

            auto camTransform = engine.getCameraTransform(camera);
            auto camPos = camTransform.position;

            auto projection = getProjectionMatrix(engine.getCameraProjection(camera));
            auto view = glm::translate(glm::mat4(1.0f), camPos);

            auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

            auto shaderDataIdx = 3;
            for (const auto& [materialHandle, entitySet] : compiledScene.materialToMeshes)
            {
                auto pipeline = compiledScene.materialToPipeline[materialHandle];
                ctx.bindPipeline(commandBuffer, pipeline);
                for (const auto& entityHandle : entitySet)
                {
                    auto modelTransform = engine.getTransform(entityHandle);

                    auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                        glm::mat4_cast(modelTransform.rotation) *
                        glm::scale(glm::mat4(1.0f), modelTransform.scale);

                    auto baseOffset = (sizeof(projection) + sizeof(view) + sizeof(model)) * shaderDataIdx;
                    ctx.fillBuffer(shaderDataBuffer, &projection, sizeof(projection), baseOffset);
                    ctx.fillBuffer(shaderDataBuffer, &view, sizeof(view), sizeof(projection) + baseOffset);
                    ctx.fillBuffer(shaderDataBuffer, &model, sizeof(model), sizeof(projection) + sizeof(view) + baseOffset);

                    auto bufferHandle = compiledScene.meshData[entityHandle];
                    auto desc = ctx.getResourceDesc(bufferHandle);
                    ctx.bindDescriptorSet(commandBuffer, pipeline, engine.getBindlessDescriptorSet(), 0);
                    ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                    ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.vertexIndexBuffer.offset, rasm::Format::U32_UINT);
                    auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                    ctx.pushConstants(commandBuffer, pipeline, rasm::ShaderType::VERTEX, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
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
