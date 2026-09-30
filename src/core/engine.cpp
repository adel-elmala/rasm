#include "rasm/core/engine.h"
#include "rasm/core/utils.h"

#include "spdlog/spdlog.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/quaternion.hpp"

namespace rasm
{
    ShaderCompiler::Target getShaderCompilerTarget(rasm::Backend backend)
    {
        switch (backend)
        {
        case Backend::VULKAN:
            return ShaderCompiler::Target::SPIRV;
        case Backend::DX12:
            return ShaderCompiler::Target::HLSL;
        case Backend::METAL:
            return ShaderCompiler::Target::MSL;
        default:
            spdlog::error("Unsupported backend for shader compilation.");
            return ShaderCompiler::Target::SPIRV; // Default to SPIR-V
        }
    }

    Engine::Engine(const EngineConfig &config) : config(config)
    {
        // In a real implementation, this is where we'd initialize the window, graphics context, etc.
        spdlog::info("Engine initialized with config: appName={}, windowWidth={}, windowHeight={}, enableValidation={}",
                     config.appName, config.windowWidth, config.windowHeight, config.enableValidation);

        this->mainWindow = createWindow(config.windowWidth, config.windowHeight);

        ctx = RenderContext(this);
        if (!ctx.initialize(config.preferredBackend))
        {
            spdlog::error("Failed to initialize render context.");
            ctx.cleanup();
            isRunning = false;
        }

        // Initialize frame resources for double buffering
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            frameResources[i].commandPool = ctx.createCommandPool(vkb::QueueType::graphics);
            frameResources[i].commandBuffer = ctx.createCommandBuffer(frameResources[i].commandPool);
            frameResources[i].readyToDrawSemaphore = ctx.createSemaphore();
            frameResources[i].inFlightFence = ctx.createFence(true);

            // Assuming a fixed size for shader data buffer for simplicity
            ResourceDesc shaderDataBufferDesc{};
            shaderDataBufferDesc.type = ResourceType::BUFFER;
            shaderDataBufferDesc.name = "ShaderDataBuffer for Frame " + std::to_string(i);
            shaderDataBufferDesc.buffer.size = 1024 * 1024; // 1 MB
            shaderDataBufferDesc.buffer.usage = BufferUsage::DEVICE_ADDRESS;

            frameResources[i].shaderDataBuffer = ctx.createBuffer(shaderDataBufferDesc);

            // Create a depth texture.
            auto depthTextureDesc = ResourceDesc{};
            depthTextureDesc.type = ResourceType::TEXTURE;
            depthTextureDesc.name = "DepthTexture for Frame " + std::to_string(i);
            depthTextureDesc.texture.width = static_cast<uint32_t>(config.windowWidth);
            depthTextureDesc.texture.height = static_cast<uint32_t>(config.windowHeight);
            depthTextureDesc.texture.format = Format::D24_UNORM_S8_UINT; // TODO: check this format
            depthTextureDesc.texture.usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT;

            frameResources[i].depthTexture = ctx.createTexture(depthTextureDesc);
        }

        // Create swapchain and associated resources
        swapchain.imageHandles = ctx.getSwapchainImages();
        for (size_t i = 0; i < swapchain.imageHandles.size(); ++i)
        {
            swapchain.readyToPresentSemaphores.push_back(ctx.createSemaphore());
        }
        swapchain.imageFormat = ctx.getSwapchainImageFormat();

        auto shaderTarget = getShaderCompilerTarget(config.preferredBackend);
        if (!shaderCompiler.initialize(shaderTarget))
        {
            spdlog::error("Failed to initialize shader compiler.");
            isRunning = false;
        }

        // Create bindless texture descriptor set layout, pool, and set
        ResourceDesc bindlessLayoutDesc{};
        bindlessLayoutDesc.type = ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT;
        bindlessLayoutDesc.name = "BindlessDescriptorSetLayout";
        bindlessLayoutDesc.bindlessDescriptorSetLayout.stage = ShaderType::FRAGMENT;
        bindlessLayoutDesc.bindlessDescriptorSetLayout.count = MAX_BINDLESS_TEXTURES;
        bindlessLayoutDesc.bindlessDescriptorSetLayout.type = ResourceType::TEXTURE;

        bindlessDescriptorSetLayout = ctx.createBindlessDescriptorSetLayout(bindlessLayoutDesc);

