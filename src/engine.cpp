#include "rasm/engine.h"
#include "platform/paths.h"
#include "renderer/geometryPool.h"
#include "renderer/frameStats.h"
#include "renderer/gpuScene.h"
#include "renderer/shaderCompiler.h"
#include "renderer/statsOverlay.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "stb_image.h"

#include "glm/glm.hpp"

#include <chrono>
#include <future>
#include <vector>

namespace rasm
{
    namespace
    {
        constexpr glm::vec4 SWAPCHAIN_CLEAR_COLOR(0.018f, 0.018f, 0.018f, 1.0f);
        // Main-thread time a frame spends finishing loads. The load that crosses it still finishes.
        constexpr std::chrono::milliseconds LOAD_BUDGET(4);

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

    Engine::Engine(const EngineConfig &config) : config(config), geometryPool(std::make_unique<GeometryPool>()), gpuScene(std::make_unique<GpuScene>()), frameStats(std::make_unique<FrameStatsCollector>()), statsOverlay(std::make_unique<StatsOverlay>()), ctx(this), shaderCompiler(std::make_unique<ShaderCompiler>())
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

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            frameResources[i].commandBuffer = ctx.createCommandBuffer("Frame Command Buffer " + std::to_string(i));
            if (!frameResources[i].commandBuffer.isValid())
            {
                spdlog::error("Failed to create command buffer for frame {}", i);
                return;
            }
        }

        auto shaderTarget = getShaderCompilerTarget(config.preferredBackend);
        if (!shaderCompiler->initialize(shaderTarget, this->config.shaderDir))
        {
            spdlog::error("Failed to initialize shader compiler.");
            return;
        }

        if (!geometryPool->create(ctx, this->config) || !createGpuScene())
            return;
        createFrameStats();

