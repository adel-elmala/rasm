#include "renderer/ui.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "platform/uiPlatform.h"
#include "renderer/inspector.h"
#include "renderer/shaderCompiler.h"
#include "renderer/uberMaterialTypes.h"

#include "imgui.h"
#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace rasm
{
    namespace
    {
        constexpr uint64_t MIN_VERTEX_BYTES = 64u << 10;
        constexpr uint64_t MIN_INDEX_BYTES = 16u << 10;
        // An ImTextureID is the bindless slot + 1 (0 is ImGui's "no texture"), with this bit for a texture in an sRGB
        // format. The push constant carries the slot with SRGB_TEXTURE_BIT.
        constexpr uint64_t SRGB_TEXTURE_ID = 1ull << 32;
        constexpr uint32_t SRGB_TEXTURE_BIT = 1u << 31;

        // shaders/passes/ui.slang pulls ImDrawVert as UiVertex and the indices are 16-bit.
        static_assert(sizeof(ImDrawVert) == 20 && offsetof(ImDrawVert, uv) == 8 && offsetof(ImDrawVert, col) == 16, "ImDrawVert must match UiVertex in shaders/passes/ui.slang");
        static_assert(sizeof(ImDrawIdx) == 2, "the UI pass binds 16-bit indices");
    }

    namespace
    {
        bool isSrgb(Format format)
        {
            return format == Format::R8G8B8A8_SRGB || format == Format::B8G8R8A8_SRGB || format == Format::BC7_SRGB || format == Format::ASTC_4x4_SRGB ||
                   format == Format::ETC2_R8G8B8A8_SRGB;
        }

        // A UI buffer that is too small is made again, one size up. Only its frame slot uses it, and the slot's fence
        // was waited for, so the old one can go at once.
        bool reserveUiBuffer(RenderContext &ctx, BufferHandle &buffer, uint64_t &capacity, uint64_t size, uint64_t minimum, BufferUsage usage, const std::string &name)
        {
            if (buffer.isValid() && size <= capacity)
                return true;
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
            capacity = growUiCapacity(size, minimum);
            buffer = ctx.createBuffer({.name = name, .usage = usage, .size = capacity, .hostVisible = true});
            if (!buffer.isValid())
            {
                capacity = 0;
                spdlog::error("Failed to create the UI's {} of {} bytes.", name, growUiCapacity(size, minimum));
                return false;
            }
            return true;
        }

        // Queues the UI texture of id for release. The frames that may draw with it are the ones submitted so far, so it
        // can go once the graphics fence reaches releaseValue, their last value.
        void releaseUiTexture(Ui &ui, ImTextureID id, uint64_t releaseValue)
        {
            const auto slot = static_cast<uint32_t>((id & ~SRGB_TEXTURE_ID) - 1);
            ui.releases.push_back({.texture = ui.textures[slot], .slot = slot, .fenceValue = releaseValue});
            ui.textures.erase(slot);
        }

        // A texture ImGui asks for, or one that changed: a new texture with all of its pixels in a slot nobody samples,
        // and the old one released. A texture that can't be made keeps the request, so ImGui's old texture stays drawn
        // and the next frame tries again.
        void updateUiTexture(Engine &engine, RenderContext &ctx, Ui &ui, ImTextureData &texture, uint64_t releaseValue)
        {
            if (texture.Status == ImTextureStatus_WantDestroy)
            {
                if (texture.GetTexID() != ImTextureID_Invalid)
                    releaseUiTexture(ui, texture.GetTexID(), releaseValue);
                texture.SetTexID(ImTextureID_Invalid);
                texture.SetStatus(ImTextureStatus_Destroyed);
                return;
            }

            const auto width = static_cast<uint32_t>(texture.Width);
            const auto height = static_cast<uint32_t>(texture.Height);
            const void *pixels = texture.GetPixels();
            if (texture.Format == ImTextureFormat_Alpha8)
            {
                ui.expanded.resize(static_cast<size_t>(width) * height * 4);
                const auto *alpha = static_cast<const uint8_t *>(pixels);
                for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
                {
                    ui.expanded[i * 4] = 255;
                    ui.expanded[i * 4 + 1] = 255;
                    ui.expanded[i * 4 + 2] = 255;
                    ui.expanded[i * 4 + 3] = alpha[i];
                }
                pixels = ui.expanded.data();
            }

            const TextureHandle handle = ctx.createTexture({.name = "UI Texture " + std::to_string(texture.UniqueID), .width = width, .height = height, .format = Format::R8G8B8A8_UNORM, .mipLevels = 1});
            const TextureLevel level{.data = pixels, .size = static_cast<uint64_t>(width) * height * 4};
            if (!handle.isValid() || !ctx.fillTexture(handle, &level, 1))
            {
                spdlog::error("Failed to create a UI texture of {} x {}; trying again next frame.", width, height);
                if (handle.isValid())
                    ctx.destroyTexture(handle);
                return;
            }
            uint32_t slot = MAX_BINDLESS_TEXTURES;
            if (!ui.freeSlots.empty())
            {
                slot = ui.freeSlots.back();
                ui.freeSlots.pop_back();
            }
            const uint32_t index = engine.addBindlessTexture(engine.getBindlessDescriptorSet(), handle, SamplerType::LINEAR_CLAMP, slot);
            if (index == UINT32_MAX)
            {
                if (slot != MAX_BINDLESS_TEXTURES)
                    ui.freeSlots.push_back(slot);
                ctx.destroyTexture(handle);
                return;
            }
            if (texture.GetTexID() != ImTextureID_Invalid)
                releaseUiTexture(ui, texture.GetTexID(), releaseValue);
            ui.textures[index] = handle;
            texture.SetTexID(static_cast<ImTextureID>(index) + 1);
            texture.SetStatus(ImTextureStatus_OK);
        }
    }

    bool getUiScissor(const float clip[4], const float displayPos[2], const float framebufferScale[2], uint32_t width, uint32_t height, UiScissor &scissor)
    {
        const float left = std::max((clip[0] - displayPos[0]) * framebufferScale[0], 0.0f);
        const float top = std::max((clip[1] - displayPos[1]) * framebufferScale[1], 0.0f);
        const float right = std::min((clip[2] - displayPos[0]) * framebufferScale[0], static_cast<float>(width));
        const float bottom = std::min((clip[3] - displayPos[1]) * framebufferScale[1], static_cast<float>(height));
        if (right <= left || bottom <= top)
            return false;
        scissor.x = static_cast<int32_t>(left);
        scissor.y = static_cast<int32_t>(top);
        scissor.width = static_cast<uint32_t>(std::ceil(right)) - static_cast<uint32_t>(scissor.x);
        scissor.height = static_cast<uint32_t>(std::ceil(bottom)) - static_cast<uint32_t>(scissor.y);
        return true;
    }

    uint64_t growUiCapacity(uint64_t size, uint64_t minimum)
    {
        uint64_t capacity = std::max<uint64_t>(minimum, 1);
        while (capacity < size)
            capacity *= 2;
        return capacity;
    }

    void takeUiReleases(std::vector<UiRelease> &releases, uint64_t completedValue, std::vector<UiRelease> &done)
    {
        for (size_t i = 0; i < releases.size();)
        {
            if (completedValue >= releases[i].fenceValue)
            {
                done.push_back(releases[i]);
                releases[i] = releases.back();
                releases.pop_back();
            }
            else
                ++i;
        }
    }

    // Startup with EngineConfig::ui: the context, the SDL3 input backend (none when headless) and the pipeline. The font atlas is made by the
    // first frame's texture requests. A failure leaves the UI off and the engine running.
    bool Engine::createUi()
    {
        IMGUI_CHECKVERSION();
        ui = std::make_unique<Ui>();
        // The Buffers section's state, and the GPU tables section's struct layouts from the shaders compiled from now on.
        inspector = std::make_unique<Inspector>();
        shaderCompiler->keepStructLayouts();
        ui->context = ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        // No imgui.ini in the working directory (principle 5), and nothing that carries over between runs.
        io.IniFilename = nullptr;
        io.BackendRendererName = "rasm";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

        // The style and the fonts follow the display's scale, so the UI keeps its size on a 150 % or 4K display.
        const float scale = getUiContentScale(mainWindow);
        ImGuiStyle &style = ImGui::GetStyle();
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;

        ui->platform = initUiPlatform(mainWindow);
        // Dear ImGui's colors and its blending are meant for sRGB numbers. Through the swapchain's UNORM view the blend
        // works on them, as ImGui's own backends do; without one the shader decodes the colors and the blend is in
        // linear light, which makes light text on a dark window look a little bolder.
        const Format encodedFormat = ctx.getSwapchainEncodedFormat();
        ui->encoded = encodedFormat != Format::UNKNOWN;
        const Format colorFormats[MAX_COLOR_ATTACHMENTS] = {ui->encoded ? encodedFormat : ctx.getSwapchainImageFormat()};
        const std::string source = std::string(ui->encoded ? "#define RASM_UI_ENCODED 1\n" : "") + readFile(pathFromUtf8(config.shaderDir) / "passes/ui.slang");
        ui->pipeline = createGraphicsPipeline("ui", source, colorFormats, Format::UNKNOWN, "vertMain", "fragMain", BlendMode::PREMULTIPLIED_ALPHA, false);
        if (!ui->platform || !ui->pipeline.isValid())
        {
            spdlog::error("Failed to start the UI; it stays off.");
            destroyUi();
            return false;
        }
        spdlog::info("Dear ImGui {} is on, at a scale of {:.2f}; F1 shows the RASM window.", ImGui::GetVersion(), scale);
        return true;
    }

    void Engine::destroyUi()
    {
        if (!ui)
            return;
        if (ui->context)
        {
            // Every texture is freed with the context below, so ImGui must not ask for it again.
            for (ImTextureData *texture : ImGui::GetPlatformIO().Textures)
            {
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
            if (ui->platform)
                shutdownUiPlatform(mainWindow);
            ImGui::GetIO().BackendRendererName = nullptr;
            ImGui::GetIO().BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
            ImGui::DestroyContext(ui->context);
        }
        ui.reset();
        inspector.reset();
    }

    // Runs in beginFrame after the slot's fence wait and before syncGpuScene, so what the UI changes reaches this frame.
    // Without the RASM window and a callback nothing is built: no ImGui frame, no events, no draws.
    void Engine::buildUi(CameraHandle camera)
    {
        ui->draws.clear();
        ui->drawn = false;
        inspector->shown = false;
        const bool active = config.showStats || static_cast<bool>(ui->callback);
        if (!active)
        {
            ui->active = false;
            return;
        }
        ZoneScopedN("UI");
        // The UI got no events while it slept: keys would stay down and the mouse would be where it was.
        if (!ui->active)
            resumeUiInput(mainWindow);
        ui->active = true;

        newUiPlatformFrame(mainWindow, deltaTime);
        ImGui::NewFrame();
        drawFrozenFrustums(camera);
        if (config.showStats)
            drawEngineWindow();
        if (ui->callback)
            ui->callback();
        ImGui::Render();

        // Released textures that no submitted frame draws with any more give their slot back.
        std::vector<UiRelease> done;
        takeUiReleases(ui->releases, ctx.getFenceValue(frameFence), done);
        for (const UiRelease &release : done)
        {
            registry.bindlessTextureIndexMap.erase(release.texture);
            ctx.destroyTexture(release.texture);
            ui->freeSlots.push_back(release.slot);
        }

        const ImDrawData *drawData = ImGui::GetDrawData();
        if (drawData->Textures)
        {
            for (ImTextureData *texture : *drawData->Textures)
            {
                if (texture->Status != ImTextureStatus_OK)
                    updateUiTexture(*this, ctx, *ui, *texture, frameFenceValue);
            }
        }
        if (drawData->TotalVtxCount == 0 || drawData->TotalIdxCount == 0)
            return;

        UiSlot &slot = ui->slots[frameCount];
        const uint64_t vertexBytes = static_cast<uint64_t>(drawData->TotalVtxCount) * sizeof(ImDrawVert);
        const uint64_t indexBytes = static_cast<uint64_t>(drawData->TotalIdxCount) * sizeof(ImDrawIdx);
        if (!reserveUiBuffer(ctx, slot.vertices, slot.vertexBytes, vertexBytes, MIN_VERTEX_BYTES, BufferUsage::STORAGE, "UI Vertices " + std::to_string(frameCount)) ||
            !reserveUiBuffer(ctx, slot.indices, slot.indexBytes, indexBytes, MIN_INDEX_BYTES, BufferUsage::INDEX, "UI Indices " + std::to_string(frameCount)))
            return;

        const auto displayWidth = static_cast<uint32_t>(std::clamp(drawData->DisplaySize.x, 1.0f, 65535.0f));
        const auto displayHeight = static_cast<uint32_t>(std::clamp(drawData->DisplaySize.y, 1.0f, 65535.0f));
        ui->displaySize = displayWidth | displayHeight << 16;
        const float displayPos[2] = {drawData->DisplayPos.x, drawData->DisplayPos.y};
        const float framebufferScale[2] = {drawData->FramebufferScale.x, drawData->FramebufferScale.y};
        const uint64_t vertexAddress = ctx.getBufferDeviceAddress(slot.vertices);
        uint64_t vertexOffset = 0;
        uint32_t indexOffset = 0;
        for (const ImDrawList *list : drawData->CmdLists)
        {
            const auto listVertexBytes = static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
            const auto listIndexBytes = static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
            if (listVertexBytes == 0 || listIndexBytes == 0 ||
                !ctx.fillBuffer(slot.vertices, list->VtxBuffer.Data, listVertexBytes, vertexOffset * sizeof(ImDrawVert)) ||
                !ctx.fillBuffer(slot.indices, list->IdxBuffer.Data, listIndexBytes, static_cast<size_t>(indexOffset) * sizeof(ImDrawIdx)))
            {
                vertexOffset += static_cast<uint64_t>(list->VtxBuffer.Size);
                indexOffset += static_cast<uint32_t>(list->IdxBuffer.Size);
                continue;
            }
            for (const ImDrawCmd &command : list->CmdBuffer)
            {
                // A user callback runs here, while the frame is built: RASM records the UI later, in its pass.
                if (command.UserCallback)
                {
                    if (command.UserCallback != ImDrawCallback_ResetRenderState)
                        command.UserCallback(list, &command);
                    continue;
                }
                const float clip[4] = {command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w};
                UiDraw draw = {};
                const ImTextureID texture = command.GetTexID();
                if (command.ElemCount == 0 || texture == ImTextureID_Invalid || !getUiScissor(clip, displayPos, framebufferScale, swapchain.width, swapchain.height, draw.scissor))
                    continue;
                draw.vertices = vertexAddress + (vertexOffset + command.VtxOffset) * sizeof(ImDrawVert);
                draw.firstIndex = indexOffset + command.IdxOffset;
                draw.indexCount = command.ElemCount;
                draw.texture = static_cast<uint32_t>((texture & ~SRGB_TEXTURE_ID) - 1) | ((texture & SRGB_TEXTURE_ID) != 0 ? SRGB_TEXTURE_BIT : 0u);
                ui->draws.push_back(draw);
            }
            vertexOffset += static_cast<uint64_t>(list->VtxBuffer.Size);
            indexOffset += static_cast<uint32_t>(list->IdxBuffer.Size);
        }
    }

    // Records the frame's UI into the open render pass on the swapchain.
    void Engine::drawUi()
    {
        ui->drawn = true;
        if (ui->draws.empty())
            return;
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        ctx.bindPipeline(commandBuffer, ui->pipeline);
        ctx.bindDescriptorSet(commandBuffer, ui->pipeline, bindlessDescriptorSet, 0);
        ctx.bindIndexBuffer(commandBuffer, ui->slots[frameCount].indices, 0, Format::U16_UINT);
        for (const UiDraw &draw : ui->draws)
        {
            const gfx::UiPushConstants pushConstants{.vertices = draw.vertices, .texture = draw.texture, .displaySize = ui->displaySize};
            ctx.setScissor(commandBuffer, draw.scissor.x, draw.scissor.y, draw.scissor.width, draw.scissor.height);
            ctx.pushConstants(commandBuffer, ui->pipeline, ShaderType::VERTEX_FRAGMENT, &pushConstants, sizeof(pushConstants), 0);
            ctx.drawIndexed(commandBuffer, draw.indexCount, 1, 0, draw.firstIndex, 0);
        }
        ctx.setScissor(commandBuffer, 0, 0, swapchain.width, swapchain.height);
    }

    // The frame's last draw on the swapchain, after the tone map. It keeps the image and has no depth.
    bool Engine::addUiPass(RenderGraph &graph)
    {
        if (!ui)
            return false;
        graph.addPass(
            "UI",
            [](PassBuilder &builder)
            { builder.writeSwapchain(); },
            [this](RenderContext &)
            {
                if (ui)
                    drawUi();
            });
        graph.passes.back().encodedSwapchain = true;
        return true;
    }

    // A frame whose graph has no UI pass gets one in endFrame, so the RASM window shows in every application.
    void Engine::drawUiFallback()
    {
        if (!ui || ui->drawn || ui->draws.empty())
            return;
        if (!ui->fallbackLogged)
        {
            spdlog::debug("No UI pass drew the UI this frame, so endFrame draws it in a render pass of its own; Engine::addUiPass after the tone map saves it.");
            ui->fallbackLogged = true;
        }
        const auto renderTarget = currentRenderTarget;
        const uint32_t face = currentFace;
        resetRenderTarget();
        beginPass("UI", {.loadColor = true, .encodedColor = true});
        drawUi();
        endPass();
        setRenderTarget(renderTarget, face);
    }

    void Engine::setUiCallback(std::function<void()> callback)
    {
        if (!ui)
        {
            spdlog::warn("setUiCallback: the UI is off (EngineConfig::ui), so the callback never runs.");
            return;
        }
        ui->callback = std::move(callback);
    }

    // A loaded texture that no material samples yet gets its bindless slot here, as a material would give it.
    uint64_t Engine::getUiTexture(const TextureHandle &texture)
    {
        uint32_t index = getBindlessTextureIndex(texture);
        if (index == UINT32_MAX && texture.isValid() && isLoaded(texture))
            index = addBindlessTexture(bindlessDescriptorSet, texture);
        if (index == UINT32_MAX)
            return 0;
        return (static_cast<uint64_t>(index) + 1) | (isSrgb(ctx.getTextureDesc(texture).format) ? SRGB_TEXTURE_ID : 0);
    }
}
