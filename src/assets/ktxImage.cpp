#include "assets/ktxImage.h"

#include "platform/paths.h"
#include "rhi/device.h"
#include "rhi/mipChain.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "KHR/khr_df.h"
#include "ktx.h"
// KTX2 names a file's format by its VkFormat value, so the container's own constants come from here.
#include "vulkan/vulkan_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace rasm
{
    namespace
    {
        struct KtxTexture
        {
            ktxTexture2 *texture = nullptr;

            ~KtxTexture()
            {
                if (texture)
                    ktxTexture_Destroy(ktxTexture(texture));
            }
        };

        constexpr unsigned char KTX2_IDENTIFIER[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

        // Best first. Color slots read every channel as color, so a one- or two-channel file is gray or gray and
        // alpha there and takes an RGBA format. A one- or two-channel format holds R, or a normal map's x and y, so
        // only a slot that reads just those gets one: an RGBA format keeps what the file decodes to for any other.
        constexpr Format COLOR_FORMATS[] = {Format::BC7_SRGB, Format::ASTC_4x4_SRGB, Format::ETC2_R8G8B8A8_SRGB, Format::R8G8B8A8_SRGB};
        constexpr Format DATA_FORMATS[] = {Format::BC7_UNORM, Format::ASTC_4x4_UNORM, Format::ETC2_R8G8B8A8_UNORM, Format::R8G8B8A8_UNORM};
        constexpr Format RG_FORMATS[] = {Format::BC5_UNORM, Format::EAC_R11G11_UNORM, Format::R8G8B8A8_UNORM};
        constexpr Format R_FORMATS[] = {Format::BC4_UNORM, Format::EAC_R11_UNORM, Format::R8G8B8A8_UNORM};
    }

    namespace
    {
        Format fromVkFormat(uint32_t vkFormat)
        {
            switch (vkFormat)
            {
            case VK_FORMAT_R8G8B8A8_UNORM:
                return Format::R8G8B8A8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB:
                return Format::R8G8B8A8_SRGB;
            case VK_FORMAT_BC4_UNORM_BLOCK:
                return Format::BC4_UNORM;
            case VK_FORMAT_BC5_UNORM_BLOCK:
                return Format::BC5_UNORM;
            case VK_FORMAT_BC7_UNORM_BLOCK:
                return Format::BC7_UNORM;
            case VK_FORMAT_BC7_SRGB_BLOCK:
                return Format::BC7_SRGB;
            case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
                return Format::ASTC_4x4_UNORM;
            case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
                return Format::ASTC_4x4_SRGB;
            case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
                return Format::ETC2_R8G8B8A8_UNORM;
            case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
                return Format::ETC2_R8G8B8A8_SRGB;
            case VK_FORMAT_EAC_R11_UNORM_BLOCK:
                return Format::EAC_R11_UNORM;
            case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
                return Format::EAC_R11G11_UNORM;
            default:
                return Format::UNKNOWN;
            }
        }

        // The same bits read as sRGB or linear. The one- and two-channel formats have no sRGB form.
        Format withColorSpace(Format format, bool srgb)
        {
            switch (format)
            {
            case Format::R8G8B8A8_UNORM:
            case Format::R8G8B8A8_SRGB:
                return srgb ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM;
            case Format::BC7_UNORM:
            case Format::BC7_SRGB:
                return srgb ? Format::BC7_SRGB : Format::BC7_UNORM;
            case Format::ASTC_4x4_UNORM:
            case Format::ASTC_4x4_SRGB:
                return srgb ? Format::ASTC_4x4_SRGB : Format::ASTC_4x4_UNORM;
            case Format::ETC2_R8G8B8A8_UNORM:
            case Format::ETC2_R8G8B8A8_SRGB:
                return srgb ? Format::ETC2_R8G8B8A8_SRGB : Format::ETC2_R8G8B8A8_UNORM;
            default:
                return format;
            }
        }

        ktx_transcode_fmt_e toTranscodeFormat(Format format)
        {
            switch (format)
            {
            case Format::BC4_UNORM:
                return KTX_TTF_BC4_R;
            case Format::BC5_UNORM:
                return KTX_TTF_BC5_RG;
            case Format::BC7_UNORM:
            case Format::BC7_SRGB:
                return KTX_TTF_BC7_RGBA;
            case Format::ASTC_4x4_UNORM:
            case Format::ASTC_4x4_SRGB:
                return KTX_TTF_ASTC_4x4_RGBA;
            case Format::ETC2_R8G8B8A8_UNORM:
            case Format::ETC2_R8G8B8A8_SRGB:
                return KTX_TTF_ETC2_RGBA;
            case Format::EAC_R11_UNORM:
                return KTX_TTF_ETC2_EAC_R11;
            case Format::EAC_R11G11_UNORM:
                return KTX_TTF_ETC2_EAC_RG11;
            case Format::R8G8B8A8_UNORM:
            case Format::R8G8B8A8_SRGB:
                return KTX_TTF_RGBA32;
            default:
                return KTX_TTF_NOSELECTION;
            }
        }

        bool isRgba8(Format format)
        {
            return format == Format::R8G8B8A8_UNORM || format == Format::R8G8B8A8_SRGB;
        }

        Format firstSupported(const Format *formats, size_t count, uint64_t supportedFormats)
        {
            for (size_t i = 0; i < count; ++i)
            {
                if ((supportedFormats & formatBit(formats[i])) != 0)
                    return formats[i];
            }
            return Format::UNKNOWN;
        }

        bool readInfo(ktxTexture2 *texture, KtxInfo &info, std::string &error)
        {
            if (texture->numDimensions != 2 || texture->numLayers != 1 || texture->numFaces != 1 || texture->baseWidth == 0 || texture->baseHeight == 0)
            {
                error = "only 2D textures with one layer and one face are supported";
                return false;
            }
            // libktx counts a UASTC file whose two channels are R and G (KHR_DF_CHANNEL_UASTC_RG) as one channel.
            const uint32_t *descriptor = texture->pDfd + 1;
            const bool uastcRg = KHR_DFDVAL(descriptor, MODEL) == KHR_DF_MODEL_UASTC && KHR_DFDSVAL(descriptor, 0, CHANNELID) == KHR_DF_CHANNEL_UASTC_RG;
            info = {.width = texture->baseWidth,
                    .height = texture->baseHeight,
                    .levelCount = texture->numLevels,
                    .components = uastcRg ? 2 : ktxTexture2_GetNumComponents(texture),
                    .basis = ktxTexture2_NeedsTranscoding(texture),
                    .yInGreen = uastcRg,
                    .format = fromVkFormat(texture->vkFormat)};
            if (!info.basis && info.format == Format::UNKNOWN)
            {
                error = "its format (VkFormat " + std::to_string(texture->vkFormat) + ") is not one RASM samples";
                return false;
            }
            return true;
        }

        // A two-channel file transcoded to RGBA8 gives (x, x, x, y), or (x, y, 0, 1) when its channels are R and G. A
        // normal map wants (x, y, z), with z rebuilt from x and y, so it reads like any RGBA8 normal map.
        void rebuildNormalZ(unsigned char *texels, size_t texelCount, bool yInGreen)
        {
            for (size_t i = 0; i < texelCount; ++i)
            {
                unsigned char *texel = texels + i * 4;
                const unsigned char yByte = yInGreen ? texel[1] : texel[3];
                const float x = texel[0] / 127.5f - 1.0f;
                const float y = yByte / 127.5f - 1.0f;
                const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                texel[1] = yByte;
                texel[2] = static_cast<unsigned char>(std::lround((z + 1.0f) * 127.5f));
                texel[3] = 255;
            }
        }
    }

    uint64_t getSupportedTextureFormats(const RenderContext &ctx)
    {
        uint64_t formats = 0;
        for (const Format format : {Format::R8G8B8A8_UNORM, Format::R8G8B8A8_SRGB, Format::BC4_UNORM, Format::BC5_UNORM, Format::BC7_UNORM, Format::BC7_SRGB,
                                    Format::ASTC_4x4_UNORM, Format::ASTC_4x4_SRGB, Format::ETC2_R8G8B8A8_UNORM, Format::ETC2_R8G8B8A8_SRGB,
                                    Format::EAC_R11_UNORM, Format::EAC_R11G11_UNORM})
        {
            if (ctx.supportsTextureFormat(format))
                formats |= formatBit(format);
        }
        return formats;
    }

    bool isKtx2(const unsigned char *bytes, size_t size)
    {
        return size >= sizeof(KTX2_IDENTIFIER) && std::memcmp(bytes, KTX2_IDENTIFIER, sizeof(KTX2_IDENTIFIER)) == 0;
    }

    // Reads the header, the level index and the data format descriptor, not the levels.
    bool readKtxInfo(const std::string &path, KtxInfo &info, std::string &error)
    {
        std::FILE *file = openFileForReading(path);
        if (!file)
        {
            error = "the file can't be opened";
            return false;
        }
        KtxTexture texture;
        const KTX_error_code result = ktxTexture2_CreateFromStdioStream(file, KTX_TEXTURE_CREATE_SKIP_KVDATA_BIT, &texture.texture);
        std::fclose(file);
        if (result != KTX_SUCCESS)
        {
            error = ktxErrorString(result);
            return false;
        }
        return readInfo(texture.texture, info, error);
    }

    bool readKtxInfo(const unsigned char *bytes, size_t size, KtxInfo &info, std::string &error)
    {
        KtxTexture texture;
        const KTX_error_code result = ktxTexture2_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_SKIP_KVDATA_BIT, &texture.texture);
        if (result != KTX_SUCCESS)
        {
            error = ktxErrorString(result);
            return false;
        }
        return readInfo(texture.texture, info, error);
    }

    // A Basis file without mips is transcoded to RGBA8, whose mips are made on the CPU: a block format's mips
    // can't be made without an encoder, and a texture without them shimmers when it is minified. Returns UNKNOWN
    // when the device samples none of the candidates, or a file that is not Basis is in a format it can't sample.
    Format chooseKtxFormat(const KtxInfo &info, KtxSlot slot, uint64_t supportedFormats)
    {
        const bool srgb = slot == KtxSlot::COLOR;
        if (!info.basis)
        {
            const Format format = withColorSpace(info.format, srgb);
            return (supportedFormats & formatBit(format)) != 0 ? format : Format::UNKNOWN;
        }

        if (info.levelCount == 1 && (info.width > 1 || info.height > 1))
            return srgb ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM;
        if (srgb)
            return firstSupported(COLOR_FORMATS, std::size(COLOR_FORMATS), supportedFormats);
        const bool red = info.components == 1 && (slot == KtxSlot::OCCLUSION || slot == KtxSlot::CHANNELS);
        const bool xy = info.components == 2 && (slot == KtxSlot::NORMAL || slot == KtxSlot::CHANNELS);
        if (red)
            return firstSupported(R_FORMATS, std::size(R_FORMATS), supportedFormats);
        // The transcoder reads a two-channel block format's y from alpha, so a file with y in green goes to RGBA8.
        if (xy)
            return info.yInGreen ? Format::R8G8B8A8_UNORM : firstSupported(RG_FORMATS, std::size(RG_FORMATS), supportedFormats);
        return firstSupported(DATA_FORMATS, std::size(DATA_FORMATS), supportedFormats);
    }

    // What a texture whose file failed to decode shows: white, or a flat normal for a two-channel format, so a
    // material that samples it looks plain rather than black or, for ASTC's reserved all-zero block, magenta. One
    // constant block per format, from each format's spec.
    void makeBlankKtxLevels(Format format, uint32_t width, uint32_t height, uint32_t mipCount, TextureRaw &raw)
    {
        // BC7 mode 6 with every endpoint 255 and every index 0.
        const unsigned char bc7White[16] = {0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
        // Both endpoints 255, every index 0 (endpoint 0).
        const unsigned char bc4White[8] = {0xFF, 0xFF};
        // Two BC4 halves of 128: x = y = 0.5, a normal along z.
        const unsigned char bc5Flat[16] = {0x80, 0x80, 0, 0, 0, 0, 0, 0, 0x80, 0x80};
        // A void-extent block: one RGBA16 color, 0xFFFF each, for the whole block.
        const unsigned char astcWhite[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        // EAC: base 255, multiplier 1, table 0, every index 4 (+2), clamped to 1. ETC2 color: individual mode, both
        // colors 15 (255), table 0, every index 0 (+2), clamped to 255.
        const unsigned char eacWhite[8] = {0xFF, 0x10, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24};
        const unsigned char etc2White[16] = {0xFF, 0x10, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24, 0xFF, 0xFF, 0xFF};
        // EAC with base 128: about 0.5 for x and y.
        const unsigned char eacRgFlat[16] = {0x80, 0x10, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24, 0x80, 0x10, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24};
        const unsigned char rgbaWhite[4] = {0xFF, 0xFF, 0xFF, 0xFF};

        const unsigned char *block = rgbaWhite;
        switch (format)
        {
        case Format::BC7_UNORM:
        case Format::BC7_SRGB:
            block = bc7White;
            break;
        case Format::BC4_UNORM:
            block = bc4White;
            break;
        case Format::BC5_UNORM:
            block = bc5Flat;
            break;
        case Format::ASTC_4x4_UNORM:
        case Format::ASTC_4x4_SRGB:
            block = astcWhite;
            break;
        case Format::ETC2_R8G8B8A8_UNORM:
        case Format::ETC2_R8G8B8A8_SRGB:
            block = etc2White;
            break;
        case Format::EAC_R11_UNORM:
            block = eacWhite;
            break;
        case Format::EAC_R11G11_UNORM:
            block = eacRgFlat;
            break;
        default:
            break;
        }

        const uint32_t blockBytes = rhi::getFormatBlock(format).bytes;
        raw = {.width = width, .height = height, .format = format};
        for (uint32_t level = 0; level < mipCount; ++level)
        {
            const uint64_t size = rhi::getTextureLevelSize(format, std::max(1u, width >> level), std::max(1u, height >> level));
            raw.levelSizes.push_back(size);
            for (uint64_t offset = 0; offset < size; offset += blockBytes)
                raw.levels.insert(raw.levels.end(), block, block + blockBytes);
        }
    }

    // The file's levels, or the full chain for an RGBA8 texture whose file has a single level.
    uint32_t getKtxMipCount(const KtxInfo &info, Format format)
    {
        if (isRgba8(format) && info.levelCount == 1)
            return rhi::getFullMipCount(info.width, info.height);
        return info.levelCount;
    }

    // Each case loses memory or quality, and the fix is in the file.
    void warnAboutKtxFormat(const std::string &name, const KtxInfo &info, Format format)
    {
        if (info.basis && info.yInGreen && format == Format::R8G8B8A8_UNORM && info.levelCount > 1)
            spdlog::warn("KTX2 texture {} is a two-channel UASTC file with y in green, so it is transcoded to RGBA8 and loses its compression. "
                         "Encode normal maps with x in RGB and y in alpha to get BC5: ktx create --normal-mode.",
                         name);
        if (info.levelCount > 1 || (info.width == 1 && info.height == 1))
            return;
        if (info.basis)
            spdlog::warn("KTX2 texture {} has no mips, so it is transcoded to RGBA8 and its mips are made on the CPU: it loses its compression "
                         "(4x the memory or more) and loads slower. Make the file with mips: ktx create --generate-mipmap, or gltf-transform.",
                         name);
        else if (!isRgba8(format))
            spdlog::warn("KTX2 texture {} has no mips and a block format, so it has one level and shimmers when it is minified. "
                         "Make the file with mips: ktx create --generate-mipmap.",
                         name);
    }

    // Runs on a worker thread. Transcodes a Basis file to `format`, or takes the levels of any other file as they are
    // (inflated when the file is zstd-compressed), then makes the mips of an RGBA8 texture whose file had one level.
    bool decodeKtx(const unsigned char *bytes, size_t size, Format format, TextureRaw &raw, std::string &error)
    {
        ZoneScoped;

        KtxTexture texture;
        KTX_error_code result = ktxTexture2_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT | KTX_TEXTURE_CREATE_SKIP_KVDATA_BIT, &texture.texture);
        if (result != KTX_SUCCESS)
        {
            error = ktxErrorString(result);
            return false;
        }
        KtxInfo info;
        if (!readInfo(texture.texture, info, error))
            return false;

        if (info.basis)
        {
            ZoneScopedN("ktxTexture2_TranscodeBasis");
            // libktx sets up the Basis transcoder's tables on its first transcode, behind a flag with no lock, and
            // several workers transcode at once: the first transcode runs alone.
            static std::once_flag firstTranscode;
            bool transcoded = false;
            std::call_once(firstTranscode, [&]()
                           {
                               result = ktxTexture2_TranscodeBasis(texture.texture, toTranscodeFormat(format), 0);
                               transcoded = true; });
            if (!transcoded)
                result = ktxTexture2_TranscodeBasis(texture.texture, toTranscodeFormat(format), 0);
            if (result != KTX_SUCCESS)
            {
                error = std::string("transcoding failed: ") + ktxErrorString(result);
                return false;
            }
        }
        else if (withColorSpace(info.format, true) != withColorSpace(format, true))
        {
            error = "the file's format does not match the texture's";
            return false;
        }

        raw = {.width = info.width, .height = info.height, .format = format};
        ktx_uint8_t *texels = ktxTexture_GetData(ktxTexture(texture.texture));
        uint64_t total = 0;
        for (uint32_t level = 0; level < info.levelCount; ++level)
        {
            const uint64_t levelSize = rhi::getTextureLevelSize(format, std::max(1u, info.width >> level), std::max(1u, info.height >> level));
            if (ktxTexture_GetImageSize(ktxTexture(texture.texture), level) != levelSize)
            {
                error = "level " + std::to_string(level) + " has the wrong size";
                return false;
            }
            total += levelSize;
        }

        raw.levels.resize(total);
        uint64_t offset = 0;
        for (uint32_t level = 0; level < info.levelCount; ++level)
        {
            ktx_size_t source = 0;
            ktxTexture_GetImageOffset(ktxTexture(texture.texture), level, 0, 0, &source);
            const uint64_t levelSize = ktxTexture_GetImageSize(ktxTexture(texture.texture), level);
            std::memcpy(raw.levels.data() + offset, texels + source, levelSize);
            raw.levelSizes.push_back(levelSize);
            offset += levelSize;
        }

        if (info.basis && info.components == 2 && format == Format::R8G8B8A8_UNORM)
            rebuildNormalZ(raw.levels.data(), raw.levels.size() / 4, info.yInGreen);

        const uint32_t mipCount = getKtxMipCount(info, format);
        if (mipCount > info.levelCount)
        {
            ZoneScopedN("buildMipChain");
            std::vector<unsigned char> mips;
            std::vector<TextureLevel> levels(mipCount);
            if (!rhi::buildMipChain(raw.levels.data(), info.width, info.height, format == Format::R8G8B8A8_SRGB, mipCount, mips, levels.data()))
            {
                error = "making the mips failed";
                return false;
            }
            raw.levels.insert(raw.levels.end(), mips.begin(), mips.end());
            for (uint32_t level = 1; level < mipCount; ++level)
                raw.levelSizes.push_back(levels[level].size);
        }
        return true;
    }
}