        // Create bindless texture descriptor set layout, pool, and set
        bindlessDescriptorSetLayout = ctx.createBindlessDescriptorSetLayout({.name = "BindlessDescriptorSetLayout",
                                                                             .stage = ShaderType::ALL,
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

        // Swapchain images and depth textures. The depth textures go in the bindless set, so they come after it.
        if (!createSwapchainResources())
            return;

        // Its pipeline is built with the bindless layout, so it comes after it.
        if (!createStatsOverlay())
            return;

        // The glTF default material: white base color, metallic 1, roughness 1.
        defaultMaterial = createMaterial(MaterialType::PBR);
        if (!defaultMaterial.isValid())
        {
            spdlog::error("Failed to create the default material.");
            return;
        }
        Material defaultValues = getMaterial(defaultMaterial);
        defaultValues.name = "default";
        defaultValues.params[static_cast<uint32_t>(PbrParam::METALLIC)] = 1.0f;
        updateMaterial(defaultMaterial, defaultValues);

        isRunning = true;
    }

    Engine::~Engine()
    {
        // Mesh results free themselves; decoded pixels are a plain stbi pointer.
        for (auto &[handle, load] : pendingTextures)
            stbi_image_free(load.result.get().data);

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

    void Engine::waitForLoads()
    {
        finishLoads(true);
    }

    // Runs at the start of a frame, before anything binds the bindless set or the geometry pool, so
    // what it writes is in place for this frame's draws and its uploads go out with this frame's submit.
    // Without wait it stops at LOAD_BUDGET, so loads that finish together spread over several frames.
    void Engine::finishLoads(bool wait)
    {
        const auto deadline = std::chrono::steady_clock::now() + LOAD_BUDGET;
        auto overBudget = [wait, deadline]()
        { return !wait && std::chrono::steady_clock::now() >= deadline; };

        for (auto it = pendingTextures.begin(); it != pendingTextures.end() && !overBudget();)
        {
            if (!wait && it->second.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                ++it;
                continue;
            }
            finishTextureLoad(it->second);
            it = pendingTextures.erase(it);
            // syncGpuScene checks the entities that wait for textures only after one finished.
            gpuScene->texturesFinished = true;
        }

        for (size_t i = 0; i < pendingMeshes.size() && !overBudget();)
        {
            if (!wait && pendingMeshes[i].result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                ++i;
                continue;
            }
            finishMeshLoad(pendingMeshes[i]);
            pendingMeshes.erase(pendingMeshes.begin() + i);
        }
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
        const Format colorFormats[MAX_COLOR_ATTACHMENTS] = {ctx.getSwapchainImageFormat()};
        return createGraphicsPipeline(name, shaderSource, colorFormats, DEPTH_FORMAT);
    }

    // The pipeline draws into targets with the formats of this one: its colors, in order, and its depth.
    PipelineHandle Engine::createPipeline(const std::string &name, const std::string &shaderSource, const RenderTargetHandle &renderTarget)
    {
        const auto &rt = ctx.getRenderTarget(renderTarget);
        if (rt.colorCount == 0)
        {
            spdlog::error("Pipeline '{}' needs a render target with color formats.", name);
            return {};
        }
        return createGraphicsPipeline(name, shaderSource, rt.colorFormats, rt.depthFormat);
    }

    PipelineHandle Engine::createGraphicsPipeline(const std::string &name, const std::string &shaderSource, const Format *colorFormats, Format depthFormat)
    {
        ZoneScoped;

        const char *vertEntryPoint = "vertMain";
        const char *fragEntryPoint = "fragMain";
        auto vertShader = shaderCompiler->compileFromString(shaderSource, name + ".vert.slang", vertEntryPoint);
        auto fragShader = shaderCompiler->compileFromString(shaderSource, name + ".frag.slang", fragEntryPoint);

        if (vertShader.code.empty() || fragShader.code.empty())
        {
            spdlog::error("Failed to compile the shaders of pipeline '{}'.", name);
            return {};
        }

        PipelineDesc desc = {.name = "Pipeline for " + name,
                             .vertexShader = ctx.createShader({.name = name + ".vert", .type = ShaderType::VERTEX, .entryPoint = vertEntryPoint, .code = std::move(vertShader.code)}),
                             .fragmentShader = ctx.createShader({.name = name + ".frag", .type = ShaderType::FRAGMENT, .entryPoint = fragEntryPoint, .code = std::move(fragShader.code)}),
                             .descriptorSetLayout = bindlessDescriptorSetLayout,
                             .topology = PrimitiveTopology::TRIANGLE_LIST,
                             .depthStencilAttachmentFormat = depthFormat};
        std::copy(colorFormats, colorFormats + MAX_COLOR_ATTACHMENTS, std::begin(desc.colorAttachmentFormats));
        return ctx.createPipeline(desc);
    }

    PipelineHandle Engine::createComputePipeline(const std::string &name, const std::string &shaderSource, const std::string &entryPoint)
    {
        ZoneScoped;

        auto compShader = shaderCompiler->compileFromString(shaderSource, name + ".comp.slang", entryPoint);
        ShaderHandle shader;
        if (!compShader.code.empty())
            shader = ctx.createShader({.name = name + ".comp", .type = ShaderType::COMPUTE, .entryPoint = entryPoint, .code = std::move(compShader.code)});
        // An invalid handle would turn the desc into a graphics pipeline with no shaders.
        if (!shader.isValid())
        {
            spdlog::error("Failed to create the shader of compute pipeline '{}'.", name);
            return {};
        }

        return ctx.createPipeline({.name = "Compute Pipeline for " + name, .computeShader = shader, .descriptorSetLayout = bindlessDescriptorSetLayout});
    }

    bool Engine::beginFrame(CameraHandle camera)
    {
        ZoneScoped;

        pollEvents(camera);
        finishLoads(false);

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

        // After the wait: they read what the slot's last frame measured and rewrite the tables it read.
        updateFrameStats();
        updateStatsOverlay();
        syncGpuScene();

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
        frameStats->frameTimer = beginGpuTimer(currentFrame.commandBuffer, "Frame");
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
            beginPass("Clear");
            endPass();
            setRenderTarget(renderTarget);
        }
        drawStatsOverlay();
        ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::PRESENT_SRC);

        endGpuTimer(currentFrame.commandBuffer, frameStats->frameTimer);
        ctx.endCommandBuffer(currentFrame.commandBuffer);

        // The frame's scene draws read what culling writes, so its command buffer goes first. A frame whose culling
        // failed to record is not submitted, since its draws would read stale counts.
        bool culling = false;
        const bool cullingRecorded = recordCulling(culling);
        const CommandBufferHandle commandBuffers[] = {gpuScene->cullCommandBuffers[frameCount], currentFrame.commandBuffer};
        const uint32_t firstCommandBuffer = culling ? 0 : 1;

        // The slot only takes the new value once a submit will signal it; a value nothing signals would block
        // the next wait on this slot forever. After a failed submit the swapchain keeps the image acquired,
        // and the next acquire hands it out again.
        const uint64_t signalValue = frameFenceValue + 1;
        if (cullingRecorded && ctx.submit(commandBuffers + firstCommandBuffer, 2 - firstCommandBuffer, frameFence, signalValue, true))
        {
            frameFenceValue = signalValue;
            currentFrame.fenceValue = signalValue;

            auto status = ctx.present();
            if (status == SwapchainStatus::SUBOPTIMAL || status == SwapchainStatus::OUT_OF_DATE)
                swapchainOutOfDate = true;
        }
        else
        {
            ctx.discardRecording();
        }

        frameCount = (frameCount + 1) % MAX_FRAMES_IN_FLIGHT; // Toggle between 0 and 1 for double buffering

        FrameMark;
    }

