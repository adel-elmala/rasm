#include "rasm/core/scene.h"
#include "rasm/core/engine.h"
#include "rasm/core/camera.h"

namespace rasm
{
    SceneHandle Engine::createScene()
    {
        const SceneHandle id{nextHandle.scene++, 1};

        auto scene = Scene{.handle = id};
        registery.scenes[id.index] = scene;

        assert(id.index < MAX_SCENES);

        currentScene = id;

        return id;
    }

    void Engine::addEntityToScene(SceneHandle scene, EntityHandle entity)
    {
        if (scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for adding entity.");
            isRunning = false;
            return;
        }

        auto &sceneRef = registery.scenes[scene.index];

        if (sceneRef.entityCount + 1 > MAX_ENTITIES_PER_SCENE)
        {
            spdlog::error("Exceeded maximum entities per scene.");
            return;
        }

        sceneRef.entities[sceneRef.entityCount++] = entity;
    }

    void Engine::addCameraToScene(SceneHandle scene, CameraHandle camera)
    {
        if (scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for adding camera.");
            isRunning = false;
            return;
        }

        auto &sceneRef = registery.scenes[scene.index];

        if (sceneRef.cameraCount + 1 > MAX_CAMERAS_PER_SCENE)
        {
            spdlog::error("Exceeded maximum cameras per scene.");
            return;
        }

        sceneRef.cameras[sceneRef.cameraCount++] = camera;
    }

    void Engine::addLightToScene(SceneHandle scene, LightHandle light)
    {
        if (scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for adding light.");
            isRunning = false;
            return;
        }

        auto &sceneRef = registery.scenes[scene.index];

        if (sceneRef.lightCount + 1 > MAX_LIGHTS_PER_SCENE)
        {
            spdlog::error("Exceeded maximum lights per scene.");
            return;
        }

        sceneRef.lights[sceneRef.lightCount++] = light;
    }

    CompiledScene Engine::compileScene(SceneHandle scene)
    {
        if (scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for compilation.");
            isRunning = false;
            return CompiledScene{};
        }

        if (compiledScenes.find(scene) != compiledScenes.end())
        {
            return compiledScenes[scene];
        }

        CompiledScene compiledScene;

        auto _scene = registery.scenes[scene.index];

        for (auto entityHandle : _scene.entities)
        {
            auto entity = registery.entities[entityHandle.index];

            auto mat = entity.material;
            auto mesh = entity.mesh;

            auto bufferHandle = uploadMesh(mesh);
            if (!bufferHandle.isValid())
            {
                spdlog::error("Failed to upload mesh for Entity {}. Skipping.", entity.handle.index);
                isRunning = false;
                continue;
            }

            compiledScene.meshData[entity.handle] = bufferHandle;
            compiledScene.materialToMeshes[mat].insert(entity.handle);
        }

        auto swapchainFormat = ctx.getSwapchainImageFormat();
        auto depthFormat = Format::D24_UNORM_S8_UINT;

        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            // create a pipeline for each material.
            auto pipelineDesc = ResourceDesc{};
            pipelineDesc.type = ResourceType::GRAPHICS_PIPELINE;
            pipelineDesc.name = "Pipeline for Material " + std::to_string(materialHandle.index);

            if (materialHandle.index >= MAX_MATERIALS)
            {
                spdlog::error("Material not found for handle {}.", materialHandle.index);
                continue;
            }
            auto material = registery.materials[materialHandle.index];

            auto shader = material.shader;
            auto shaderSource = registery.shaders[shader.index];

            auto shaderName = material.name;
            auto vertShader = shaderCompiler.compileFromString(shaderSource, shaderName + std::string(".vert.slang"), "vertMain");
            auto fragShader = shaderCompiler.compileFromString(shaderSource, shaderName + std::string(".frag.slang"), "fragMain");

            auto shaderDesc = ResourceDesc{};
            shaderDesc.type = ResourceType::SHADER;
            shaderDesc.shader.shaderType = ShaderType::VERTEX;
            shaderDesc.shader.sourceSize = vertShader.code.size();
            shaderDesc.shader.source = reinterpret_cast<const char *>(vertShader.code.data());

            pipelineDesc.pipeline.vertexShader = ctx.createShader(shaderDesc);

            shaderDesc.shader.shaderType = ShaderType::FRAGMENT;
            shaderDesc.shader.sourceSize = fragShader.code.size();
            shaderDesc.shader.source = reinterpret_cast<const char *>(fragShader.code.data());

            pipelineDesc.pipeline.fragmentShader = ctx.createShader(shaderDesc);

            pipelineDesc.pipeline.topology = PrimitiveTopology::TRIANGLE_LIST;

            pipelineDesc.pipeline.colorAttachmentFormat = swapchainFormat;
            pipelineDesc.pipeline.depthStencilAttachmentFormat = depthFormat;

            rasm::VertexAttributeDescription attrDesc[4] = {};
            auto size = std::min(vertShader.vertexInputLayout.size(), static_cast<size_t>(4));
            for (size_t i = 0; i < size; ++i)
            {
                attrDesc[i] = vertShader.vertexInputLayout[i];
            }

            pipelineDesc.pipeline.vertexInputLayout = {
                .attributes = {attrDesc[0], attrDesc[1], attrDesc[2], attrDesc[3]},
                .binding = 0,
                .stride = sizeof(Vertex),
                .perInstance = false};

            pipelineDesc.pipeline.descriptorSetLayout = bindlessDescriptorSetLayout;

            auto pipeline = ctx.createPipeline(pipelineDesc);
            compiledScene.materialToPipeline[materialHandle] = pipeline;
        }

        compiledScenes[scene] = compiledScene;

        return compiledScene;
    }

}