        ResourceDesc bindlessPoolDesc{};
        bindlessPoolDesc.type = ResourceType::DESCRIPTOR_POOL;
        bindlessPoolDesc.name = "BindlessDescriptorPool";
        bindlessPoolDesc.descriptorPool.descriptorType[0].descriptorType = ResourceType::TEXTURE;
        bindlessPoolDesc.descriptorPool.descriptorType[0].descriptorCount = MAX_BINDLESS_TEXTURES;
        bindlessPoolDesc.descriptorPool.maxSets = 1;

        bindlessDescriptorPool = ctx.createDescriptorPool(bindlessPoolDesc);

        ResourceDesc bindlessSetDesc{};
        bindlessSetDesc.type = ResourceType::DESCRIPTOR_SET;
        bindlessSetDesc.name = "BindlessDescriptorSet";
        bindlessSetDesc.descriptorSet.pool = bindlessDescriptorPool;
        bindlessSetDesc.descriptorSet.layout = bindlessDescriptorSetLayout;

        bindlessDescriptorSet = ctx.allocateDescriptorSet(bindlessSetDesc, bindlessDescriptorPool, bindlessDescriptorSetLayout);
    }

    Engine::~Engine()
    {
        // Clean up resources, free memory, etc.
        for (auto &[handle, tex] : registery.textureData)
        {
            stbi_image_free(tex.data);
        }

        for (auto mesh : registery.meshes)
        {
            if (mesh.type == Mesh::MeshType::GLTF)
            {
                // tinygltf::Model doesn't require explicit cleanup.
            }

            if (mesh.type == Mesh::MeshType::OBJ)
            {
                // If we had implemented OBJ loading, we would clean up any allocated resources here.
            }
        }

        ctx.cleanup();

        destroyWindow(this->mainWindow);
        spdlog::info("Engine shutdown, cleaned up resources.");
    }

    TextureHandle Engine::loadTexture(const std::string &path)
    {
        // find if the texture is already loaded, if so return existing handle
        if (registery.loadedTextures.find(path) != registery.loadedTextures.end())
        {
            return registery.loadedTextures[path];
        }

        // find if the texture extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedTextureExtensions.find(extension) == supportedTextureExtensions.end())
        {
            spdlog::error("Unsupported texture format: {}", extension);
            isRunning = false;
            return TextureHandle{};
        }

        int width, height, nChannels;
        unsigned char *data = stbi_load(path.c_str(), &width, &height, &nChannels, 4);

        if (!data)
        {
            spdlog::error("Failed to load texture: {}", path);
            isRunning = false;
            return TextureHandle{};
        }

        TextureRaw tex{};
        tex.width = width;
        tex.height = height;
        tex.channels = nChannels;
        tex.data = data;

        auto textureDesc = ResourceDesc{};
        textureDesc.name = path;
        textureDesc.type = ResourceType::TEXTURE;
        textureDesc.texture.width = width;
        textureDesc.texture.height = height;
        textureDesc.texture.format = Format::R8G8B8A8_UNORM;
        textureDesc.texture.usage = TextureUsage::SAMPLED;

        auto textureHandle = ctx.createTexture(textureDesc);
        if (!ctx.fillTexture(textureHandle, data))
        {
            spdlog::error("Failed to fill texture: {}", path);
            isRunning = false;
            return TextureHandle{};
        }

        // using the handle index as the slot for bindless descriptor set
        // this will result in a fragmented bindless descriptor set if the handles are not contiguous, (e.g, some handles are not for a sampled texture...),
        // but for simplicity we will use this approach for now
        if (!ctx.updateBindlessDescriptorSet(bindlessDescriptorSet, textureHandle, static_cast<uint32_t>(textureHandle.index)))
        {
            spdlog::error("Failed to update bindless descriptor set for texture: {}", path);
            isRunning = false;
            return TextureHandle{};
        }

        registery.loadedTextures[path] = textureHandle;
        registery.textureData.insert({textureHandle, std::move(tex)});

        return textureHandle;
    }

    TextureHandle Engine::createTexture(const ResourceDesc &desc)
    {
        if (desc.type != ResourceType::TEXTURE)
        {
            spdlog::error("Invalid resource type for createTexture.");
            return TextureHandle{};
        }

        return ctx.createTexture(desc);
    }

    BufferHandle Engine::createBuffer(const ResourceDesc &desc)
    {
        if (desc.type != ResourceType::BUFFER)
        {
            spdlog::error("Invalid resource type for createBuffer.");
            return BufferHandle{};
        }

        return ctx.createBuffer(desc);
    }

    RenderTargetHandle Engine::createRenderTarget(const ResourceDesc &desc)
    {
        if (desc.type != ResourceType::RENDER_TARGET)
        {
            spdlog::error("Invalid resource type for createRenderTarget.");
            return RenderTargetHandle{};
        }

        return ctx.createRenderTarget(desc);
    }

    RenderGraph Engine::createRenderGraph()
    {
        RenderGraph graph(this);
        return graph;
    }

    void Engine::beginFrame(CameraHandle camera)
    {
        // wait for the gpu to finish rendering the previous frame
        auto &currentFrame = frameResources[frameCount];
        ctx.waitForFence(currentFrame.inFlightFence);
        ctx.resetFence(currentFrame.inFlightFence);

        pollEvents(camera);

        if (this->resized)
        {
            recreateSwapchain();
            this->resized = false;
        }

        // acquire the next image from the swapchain
        ctx.acquireNextImage(currentFrame.readyToDrawSemaphore, UINT64_MAX, imageIdx);

        // record commands for the current frame
        ctx.beginCommandBuffer(currentFrame.commandBuffer);
    }

    void Engine::endFrame()
    {
        auto &currentFrame = frameResources[frameCount];

        ctx.endCommandBuffer(currentFrame.commandBuffer);

        ctx.submit(currentFrame.commandBuffer,
                   {currentFrame.readyToDrawSemaphore},
                   {swapchain.readyToPresentSemaphores[imageIdx]},
                   currentFrame.inFlightFence);

        ctx.present(imageIdx, swapchain.readyToPresentSemaphores[imageIdx]);

        frameCount = (frameCount + 1) % MAX_FRAMES_IN_FLIGHT; // Toggle between 0 and 1 for double buffering
    }

    // TODO: delete
    void Engine::beginOffscreenFrame(const RenderTargetHandle &renderTarget, CameraHandle camera)
    {
        auto &currentFrame = frameResources[frameCount];
        ctx.waitForFence(currentFrame.inFlightFence);
        ctx.resetFence(currentFrame.inFlightFence);

        pollEvents(camera);

        // record commands for the current frame
        ctx.beginCommandBuffer(currentFrame.commandBuffer);

        const RenderTarget &renderTargetData = ctx.getRenderTarget(renderTarget);

        // transition render target images to be ready for rendering
        ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
        ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.depthAttachment[frameCount], TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);

        // begin dynamic rendering
        ctx.beginRendering(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], renderTargetData.depthAttachment[frameCount]);
        ctx.setViewport(currentFrame.commandBuffer, 0.0f, 0.0f, static_cast<float>(renderTargetData.width), static_cast<float>(renderTargetData.height));
        ctx.setScissor(currentFrame.commandBuffer, 0, 0, renderTargetData.width, renderTargetData.height);
    }

    // TODO: delete
    void Engine::endOffscreenFrame()
    {
        auto &currentFrame = frameResources[frameCount];

        ctx.endRendering(currentFrame.commandBuffer);
        ctx.endCommandBuffer(currentFrame.commandBuffer);

        ctx.submit(currentFrame.commandBuffer,
                   {},
                   {},
                   currentFrame.inFlightFence);

        frameCount = (frameCount + 1) % MAX_FRAMES_IN_FLIGHT; // Toggle between 0 and 1 for double buffering
    }

    void Engine::beginPass()
    {
        auto currentFrame = frameResources[frameCount];

        if (currentRenderTarget.isValid())
        {
            auto renderTargetData = ctx.getRenderTarget(currentRenderTarget);

            // transition render target images to be ready for rendering
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.depthAttachment[frameCount], TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);
            ctx.beginRendering(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], renderTargetData.depthAttachment[frameCount]);
        }
        else
        {
            // transition swapchain image to be ready for rendering
            ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
            ctx.transitionImageLayout(currentFrame.commandBuffer, currentFrame.depthTexture, TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);
            ctx.beginRendering(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], currentFrame.depthTexture);
        }

        // begin dynamic rendering
        ctx.setViewport(currentFrame.commandBuffer, 0.0f, 0.0f, static_cast<float>(config.windowWidth), static_cast<float>(config.windowHeight));
        ctx.setScissor(currentFrame.commandBuffer, 0, 0, config.windowWidth, config.windowHeight);
    }

    void Engine::endPass()
    {
        auto currentFrame = frameResources[frameCount];
        ctx.endRendering(currentFrame.commandBuffer);

        if (currentRenderTarget.isValid())
        {
            auto renderTargetData = ctx.getRenderTarget(currentRenderTarget);

            // transition render target images to be ready for sampling
            // ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], TextureUsage::COLOR_ATTACHMENT, TextureUsage::SAMPLED);
            // ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.depthAttachment[frameCount], TextureUsage::DEPTH_STENCIL_ATTACHMENT, TextureUsage::SAMPLED);
        }
        else
        {
            ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::COLOR_ATTACHMENT, TextureUsage::PRESENT_SRC);
        }

        // TODO: insert barrier to ensure the pass has finished before subsequent operations
    }

    void Engine::setRenderTarget(const RenderTargetHandle &renderTarget)
    {
        this->currentRenderTarget = renderTarget;
    }

    void Engine::resetRenderTarget()
    {
        this->currentRenderTarget = RenderTargetHandle{};
    }

    void Engine::setScene(SceneHandle scene)
    {
        this->currentScene = scene;
    }

    SceneHandle Engine::getScene() const
    {
        return currentScene;
    }

    void Engine::recreateSwapchain()
    {
        ctx.recreateSwapchain();
        swapchain.imageHandles = ctx.getSwapchainImages();
        swapchain.imageFormat = ctx.getSwapchainImageFormat();

        // Destroy old semaphores
        for (auto &semaphore : swapchain.readyToPresentSemaphores)
        {
            ctx.destroySemaphore(semaphore);
        }
        swapchain.readyToPresentSemaphores.clear();

        for (size_t i = 0; i < swapchain.imageHandles.size(); ++i)
        {
            swapchain.readyToPresentSemaphores.push_back(ctx.createSemaphore());
        }

        // Update depth textures for each frame resource
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            auto depthTextureDesc = ResourceDesc{};
            depthTextureDesc.type = ResourceType::TEXTURE;
            depthTextureDesc.name = "DepthTexture for Frame " + std::to_string(i);
            depthTextureDesc.texture.width = static_cast<uint32_t>(config.windowWidth);
            depthTextureDesc.texture.height = static_cast<uint32_t>(config.windowHeight);
            depthTextureDesc.texture.format = Format::D24_UNORM_S8_UINT; // TODO: check this format
            depthTextureDesc.texture.usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT;

            frameResources[i].depthTexture = ctx.createTexture(depthTextureDesc);
        }
    }

    void Engine::render2(SceneHandle scene, CameraHandle camera)
    {
        beginFrame(camera);
        beginPass();

        auto preparedScene = prepareScene(scene);

        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;

        auto cam = registery.cameras[camera.index];
        auto camTransform = cam.transform;
        auto camPos = camTransform.position;

        auto projection = getProjectionMatrix(cam.projection);
        auto view = glm::translate(glm::mat4(1.0f), camPos);

        ctx.bindPipeline(commandBuffer, preparedScene.uberMaterialPipeline);
        {
            // Build the SceneData exactly as the shader declares it so the layouts cannot drift apart.
            SceneData sceneData{};
            sceneData.projection = projection;
            sceneData.view = view;
            sceneData.vertsPtr = ctx.getBufferDeviceAddress(preparedScene.megaVertexBuffer);
            sceneData.drawInfoPtr = ctx.getBufferDeviceAddress(preparedScene.drawInfoBuffer);
            sceneData.modeldataPtr = ctx.getBufferDeviceAddress(preparedScene.megaModelMatsBuffer);

            ctx.fillBuffer(preparedScene.sceneDataBuffer, &sceneData, sizeof(sceneData), 0);

            ctx.bindDescriptorSet(commandBuffer, preparedScene.uberMaterialPipeline, bindlessDescriptorSet, 0);
            ctx.bindIndexBuffer(commandBuffer, preparedScene.megaIndexBuffer, 0, Format::U32_UINT);

            auto sceneDataBufferPtr = ctx.getBufferDeviceAddress(preparedScene.sceneDataBuffer);
            ctx.pushConstants(commandBuffer, preparedScene.uberMaterialPipeline, ShaderType::VERTEX, &sceneDataBufferPtr, sizeof(uint64_t), 0);
            ctx.drawIndexedIndirect(commandBuffer, preparedScene.drawInfoBuffer, 0, preparedScene.drawInfoCount, sizeof(DrawInfo));
        }
        endPass();
        endFrame();
    }

    void Engine::render(SceneHandle scene, CameraHandle camera)
    {
        beginFrame(camera);
        beginPass();

        auto compiledScene = compileScene(scene);
        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;
        auto shaderDataBuffer = currentFrame.shaderDataBuffer;

        auto cam = registery.cameras[camera.index];
        auto camTransform = cam.transform;
        auto camPos = camTransform.position;

        auto projection = getProjectionMatrix(cam.projection);
        auto view = glm::translate(glm::mat4(1.0f), camPos);

        auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

        auto shaderDataIdx = 0;
        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            auto pipeline = compiledScene.materialToPipeline[materialHandle];
            ctx.bindPipeline(commandBuffer, pipeline);
            for (const auto &entityHandle : entitySet)
            {
                auto modelTransform = registery.entities[entityHandle.index].transform;

                auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                             glm::mat4_cast(modelTransform.rotation) *
                             glm::scale(glm::mat4(1.0f), modelTransform.scale);

                auto baseOffset = (sizeof(projection) + sizeof(view) + sizeof(model)) * shaderDataIdx;
                ctx.fillBuffer(shaderDataBuffer, &projection, sizeof(projection), baseOffset);
                ctx.fillBuffer(shaderDataBuffer, &view, sizeof(view), sizeof(projection) + baseOffset);
                ctx.fillBuffer(shaderDataBuffer, &model, sizeof(model), sizeof(projection) + sizeof(view) + baseOffset);

                auto bufferHandle = compiledScene.meshData[entityHandle];
                auto desc = ctx.getResourceDesc(bufferHandle);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
                ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.offset, Format::U32_UINT);
                auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
                ctx.drawIndexed(commandBuffer, static_cast<uint32_t>(desc.buffer.vertexIndexBuffer.indexCount), 1, 0, 0, 0);
                shaderDataIdx++;
            }
        }
        endPass();
        endFrame();
    }

    void Engine::render(SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget)
    {
        beginOffscreenFrame(renderTarget, camera);

        auto compiledScene = compileScene(scene);
        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;
        auto shaderDataBuffer = currentFrame.shaderDataBuffer;

        auto cam = registery.cameras[camera.index];
        auto camTransform = cam.transform;
        auto camPos = camTransform.position;

        auto projection = getProjectionMatrix(cam.projection);
        auto view = glm::translate(glm::mat4(1.0f), camPos);

        auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

        auto shaderDataIdx = 0;
        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            auto pipeline = compiledScene.materialToPipeline[materialHandle];
            ctx.bindPipeline(commandBuffer, pipeline);
            for (const auto &entityHandle : entitySet)
            {
                auto modelTransform = registery.entities[entityHandle.index].transform;

                auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                             glm::mat4_cast(modelTransform.rotation) *
                             glm::scale(glm::mat4(1.0f), modelTransform.scale);

                auto baseOffset = (sizeof(projection) + sizeof(view) + sizeof(model)) * shaderDataIdx;
                ctx.fillBuffer(shaderDataBuffer, &projection, sizeof(projection), baseOffset);
                ctx.fillBuffer(shaderDataBuffer, &view, sizeof(view), sizeof(projection) + baseOffset);
                ctx.fillBuffer(shaderDataBuffer, &model, sizeof(model), sizeof(projection) + sizeof(view) + baseOffset);

                auto bufferHandle = compiledScene.meshData[entityHandle];
                auto desc = ctx.getResourceDesc(bufferHandle);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet);
                ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.offset, Format::U32_UINT);
                auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
                ctx.drawIndexed(commandBuffer, static_cast<uint32_t>(desc.buffer.vertexIndexBuffer.indexCount), 1, 0, 0, 0);
                shaderDataIdx++;
            }
        }

        endOffscreenFrame();
    }

    bool Engine::running() const
    {
        return isRunning;
    }

    const FrameResources &Engine::getCurrentFrameResources()
    {
        return frameResources[frameCount % MAX_FRAMES_IN_FLIGHT];
    }

    EngineConfig Engine::getConfig() const
    {
        return config;
    }

    WindowHandle Engine::getMainWindow() const
    {
        return mainWindow;
    }

    RenderContext &Engine::getRenderContext()
    {
        return ctx;
    }

    DescriptorSetHandle Engine::getBindlessDescriptorSet()
    {
        return bindlessDescriptorSet;
    }

} // namespace rasm