    void Engine::beginPass(const std::string &name, const AttachmentOps &ops)
    {
        auto currentFrame = frameResources[frameCount];
        frameStats->passTimer = beginGpuTimer(currentFrame.commandBuffer, name);
        uint32_t width = swapchain.width;
        uint32_t height = swapchain.height;
        TextureHandle colors[MAX_COLOR_ATTACHMENTS] = {swapchain.imageHandles[imageIdx]};
        uint32_t colorCount = 1;
        TextureHandle depth = currentFrame.depthTexture;
        glm::vec4 clearColor = SWAPCHAIN_CLEAR_COLOR;
        if (currentRenderTarget.isValid())
        {
            const auto &renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            width = renderTargetData.width;
            height = renderTargetData.height;
            colorCount = renderTargetData.colorCount;
            std::copy(std::begin(renderTargetData.attachments[frameCount].color), std::end(renderTargetData.attachments[frameCount].color), std::begin(colors));
            depth = renderTargetData.attachments[frameCount].depth;
            clearColor = renderTargetData.clearColor;
        }

        // What the pass clears is dropped. drawScene's occlusion culling pauses the pass and builds its Hi-Z from the
        // depth drawn so far, so it needs the depth stored.
        for (uint32_t i = 0; i < colorCount; ++i)
            ctx.transitionImageLayout(currentFrame.commandBuffer, colors[i], TextureUsage::COLOR_ATTACHMENT, !ops.loadColor);
        ctx.transitionImageLayout(currentFrame.commandBuffer, depth, TextureUsage::DEPTH_STENCIL_ATTACHMENT, !ops.loadDepth);
        ctx.beginRendering(currentFrame.commandBuffer, colors, colorCount, depth, clearColor,
                           {.loadColor = ops.loadColor, .loadDepth = ops.loadDepth, .storeDepth = ops.storeDepth || config.occlusionCulling});

        // begin dynamic rendering
        ctx.setViewport(currentFrame.commandBuffer, 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        ctx.setScissor(currentFrame.commandBuffer, 0, 0, width, height);
        // Every pipeline takes its cull mode from the command buffer; drawScene changes it and restores this.
        ctx.setCullMode(currentFrame.commandBuffer, CullMode::NONE);
    }

    void Engine::endPass()
    {
        auto currentFrame = frameResources[frameCount];
        ctx.endRendering(currentFrame.commandBuffer);

        if (currentRenderTarget.isValid())
        {
            // Later passes sample the color attachment through the bindless set, whose descriptors promise
            // this layout (B10). Depth stays an attachment: only drawScene's Hi-Z reads it, inside the pass.
            const auto &renderTargetData = ctx.getRenderTarget(currentRenderTarget);
            for (uint32_t i = 0; i < renderTargetData.colorCount; ++i)
                ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.attachments[frameCount].color[i], TextureUsage::SAMPLED);
        }
        else
        {
            // endFrame moves it to PRESENT_SRC.
            swapchainRendered = true;
        }
        endGpuTimer(currentFrame.commandBuffer, frameStats->passTimer);
    }

