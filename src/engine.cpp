#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/shaderCompiler.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "stb_image.h"
#include "stb_image_write.h"

#include "glm/glm.hpp"

namespace rasm
{
    namespace
    {
        constexpr Format DEPTH_FORMAT = Format::D32_SFLOAT_S8_UINT; // TODO: check this format
        constexpr glm::vec4 SWAPCHAIN_CLEAR_COLOR(0.018f, 0.018f, 0.018f, 1.0f);

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
            case Backend::WEBGPU:
                return ShaderCompiler::Target::WGSL;
            default:
                spdlog::error("Unsupported backend for shader compilation.");
                return ShaderCompiler::Target::SPIRV; // Default to SPIR-V
            }
        }
    }

    Engine::Engine(const EngineConfig &config) : config(config), ctx(this), shaderCompiler(std::make_unique<ShaderCompiler>())
    {
        // In a real implementation, this is where we'd initialize the window, graphics context, etc.
        spdlog::info("initializing engine with config: appName={}, windowWidth={}, windowHeight={}, enableValidation={}",
                     config.appName, config.windowWidth, config.windowHeight, config.enableValidation);

        this->config.shaderDir = resolveFromExecutableDir(config.shaderDir.empty() ? RASM_SHADER_DIR : config.shaderDir);
        if (!config.assetRoot.empty())
        {
            this->config.assetRoot = resolveFromExecutableDir(config.assetRoot);
        }

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

        // Each frame submit signals the next value; a slot is free again once its last value is reached.
        frameFence = ctx.createFence("Frame Fence");
        if (!frameFence.isValid())
        {
            spdlog::error("Failed to create the frame fence.");
            return;
        }

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            frameResources[i].commandBuffer = ctx.createCommandBuffer("Frame Command Buffer " + std::to_string(i));
            if (!frameResources[i].commandBuffer.isValid())
            {
                spdlog::error("Failed to create command buffer for frame {}", i);
                return;
            }
        }

        // Swapchain images and depth textures
        if (!createSwapchainResources())
            return;

        auto shaderTarget = getShaderCompilerTarget(config.preferredBackend);
        if (!shaderCompiler->initialize(shaderTarget, this->config.shaderDir))
        {
            spdlog::error("Failed to initialize shader compiler.");
            return;
        }

        // Create bindless texture descriptor set layout, pool, and set
        bindlessDescriptorSetLayout = ctx.createBindlessDescriptorSetLayout({.name = "BindlessDescriptorSetLayout",
                                                                             .stage = ShaderType::FRAGMENT,
                                                                             .count = MAX_BINDLESS_TEXTURES,
                                                                             .type = DescriptorType::COMBINED_IMAGE_SAMPLER});
        if (!bindlessDescriptorSetLayout.isValid())
        {
            spdlog::error("Failed to create bindless descriptor set layout.");
            return;
        }

        bindlessDescriptorPool = ctx.createDescriptorPool({.name = "BindlessDescriptorPool",
                                                           .sizes = {{.type = DescriptorType::COMBINED_IMAGE_SAMPLER, .count = MAX_BINDLESS_TEXTURES}},
                                                           .maxSets = 1});
        if (!bindlessDescriptorPool.isValid())
        {
            spdlog::error("Failed to create bindless descriptor pool.");
            return;
        }

        bindlessDescriptorSet = ctx.allocateDescriptorSet("BindlessDescriptorSet", bindlessDescriptorPool, bindlessDescriptorSetLayout);
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
        registry.materials[defaultMaterial.index].name = "default";
        registry.materials[defaultMaterial.index].params[static_cast<uint32_t>(PbrParam::METALLIC)] = 1.0f;

        isRunning = true;
    }

    Engine::~Engine()
    {
        // Clean up resources, free memory, etc.
        for (auto &[handle, tex] : registry.textureData)
        {
            stbi_image_free(tex.data);
        }

        ctx.cleanup();

        destroyWindow(this->mainWindow);
        spdlog::info("Engine shutdown, cleaned up resources.");
    }

    std::string Engine::resolveAssetPath(const std::string &path) const
    {
        // Every loader expects UTF-8, whichever encoding the path arrived in.
        if (config.assetRoot.empty())
        {
            return pathToUtf8(pathFromUtf8(path));
        }
        return pathToUtf8((pathFromUtf8(config.assetRoot) / pathFromUtf8(path)).lexically_normal());
    }

    TextureHandle Engine::loadTexture(const std::string &assetPath, Format format)
    {
        if (format != Format::R8G8B8A8_SRGB && format != Format::R8G8B8A8_UNORM)
        {
            spdlog::error("loadTexture requires RGBA8 sRGB or UNORM format.");
            return {};
        }
        const std::string path = resolveAssetPath(assetPath);
        const auto cacheKey = path + "#" + std::to_string(static_cast<int>(format));
        if (registry.loadedTextures.find(cacheKey) != registry.loadedTextures.end())
        {
            return registry.loadedTextures[cacheKey];
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

        auto textureHandle = ctx.createTexture({.name = path, .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height), .format = format, .usage = TextureUsage::SAMPLED});
        if (!textureHandle.isValid())
        {
            spdlog::error("Failed to create texture: {}", path);
            stbi_image_free(data);
            return TextureHandle{};
        }
        if (!ctx.fillTexture(textureHandle, data))
        {
            spdlog::error("Failed to fill texture: {}", path);
            ctx.destroyTexture(textureHandle);
            stbi_image_free(data);
            return TextureHandle{};
        }

        registry.loadedTextures[cacheKey] = textureHandle;
        registry.textureData.insert({textureHandle, std::move(tex)});

        return textureHandle;
    }

    TextureHandle Engine::createTexture(const TextureDesc &desc)
    {
        return ctx.createTexture(desc);
    }

    BufferHandle Engine::createBuffer(const BufferDesc &desc)
    {
        return ctx.createBuffer(desc);
    }

    RenderTargetHandle Engine::createRenderTarget(const RenderTargetDesc &desc)
    {
        return ctx.createRenderTarget(desc);
    }

    RenderGraph Engine::createRenderGraph()
    {
        RenderGraph graph(this);
        return graph;
    }

    PipelineHandle Engine::createPipeline(const std::string &name, const std::string &shaderSource)
    {
        ZoneScoped;

        auto vertShader = shaderCompiler->compileFromString(shaderSource, name + ".vert.slang", "vertMain");
        auto fragShader = shaderCompiler->compileFromString(shaderSource, name + ".frag.slang", "fragMain");

        if (vertShader.code.empty() || fragShader.code.empty())
        {
            spdlog::error("Failed to compile the shaders of pipeline '{}'.", name);
            return {};
        }

        return ctx.createPipeline({.name = "Pipeline for " + name,
                                   .vertexShader = ctx.createShader({.name = name + ".vert", .type = ShaderType::VERTEX, .code = std::move(vertShader.code)}),
                                   .fragmentShader = ctx.createShader({.name = name + ".frag", .type = ShaderType::FRAGMENT, .code = std::move(fragShader.code)}),
                                   .descriptorSetLayout = bindlessDescriptorSetLayout,
                                   .topology = PrimitiveTopology::TRIANGLE_LIST,
                                   .colorAttachmentFormat = ctx.getSwapchainImageFormat(),
                                   .depthStencilAttachmentFormat = DEPTH_FORMAT});
    }

    bool Engine::beginFrame(CameraHandle camera)
    {
        ZoneScoped;

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

        // Wait for the GPU to finish the previous frame in this slot before its command buffer is reused.
        auto &currentFrame = frameResources[frameCount];
        if (!ctx.waitForFence(frameFence, currentFrame.fenceValue))
            return false;

        // B4
        auto status = ctx.acquireNextImage(imageIdx);
        if (status == SwapchainStatus::OUT_OF_DATE)
        {
            swapchainOutOfDate = true;
            return false;
        }
        if (status == SwapchainStatus::FAILURE)
            return false;
        if (status == SwapchainStatus::SUBOPTIMAL)
            swapchainOutOfDate = true; // The image is acquired and still usable; recreate after this frame.

        swapchainRendered = false;
        sceneDrawCount = 0;

        // record commands for the current frame
        ctx.beginCommandBuffer(currentFrame.commandBuffer);
        return true;
    }

    void Engine::endFrame()
    {
        ZoneScoped;

        auto &currentFrame = frameResources[frameCount];

        // Present needs the image in PRESENT_SRC, so a frame that only drew to render targets clears it.
        if (!swapchainRendered)
        {
            auto renderTarget = currentRenderTarget;
            resetRenderTarget();
            beginPass();
            endPass();
            setRenderTarget(renderTarget);
        }

        ctx.endCommandBuffer(currentFrame.commandBuffer);

        // The slot only takes the new value once a submit will signal it; a value nothing signals would block
        // the next wait on this slot forever. After a failed submit the swapchain keeps the image acquired,
        // and the next acquire hands it out again.
        const uint64_t signalValue = frameFenceValue + 1;
        if (ctx.submit(currentFrame.commandBuffer, frameFence, signalValue, true))
        {
            frameFenceValue = signalValue;
            currentFrame.fenceValue = signalValue;

            auto status = ctx.present();
            if (status == SwapchainStatus::SUBOPTIMAL || status == SwapchainStatus::OUT_OF_DATE)
                swapchainOutOfDate = true;
        }

        frameCount = (frameCount + 1) % MAX_FRAMES_IN_FLIGHT; // Toggle between 0 and 1 for double buffering

        FrameMark;
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
            ctx.beginRendering(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].color, renderTargetData.attachments[frameCount].depth, renderTargetData.clearColor);
        }
        else
        {
            // transition swapchain image to be ready for rendering
            ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
            ctx.transitionImageLayout(currentFrame.commandBuffer, currentFrame.depthTexture, TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);
            ctx.beginRendering(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], currentFrame.depthTexture, SWAPCHAIN_CLEAR_COLOR);
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
            // Later passes sample the color attachment through the bindless set, whose descriptors promise
            // this layout (B10). Depth is not stored, so it stays an attachment.
            const auto &renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].color, TextureUsage::COLOR_ATTACHMENT, TextureUsage::SAMPLED);
        }
        else
        {
            ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::COLOR_ATTACHMENT, TextureUsage::PRESENT_SRC);
            swapchainRendered = true;
        }
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
        if (status != SwapchainStatus::OK)
        {
            // A 0x0 surface keeps the old swapchain, while a failure may have destroyed it with its images.
            swapchain.imageHandles = ctx.getSwapchainImages();
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

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            const TextureDesc depthTextureDesc = {.name = "DepthTexture for Frame " + std::to_string(i),
                                                  .width = swapchain.width,
                                                  .height = swapchain.height,
                                                  .format = DEPTH_FORMAT,
                                                  .usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT};

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

    void Engine::render(SceneHandle scene, CameraHandle camera)
    {
        ZoneScoped;

        if (!beginFrame(camera))
            return;
        beginPass();
        drawScene(scene, camera);
        endPass();
        endFrame();
    }

    void Engine::drawScene(SceneHandle scene, CameraHandle camera)
    {
        ZoneScoped;

        const auto &preparedScene = prepareScene(scene);

        // An invalid or empty scene, or a failed preparation (which prepareScene logged and retries next
        // frame), has no pipeline to bind; the pass keeps its cleared attachments.
        if (!preparedScene.uberMaterialPipeline.isValid())
            return;

        uint32_t width = swapchain.width;
        uint32_t height = swapchain.height;

        if (currentRenderTarget.isValid())
        {
            const auto &renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            width = renderTargetData.width;
            height = renderTargetData.height;

            // The pipeline's formats must match the attachments' for the draw to be defined.
            if (renderTargetData.colorFormat != swapchain.imageFormat || renderTargetData.depthFormat != DEPTH_FORMAT)
            {
                spdlog::error("drawScene needs a render target with the swapchain's color format and D32_SFLOAT_S8_UINT depth.");
                return;
            }
        }

        // The GPU reads SceneData after the whole frame is recorded, so every call needs its own slot.
        if (sceneDrawCount >= MAX_SCENE_DRAWS_PER_FRAME)
        {
            spdlog::error("Exceeded maximum number of drawScene calls per frame.");
            return;
        }
        const uint64_t sceneDataOffset = sceneDrawCount++ * sizeof(gfx::SceneData);

        auto &sceneRef = registry.scenes[scene.index];
        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;

        auto &cameraRef = registry.cameras[camera.index];
        auto projection = getProjectionMatrix(camera, static_cast<float>(width) / static_cast<float>(height));
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
                Light sceneLightData = registry.lights[sceneLight.index];

                gfx::Light light{};
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

            ctx.fillBuffer(preparedScene.sceneDataBuffer[frameCount], &sceneData, sizeof(sceneData), sceneDataOffset);

            updateUniforms(preparedScene);

            ctx.bindDescriptorSet(commandBuffer, preparedScene.uberMaterialPipeline, bindlessDescriptorSet, 0);
            ctx.bindIndexBuffer(commandBuffer, preparedScene.megaIndexBuffer, 0, Format::U32_UINT);

            auto sceneDataBufferPtr = ctx.getBufferDeviceAddress(preparedScene.sceneDataBuffer[frameCount]) + sceneDataOffset;
            ctx.pushConstants(commandBuffer, preparedScene.uberMaterialPipeline, ShaderType::VERTEX_FRAGMENT, &sceneDataBufferPtr, sizeof(uint64_t), 0);
            ctx.drawIndexedIndirect(commandBuffer, preparedScene.drawInfoBuffer, 0, preparedScene.drawInfoCount, sizeof(gfx::DrawInfo));
        }
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

    const Window &Engine::getMainWindow() const
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
        registry.bindlessTextureIndexMap[texture] = txtID;
        return txtID;
    }

    uint32_t Engine::getBindlessTextureIndex(const TextureHandle &texture)
    {
        auto it = registry.bindlessTextureIndexMap.find(texture);
        if (it != registry.bindlessTextureIndexMap.end())
        {
            return it->second;
        }
        return UINT32_MAX;
    }

    TextureHandle Engine::getRenderTargetTexture(const RenderTargetHandle &renderTarget)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        return rt.attachments[frameCount % MAX_FRAMES_IN_FLIGHT].color;
    }

} // namespace rasm
