#include "renderer/readback.h"
#include "rasm/engine.h"
#include "platform/paths.h"

#include "glm/gtc/packing.hpp"
#include "spdlog/spdlog.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace rasm
{
    namespace
    {
        float decodeSrgb(float encoded)
        {
            return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

        uint8_t encodeSrgb8(float linear)
        {
            const float clamped = std::clamp(linear, 0.0f, 1.0f);
            const float encoded = clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(std::lround(encoded * 255.0f));
        }

        bool isSrgbFormat(Format format)
        {
            return format == Format::R8G8B8A8_SRGB || format == Format::B8G8R8A8_SRGB;
        }

        bool isEightBitFormat(Format format)
        {
            return format == Format::R8G8B8A8_UNORM || format == Format::R8G8B8A8_SRGB || format == Format::B8G8R8A8_UNORM || format == Format::B8G8R8A8_SRGB;
        }

        // One texel as linear RGBA (8-bit UNORM numbers as they are), for the float sources and the RGBA32_FLOAT output.
        glm::vec4 loadTexel(Format format, const uint8_t *texel)
        {
            float f[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            uint16_t h[4] = {};
            switch (format)
            {
            case Format::R8G8B8A8_UNORM:
            case Format::R8G8B8A8_SRGB:
            case Format::B8G8R8A8_UNORM:
            case Format::B8G8R8A8_SRGB:
            {
                const bool bgra = format == Format::B8G8R8A8_UNORM || format == Format::B8G8R8A8_SRGB;
                for (int c = 0; c < 4; ++c)
                    f[c] = static_cast<float>(texel[bgra && c < 3 ? 2 - c : c]) / 255.0f;
                if (isSrgbFormat(format))
                    for (int c = 0; c < 3; ++c)
                        f[c] = decodeSrgb(f[c]);
                break;
            }
            case Format::R16G16B16A16_SFLOAT:
                std::memcpy(h, texel, 8);
                for (int c = 0; c < 4; ++c)
                    f[c] = glm::unpackHalf1x16(h[c]);
                break;
            case Format::R16G16_SFLOAT:
                std::memcpy(h, texel, 4);
                f[0] = glm::unpackHalf1x16(h[0]);
                f[1] = glm::unpackHalf1x16(h[1]);
                break;
            case Format::R32G32B32A32_SFLOAT:
                std::memcpy(f, texel, 16);
                break;
            case Format::R32G32_SFLOAT:
                std::memcpy(f, texel, 8);
                break;
            case Format::D32_SFLOAT:
            case Format::D32_SFLOAT_S8_UINT:
                std::memcpy(f, texel, 4);
                break;
            default:
                break;
            }
            return glm::vec4(f[0], f[1], f[2], f[3]);
        }
    }

    uint32_t getReadbackTexelSize(Format format)
    {
        switch (format)
        {
        case Format::R8G8B8A8_UNORM:
        case Format::R8G8B8A8_SRGB:
        case Format::B8G8R8A8_UNORM:
        case Format::B8G8R8A8_SRGB:
        case Format::R16G16_SFLOAT:
        case Format::D32_SFLOAT:
        case Format::D32_SFLOAT_S8_UINT: // the depth aspect alone
            return 4;
        case Format::R16G16B16A16_SFLOAT:
        case Format::R32G32_SFLOAT:
            return 8;
        case Format::R32G32B32A32_SFLOAT:
            return 16;
        default:
            return 0;
        }
    }

    bool isDepthFormat(Format format)
    {
        return format == Format::D32_SFLOAT || format == Format::D32_SFLOAT_S8_UINT || format == Format::D24_UNORM_S8_UINT;
    }

    // 8-bit sources keep their bytes for RGBA8_SRGB (only BGRA is swizzled), so a readback of an sRGB image is exact.
    bool convertReadback(Format source, const uint8_t *texels, uint32_t width, uint32_t height, ReadbackFormat format, ReadbackImage &image)
    {
        const uint32_t texelSize = getReadbackTexelSize(source);
        if (texelSize == 0 || (isDepthFormat(source) && format == ReadbackFormat::RGBA8_SRGB))
            return false;

        const size_t count = static_cast<size_t>(width) * height;
        image.width = width;
        image.height = height;
        image.format = format;
        if (format == ReadbackFormat::RGBA8_SRGB)
        {
            image.pixels.resize(count * 4);
            const bool bgra = source == Format::B8G8R8A8_UNORM || source == Format::B8G8R8A8_SRGB;
            for (size_t i = 0; i < count; ++i)
            {
                const uint8_t *texel = texels + i * texelSize;
                uint8_t *out = image.pixels.data() + i * 4;
                if (isEightBitFormat(source))
                {
                    for (int c = 0; c < 4; ++c)
                        out[c] = texel[bgra && c < 3 ? 2 - c : c];
                    continue;
                }
                const glm::vec4 value = loadTexel(source, texel);
                for (int c = 0; c < 3; ++c)
                    out[c] = encodeSrgb8(value[c]);
                out[3] = static_cast<uint8_t>(std::lround(std::clamp(value.a, 0.0f, 1.0f) * 255.0f));
            }
            return true;
        }

        image.pixels.resize(count * 16);
        for (size_t i = 0; i < count; ++i)
        {
            const glm::vec4 value = loadTexel(source, texels + i * texelSize);
            std::memcpy(image.pixels.data() + i * 16, &value, 16);
        }
        return true;
    }

    bool writePng(const std::string &path, const ReadbackImage &image)
    {
        if (image.format != ReadbackFormat::RGBA8_SRGB || image.width == 0 || image.height == 0 || image.pixels.size() != static_cast<size_t>(image.width) * image.height * 4)
        {
            spdlog::error("writePng: '{}' needs an RGBA8_SRGB image with width x height pixels.", path);
            return false;
        }
        std::ofstream file(pathFromUtf8(path), std::ios::binary);
        if (!file)
        {
            spdlog::error("writePng: can't open '{}' for writing.", path);
            return false;
        }
        auto write = [](void *context, void *data, int size)
        { static_cast<std::ofstream *>(context)->write(static_cast<const char *>(data), size); };
        const int width = static_cast<int>(image.width);
        const int height = static_cast<int>(image.height);
        if (stbi_write_png_to_func(write, &file, width, height, 4, image.pixels.data(), width * 4) == 0 || !file)
        {
            spdlog::error("writePng: failed to write '{}'.", path);
            return false;
        }
        return true;
    }

    ReadbackHandle Engine::readFrame(ReadbackFormat format)
    {
        // Every image of a swapchain is made alike, so the first one tells.
        const TextureDesc desc = swapchain.imageHandles.empty() ? TextureDesc{} : ctx.getTextureDesc(swapchain.imageHandles[0]);
        if (!desc.readable)
        {
            spdlog::error("readFrame: the window's surface does not allow copies from its images (no TRANSFER_SRC in its supported usage). A headless engine (EngineConfig::platform) can always be read.");
            return {};
        }
        const ReadbackHandle handle = handleManager.getNextReadbackHandle();
        readbacks->pending.push_back({.handle = handle, .format = format});
        return handle;
    }

    ReadbackHandle Engine::readTexture(const TextureHandle &texture, ReadbackFormat format, uint32_t layer, uint32_t mip)
    {
        const TextureDesc desc = ctx.getTextureDesc(texture);
        if (desc.format == Format::UNKNOWN)
            return {};
        if (!desc.readable)
        {
            spdlog::error("readTexture: texture '{}' is not readable. Create its render target with RenderTargetDesc::readable (or the texture with TextureDesc::readable).", desc.name);
            return {};
        }
        if (desc.samples > 1)
        {
            spdlog::error("readTexture: texture '{}' is multisampled; read its resolve (addMsaaResolvePass) instead.", desc.name);
            return {};
        }
        if (getReadbackTexelSize(desc.format) == 0 || (isDepthFormat(desc.format) && format != ReadbackFormat::RGBA32_FLOAT))
        {
            spdlog::error("readTexture: texture '{}' has a format a readback can't convert{}.", desc.name, isDepthFormat(desc.format) ? " to RGBA8_SRGB (read a depth as RGBA32_FLOAT)" : "");
            return {};
        }
        if (layer >= (desc.cube ? CUBE_FACES : 1) || mip >= std::max(desc.mipLevels, 1u))
        {
            spdlog::error("readTexture: texture '{}' has no layer {} mip {}.", desc.name, layer, mip);
            return {};
        }
        const ReadbackHandle handle = handleManager.getNextReadbackHandle();
        readbacks->pending.push_back({.handle = handle, .texture = texture, .layer = layer, .mip = mip, .format = format});
        return handle;
    }

    bool Engine::isReadbackReady(ReadbackHandle readback)
    {
        for (const auto &pending : readbacks->pending)
        {
            if (pending.handle == readback)
                return pending.failed || (pending.submitted && ctx.getFenceValue(frameFence) >= pending.fenceValue);
        }
        return false;
    }

    bool Engine::takeReadback(ReadbackHandle readback, ReadbackImage &image)
    {
        auto it = std::find_if(readbacks->pending.begin(), readbacks->pending.end(), [readback](const PendingReadback &pending)
                               { return pending.handle == readback; });
        if (it == readbacks->pending.end())
        {
            spdlog::error("takeReadback: unknown readback, or one already taken.");
            return false;
        }
        if (!it->failed && !it->submitted)
        {
            // Blocking here would never end: no frame has copied it yet.
            spdlog::error("takeReadback: no frame has copied this readback yet; it is copied at the end of the next frame (endFrame).");
            return false;
        }

        bool taken = false;
        if (!it->failed && ctx.waitForFence(frameFence, it->fenceValue))
        {
            std::vector<uint8_t> texels(static_cast<size_t>(it->width) * it->height * getReadbackTexelSize(it->sourceFormat));
            taken = ctx.readBuffer(it->buffer, texels.data(), texels.size()) &&
                    convertReadback(it->sourceFormat, texels.data(), it->width, it->height, it->format, image);
        }
        if (it->buffer.isValid())
            ctx.destroyBuffer(it->buffer);
        readbacks->pending.erase(it);
        return taken;
    }

    // In the frame's last graphics batch, after every pass and the UI, before the swapchain image goes to PRESENT_SRC.
    void Engine::recordReadbacks()
    {
        const bool any = std::any_of(readbacks->pending.begin(), readbacks->pending.end(), [](const PendingReadback &pending)
                                     { return !pending.recorded && !pending.failed; });
        if (!any)
            return;
        // A compute-queue pass may have written the texture last, so the copy waits for all of the frame's compute work.
        if (asyncCompute)
            waitForQueueWork(QueueType::GRAPHICS, QueueType::COMPUTE);
        if (!useQueue(QueueType::GRAPHICS))
            return;

        const CommandBufferHandle commandBuffer = frameResources[frameCount].commandBuffer;
        for (auto &pending : readbacks->pending)
        {
            if (pending.recorded || pending.failed)
                continue;
            const TextureHandle texture = pending.texture.isValid() ? pending.texture : swapchain.imageHandles[imageIdx];
            const TextureDesc desc = ctx.getTextureDesc(texture);
            pending.sourceFormat = desc.format;
            pending.width = std::max(1u, desc.width >> pending.mip);
            pending.height = std::max(1u, desc.height >> pending.mip);
            const uint64_t size = static_cast<uint64_t>(pending.width) * pending.height * getReadbackTexelSize(desc.format);
            if (!pending.buffer.isValid() && size > 0)
                pending.buffer = ctx.createBuffer({.name = "Readback " + std::to_string(pending.handle.index), .usage = BufferUsage::STORAGE, .size = size, .hostReadable = true});
            pending.failed = size == 0 || !pending.buffer.isValid() || !ctx.copyTextureToBuffer(commandBuffer, texture, pending.layer, pending.mip, pending.buffer);
            pending.recorded = !pending.failed;
            if (pending.failed)
                spdlog::error("A readback of '{}' failed; takeReadback returns false for it.", desc.name);
        }
    }

    // After submitBatches: the copies recorded this frame are done at the frame's fence value. A frame that did not go
    // out took its copies with it, so they are recorded again in the next one.
    void Engine::finishReadbackSubmit(bool submitted)
    {
        for (auto &pending : readbacks->pending)
        {
            if (!pending.recorded || pending.submitted)
                continue;
            pending.submitted = submitted;
            pending.recorded = submitted;
            pending.fenceValue = frameFenceValue;
        }
    }
}