    void Engine::setRenderTarget(const RenderTargetHandle &renderTarget)
    {
        this->currentRenderTarget = renderTarget;
    }

    void Engine::resetRenderTarget()
    {
        this->currentRenderTarget = RenderTargetHandle{};
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

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            const TextureDesc depthTextureDesc = {.name = "DepthTexture for Frame " + std::to_string(i),
                                                  .width = swapchain.width,
                                                  .height = swapchain.height,
                                                  .format = DEPTH_FORMAT,
                                                  .usage = config.occlusionCulling ? TextureUsage::SAMPLED_DEPTH_ATTACHMENT : TextureUsage::DEPTH_STENCIL_ATTACHMENT};

            // The device is idle, so the old texture is unused, and the new one takes its bindless slot.
            uint32_t depthSlot = MAX_BINDLESS_TEXTURES;
            if (frameResources[i].depthTexture.isValid())
            {
                const uint32_t oldSlot = getBindlessTextureIndex(frameResources[i].depthTexture);
                if (oldSlot != UINT32_MAX)
                    depthSlot = oldSlot;
                registry.bindlessTextureIndexMap.erase(frameResources[i].depthTexture);
                ctx.destroyTexture(frameResources[i].depthTexture);
            }
            frameResources[i].depthTexture = ctx.createTexture(depthTextureDesc);
            if (!frameResources[i].depthTexture.isValid())
            {
                spdlog::error("Failed to create depth texture for frame {}", i);
                return false;
            }

            // drawScene's occlusion culling samples it. Without a slot that call is only frustum culled.
            if (config.occlusionCulling)
                addBindlessTexture(bindlessDescriptorSet, frameResources[i].depthTexture, SamplerType::NEAREST_CLAMP, depthSlot);
        }

        ctx.resizeRenderTargets();
        updateCameraAspectRatio(static_cast<float>(swapchain.width) / static_cast<float>(swapchain.height));
        return true;
    }

    void Engine::render(SceneHandle scene, CameraHandle camera)
    {
        ZoneScoped;

        if (!beginFrame(camera))
            return;
        beginPass("Scene");
        drawScene(scene, camera);
        endPass();
        endFrame();
    }

    bool Engine::running() const
    {
        return isRunning;
    }

    VRAMStats Engine::queryMemoryStats()
    {
        return ctx.queryVRAMStats();
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

    uint32_t Engine::addBindlessTexture(const DescriptorSetHandle &bindlessSet, const TextureHandle &texture, SamplerType sampler, uint32_t slot)
    {
        auto txtID = ctx.updateBindlessDescriptorSet(bindlessSet, texture, sampler, slot);
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

    TextureHandle Engine::getRenderTargetTexture(const RenderTargetHandle &renderTarget, uint32_t attachment)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        if (attachment >= rt.colorCount)
            return {};
        return rt.attachments[frameCount % MAX_FRAMES_IN_FLIGHT].color[attachment];
    }

    TextureHandle Engine::getPreviousRenderTargetTexture(const RenderTargetHandle &renderTarget, uint32_t attachment)
    {
        auto &rt = ctx.getRenderTarget(renderTarget);
        if (!rt.history || attachment >= rt.colorCount)
            return {};
        return rt.attachments[(frameCount + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT].color[attachment];
    }

} // namespace rasm
