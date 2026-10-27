#include "rasm/engine.h"
#include "core/utils.h"

#include "spdlog/spdlog.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/quaternion.hpp"

namespace rasm
{
    namespace
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
    }

    Engine::Engine(const EngineConfig &config) : config(config)
    {
        // In a real implementation, this is where we'd initialize the window, graphics context, etc.
        spdlog::info("initializing engine with config: appName={}, windowWidth={}, windowHeight={}, enableValidation={}",
                     config.appName, config.windowWidth, config.windowHeight, config.enableValidation);

        ctx = RenderContext(this);

        if (config.windowWidth <= 0 || config.windowHeight <= 0)
        {
            spdlog::error("Window dimensions must be positive.");
            return;
        }

        this->mainWindow = createWindow(config.windowWidth, config.windowHeight);
        if (!mainWindow.handle.isValid())
        {
            spdlog::error("Failed to initialize engine window.");
            return;
        }

        if (!ctx.initialize(config.preferredBackend))
        {
            spdlog::error("Failed to initialize render context.");
            return;
        }

        // Initialize frame resources for double buffering
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            frameResources[i].commandPool = ctx.createCommandPool(vkb::QueueType::graphics);
            if (!frameResources[i].commandPool.isValid())
            {
                spdlog::error("Failed to create command pool for frame {}", i);
                return;
            }
            frameResources[i].commandBuffer = ctx.createCommandBuffer(frameResources[i].commandPool);
            if (!frameResources[i].commandBuffer.isValid())
            {
                spdlog::error("Failed to create command buffer for frame {}", i);
                return;
            }
            frameResources[i].readyToDrawSemaphore = ctx.createSemaphore();
            if (!frameResources[i].readyToDrawSemaphore.isValid())
            {
                spdlog::error("Failed to create ready-to-draw semaphore for frame {}", i);
                return;
            }
            frameResources[i].inFlightFence = ctx.createFence(true);
            if (!frameResources[i].inFlightFence.isValid())
            {
                spdlog::error("Failed to create in-flight fence for frame {}", i);
                return;
            }

            // Assuming a fixed size for shader data buffer for simplicity
            ResourceDesc shaderDataBufferDesc{};
            shaderDataBufferDesc.type = ResourceType::BUFFER;
            shaderDataBufferDesc.name = "ShaderDataBuffer for Frame " + std::to_string(i);
            shaderDataBufferDesc.buffer.size = 1024 * 1024; // 1 MB
            shaderDataBufferDesc.buffer.usage = BufferUsage::STORAGE;

            frameResources[i].shaderDataBuffer = ctx.createBuffer(shaderDataBufferDesc);
            if (!frameResources[i].shaderDataBuffer.isValid())
            {
                spdlog::error("Failed to create shader data buffer for frame {}", i);
                return;
            }
        }

        // Swapchain images, present semaphores and depth textures
        if (!createSwapchainResources())
            return;

        auto shaderTarget = getShaderCompilerTarget(config.preferredBackend);
        if (!shaderCompiler.initialize(shaderTarget))
        {
            spdlog::error("Failed to initialize shader compiler.");
            return;
        }

        // Create bindless texture descriptor set layout, pool, and set
        ResourceDesc bindlessLayoutDesc{};
        bindlessLayoutDesc.type = ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT;
        bindlessLayoutDesc.name = "BindlessDescriptorSetLayout";
        bindlessLayoutDesc.bindlessDescriptorSetLayout.stage = ShaderType::FRAGMENT;
        bindlessLayoutDesc.bindlessDescriptorSetLayout.count = MAX_BINDLESS_TEXTURES;
        bindlessLayoutDesc.bindlessDescriptorSetLayout.type = ResourceType::TEXTURE;

        bindlessDescriptorSetLayout = ctx.createBindlessDescriptorSetLayout(bindlessLayoutDesc);
        if (!bindlessDescriptorSetLayout.isValid())
        {
            spdlog::error("Failed to create bindless descriptor set layout.");
            return;
        }

        ResourceDesc bindlessPoolDesc{};
        bindlessPoolDesc.type = ResourceType::DESCRIPTOR_POOL;
        bindlessPoolDesc.name = "BindlessDescriptorPool";
        bindlessPoolDesc.descriptorPool.descriptorType[0].descriptorType = ResourceType::TEXTURE;
        bindlessPoolDesc.descriptorPool.descriptorType[0].descriptorCount = MAX_BINDLESS_TEXTURES;
        bindlessPoolDesc.descriptorPool.maxSets = 1;

        bindlessDescriptorPool = ctx.createDescriptorPool(bindlessPoolDesc);
        if (!bindlessDescriptorPool.isValid())
        {
            spdlog::error("Failed to create bindless descriptor pool.");
            return;
        }

        ResourceDesc bindlessSetDesc{};
        bindlessSetDesc.type = ResourceType::DESCRIPTOR_SET;
        bindlessSetDesc.name = "BindlessDescriptorSet";
        bindlessSetDesc.descriptorSet.pool = bindlessDescriptorPool;
        bindlessSetDesc.descriptorSet.layout = bindlessDescriptorSetLayout;

        bindlessDescriptorSet = ctx.allocateDescriptorSet(bindlessSetDesc, bindlessDescriptorPool, bindlessDescriptorSetLayout);
        if (!bindlessDescriptorSet.isValid())
        {
            spdlog::error("Failed to allocate bindless descriptor set.");
            return;
        }

        // The glTF default material: white base color, metallic 1, roughness 1.
        defaultMaterial = createMaterial(MaterialType::PBR);
        if (!defaultMaterial.isValid())
        {
            spdlog::error("Failed to create the default material.");
            return;
        }
        registery.materials[defaultMaterial.index].name = "default";
        registery.materials[defaultMaterial.index].params[static_cast<uint32_t>(PbrParam::METALLIC)] = 1.0f;

        isRunning = true;
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

    TextureHandle Engine::loadTexture(const std::string &path, Format format)
    {
        if (format != Format::R8G8B8A8_SRGB && format != Format::R8G8B8A8_UNORM)
        {
            spdlog::error("loadTexture requires RGBA8 sRGB or UNORM format.");
            return {};
        }
        const auto cacheKey = path + "#" + std::to_string(static_cast<int>(format));
        if (registery.loadedTextures.find(cacheKey) != registery.loadedTextures.end())
        {
            return registery.loadedTextures[cacheKey];
        }

        // find if the texture extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedTextureExtensions.find(extension) == supportedTextureExtensions.end())
        {
            spdlog::error("Unsupported texture format: {}", extension);
            return TextureHandle{};
        }

        int width, height, nChannels;
        unsigned char *data = stbi_load(path.c_str(), &width, &height, &nChannels, 4);

        if (!data)
        {
            spdlog::error("Failed to load texture: {}", path);
            return TextureHandle{};
        }

        TextureRaw tex{};
        tex.width = width;
        tex.height = height;
        tex.channels = 4;
        tex.data = data;

        auto textureDesc = ResourceDesc{};
        textureDesc.name = path;
        textureDesc.type = ResourceType::TEXTURE;
        textureDesc.texture.width = width;
        textureDesc.texture.height = height;
        textureDesc.texture.format = format;
        textureDesc.texture.usage = TextureUsage::SAMPLED;

        auto textureHandle = ctx.createTexture(textureDesc);
        if (!ctx.fillTexture(textureHandle, data))
        {
            spdlog::error("Failed to fill texture: {}", path);
            return TextureHandle{};
        }

        registery.loadedTextures[cacheKey] = textureHandle;
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

    bool Engine::beginFrame(CameraHandle camera)
    {
        pollEvents(camera);

        // Nothing can be presented to a minimized or zero-sized window (B5).
        if (mainWindow.minimized || mainWindow.pixelWidth == 0 || mainWindow.pixelHeight == 0)
            return false;

        if (mainWindow.resized || swapchainOutOfDate)
        {
            mainWindow.resized = false;
            swapchainOutOfDate = !recreateSwapchain();
            if (swapchainOutOfDate)
                return false;
        }

        // wait for the gpu to finish rendering the previous frame in this slot
        auto &currentFrame = frameResources[frameCount];
        if (!ctx.waitForFence(currentFrame.inFlightFence))
            return false;

        // acquire the next image from the swapchain (B4)
        auto status = ctx.acquireNextImage(currentFrame.readyToDrawSemaphore, UINT64_MAX, imageIdx);
        if (status == SwapchainStatus::OUT_OF_DATE)
        {
            // The semaphore was not signaled; skip the frame and recreate next time.
            swapchainOutOfDate = true;
            return false;
        }
        if (status == SwapchainStatus::FAILURE)
            return false;
        if (status == SwapchainStatus::SUBOPTIMAL)
            swapchainOutOfDate = true; // The image is acquired and still usable; recreate after this frame.

        // Reset only once work will be submitted, so a skipped frame leaves the fence signaled.
        ctx.resetFence(currentFrame.inFlightFence);

        // record commands for the current frame
        ctx.beginCommandBuffer(currentFrame.commandBuffer);
        return true;
    }

    void Engine::endFrame()
    {
        auto &currentFrame = frameResources[frameCount];

        ctx.endCommandBuffer(currentFrame.commandBuffer);

        if (ctx.submit(currentFrame.commandBuffer,
                       {currentFrame.readyToDrawSemaphore},
                       {swapchain.readyToPresentSemaphores[imageIdx]},
                       currentFrame.inFlightFence))
        {
            auto status = ctx.present(imageIdx, swapchain.readyToPresentSemaphores[imageIdx]);
            if (status == SwapchainStatus::SUBOPTIMAL || status == SwapchainStatus::OUT_OF_DATE)
                swapchainOutOfDate = true;
        }

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
        ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].color, TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
        ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].depth, TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);

        // begin dynamic rendering
        ctx.beginRendering(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].color, renderTargetData.attachments[frameCount].depth);
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
        uint32_t width = swapchain.width;
        uint32_t height = swapchain.height;

        if (currentRenderTarget.isValid())
        {
            auto renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            width = renderTargetData.width;
            height = renderTargetData.height;

            // transition render target images to be ready for rendering
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].color, TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].depth, TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);
            ctx.beginRendering(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].color, renderTargetData.attachments[frameCount].depth);
        }
        else
        {
            // transition swapchain image to be ready for rendering
            ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
            ctx.transitionImageLayout(currentFrame.commandBuffer, currentFrame.depthTexture, TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);
            ctx.beginRendering(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], currentFrame.depthTexture);
        }

        // begin dynamic rendering
        ctx.setViewport(currentFrame.commandBuffer, 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        ctx.setScissor(currentFrame.commandBuffer, 0, 0, width, height);
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

    bool Engine::recreateSwapchain()
    {
        auto status = ctx.recreateSwapchain();
        if (status == SwapchainStatus::OUT_OF_DATE)
            return false; // The surface is 0x0; the old swapchain and its resources are kept.
        if (status != SwapchainStatus::OK)
        {
            // The old swapchain is gone, so are its image handles.
            swapchain.imageHandles.clear();
            return false;
        }
        if (!createSwapchainResources())
            return false;
        spdlog::info("Recreated swapchain: {}x{}", swapchain.width, swapchain.height);
        return true;
    }

    // Called at startup and after a successful swapchain recreation, both with the device idle.
    bool Engine::createSwapchainResources()
    {
        swapchain.imageHandles = ctx.getSwapchainImages();
        swapchain.imageFormat = ctx.getSwapchainImageFormat();

        // Size everything from the swapchain extent. It is in pixels and can differ from the
        // window's logical size on HiDPI displays (B17).
        auto extent = ctx.getSwapchainExtent();
        swapchain.width = extent.x;
        swapchain.height = extent.y;

        for (auto &semaphore : swapchain.readyToPresentSemaphores)
        {
            ctx.destroySemaphore(semaphore);
        }
        swapchain.readyToPresentSemaphores.clear();

        for (size_t i = 0; i < swapchain.imageHandles.size(); ++i)
        {
            auto semaphore = ctx.createSemaphore();
            if (!semaphore.isValid())
            {
                spdlog::error("Failed to create ready-to-present semaphore for swapchain image {}", i);
                return false;
            }
            swapchain.readyToPresentSemaphores.push_back(semaphore);
        }

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            auto depthTextureDesc = ResourceDesc{};
            depthTextureDesc.type = ResourceType::TEXTURE;
            depthTextureDesc.name = "DepthTexture for Frame " + std::to_string(i);
            depthTextureDesc.texture.width = swapchain.width;
            depthTextureDesc.texture.height = swapchain.height;
            depthTextureDesc.texture.format = Format::D32_SFLOAT_S8_UINT; // TODO: check this format
            depthTextureDesc.texture.usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT;

            // The device is idle, so the old texture is unused.
            if (frameResources[i].depthTexture.isValid())
                ctx.destroyTexture(frameResources[i].depthTexture);
            frameResources[i].depthTexture = ctx.createTexture(depthTextureDesc);
            if (!frameResources[i].depthTexture.isValid())
            {
                spdlog::error("Failed to create depth texture for frame {}", i);
                return false;
            }
        }

        updateCameraAspectRatio(static_cast<float>(swapchain.width) / static_cast<float>(swapchain.height));
        return true;
    }

    void Engine::render2(SceneHandle scene, CameraHandle camera)
    {
        if (!scene.isValid() || scene.index >= MAX_SCENES)
        {
            spdlog::error("Invalid scene handle for preparation.");
            return ;
        }

        auto &sceneRef = registery.scenes[scene.index];

        if (!beginFrame(camera))
            return;
        beginPass();

        auto preparedScene = prepareScene(scene);

        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;

        auto &cameraRef = registery.cameras[camera.index];
        auto projection = getProjectionMatrix(camera);
        auto view = getViewMatrix(camera);

        ctx.bindPipeline(commandBuffer, preparedScene.uberMaterialPipeline);
        {
            gfx::SceneData sceneData{};
            sceneData.projection = projection;
            sceneData.view = view;
            sceneData.cameraWorldPosition = cameraRef.transform.position;
            sceneData.numLights = sceneRef.lightCount;
            for (uint32_t i = 0; i < sceneRef.lightCount; ++i)
            {
                LightHandle sceneLight = sceneRef.lights[i];
                Light sceneLightData = registery.lights[sceneLight.index];

                gfx::Light light {};
                light.position = sceneLightData.position;
                light.direction = sceneLightData.direction;
                light.color = sceneLightData.color;
                light.intensity = sceneLightData.intensity;
                light.range = sceneLightData.range;
                light.type = static_cast<gfx::LightType>(sceneLightData.type);

                sceneData.lights[i] = light;
            }
            sceneData.vertsPtr = ctx.getBufferDeviceAddress(preparedScene.megaVertexBuffer);
            sceneData.drawInfoPtr = ctx.getBufferDeviceAddress(preparedScene.drawInfoBuffer);
            sceneData.modeldataPtr = ctx.getBufferDeviceAddress(preparedScene.megaModelDataBuffer[frameCount]);

            ctx.fillBuffer(preparedScene.sceneDataBuffer[frameCount], &sceneData, sizeof(sceneData), 0);

            updateUniforms(preparedScene);

            ctx.bindDescriptorSet(commandBuffer, preparedScene.uberMaterialPipeline, bindlessDescriptorSet, 0);
            ctx.bindIndexBuffer(commandBuffer, preparedScene.megaIndexBuffer, 0, Format::U32_UINT);

            auto sceneDataBufferPtr = ctx.getBufferDeviceAddress(preparedScene.sceneDataBuffer[frameCount]);
            ctx.pushConstants(commandBuffer, preparedScene.uberMaterialPipeline, ShaderType::VERTEX_FRAGMENT, &sceneDataBufferPtr, sizeof(uint64_t), 0);
            ctx.drawIndexedIndirect(commandBuffer, preparedScene.drawInfoBuffer, 0, preparedScene.drawInfoCount, sizeof(gfx::DrawInfo));
        }
        endPass();
        endFrame();
    }

    void Engine::render(SceneHandle scene, CameraHandle camera)
    {
        if (!beginFrame(camera))
            return;
        beginPass();

        auto compiledScene = compileScene(scene);
        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;
        auto shaderDataBuffer = currentFrame.shaderDataBuffer;

        struct ShaderData
        {
            glm::mat4 projection;
            glm::mat4 view;
            glm::mat4 model;
            uint32_t textureIndex;
        };

        ShaderData shaderData{};

        shaderData.projection = getProjectionMatrix(camera);
        shaderData.view = getViewMatrix(camera);

        auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

        auto shaderDataIdx = 0;
        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            auto pipeline = compiledScene.materialToPipeline[materialHandle];
            ctx.bindPipeline(commandBuffer, pipeline);
            ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);

            auto mat = getMaterial(materialHandle);
            shaderData.textureIndex = getBindlessTextureIndex(mat.textures[0]); // TODO: handle cases where material has more than 1 texture

            for (const auto &entityHandle : entitySet)
            {
                auto modelTransform = registery.entities[entityHandle.index].transform;

                shaderData.model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                                   glm::mat4_cast(modelTransform.rotation) *
                                   glm::scale(glm::mat4(1.0f), modelTransform.scale);

                auto baseOffset = sizeof(ShaderData) * shaderDataIdx;
                ctx.fillBuffer(shaderDataBuffer, &shaderData, sizeof(shaderData), baseOffset);

                auto bufferHandle = compiledScene.meshData[entityHandle];
                auto desc = ctx.getResourceDesc(bufferHandle);
                ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.offset, Format::U32_UINT);
                auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX_FRAGMENT, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
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

        struct ShaderData
        {
            glm::mat4 projection;
            glm::mat4 view;
            glm::mat4 model;
            uint32_t textureIndex;
        };

        ShaderData shaderData{};

        shaderData.projection = getProjectionMatrix(camera);
        shaderData.view = getViewMatrix(camera);

        auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

        auto shaderDataIdx = 0;
        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            auto pipeline = compiledScene.materialToPipeline[materialHandle];
            ctx.bindPipeline(commandBuffer, pipeline);
            ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet);

            auto mat = getMaterial(materialHandle);
            shaderData.textureIndex = getBindlessTextureIndex(mat.textures[0]); // TODO: handle cases where material has more than 1 texture

            for (const auto &entityHandle : entitySet)
            {
                auto modelTransform = registery.entities[entityHandle.index].transform;

                shaderData.model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                                   glm::mat4_cast(modelTransform.rotation) *
                                   glm::scale(glm::mat4(1.0f), modelTransform.scale);

                auto baseOffset = sizeof(ShaderData) * shaderDataIdx;
                ctx.fillBuffer(shaderDataBuffer, &shaderData, sizeof(shaderData), baseOffset);

                auto bufferHandle = compiledScene.meshData[entityHandle];
                auto desc = ctx.getResourceDesc(bufferHandle);
                ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.offset, Format::U32_UINT);
                auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX_FRAGMENT, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
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

    const Window& Engine::getMainWindow() const
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

    uint32_t Engine::addBindlessTexture(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, uint32_t slot)
    {
        auto txtID = ctx.updateBindlessDescriptorSet(bindlessSet, texture, slot);
        if (txtID == UINT32_MAX)
        {
            spdlog::error("Failed to add bindless texture.");
            return UINT32_MAX;
        }
        registery.bindlessTextureIndexMap[texture] = txtID;
        return txtID;
    }

    uint32_t Engine::getBindlessTextureIndex(const TextureHandle &texture)
    {
        auto it = registery.bindlessTextureIndexMap.find(texture);
        if (it != registery.bindlessTextureIndexMap.end())
        {
            return it->second;
        }
        return UINT32_MAX;
    }

    RenderTargetAttachments Engine::getRenderTargetAttachments(const RenderTargetHandle &renderTarget)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        return rt.attachments[frameCount % MAX_FRAMES_IN_FLIGHT];
    }

} // namespace rasm
