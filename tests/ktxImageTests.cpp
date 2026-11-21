#include "assets/ktxImage.h"
#include "importMeshText.h"
#include "platform/paths.h"
#include "rhi/device.h"

#include "doctest/doctest.h"
#include "ktx.h"
#include "tiny_gltf.h"
#include "vulkan/vulkan_core.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace rasm;

namespace
{
    enum class Codec
    {
        NONE,
        ETC1S,
        UASTC,
    };

    constexpr uint64_t BC_DEVICE = formatBit(Format::R8G8B8A8_UNORM) | formatBit(Format::R8G8B8A8_SRGB) | formatBit(Format::BC4_UNORM) |
                                   formatBit(Format::BC5_UNORM) | formatBit(Format::BC7_UNORM) | formatBit(Format::BC7_SRGB);
    constexpr uint64_t MOBILE_DEVICE = formatBit(Format::R8G8B8A8_UNORM) | formatBit(Format::R8G8B8A8_SRGB) | formatBit(Format::ASTC_4x4_UNORM) |
                                       formatBit(Format::ASTC_4x4_SRGB) | formatBit(Format::ETC2_R8G8B8A8_UNORM) | formatBit(Format::ETC2_R8G8B8A8_SRGB) |
                                       formatBit(Format::EAC_R11_UNORM) | formatBit(Format::EAC_R11G11_UNORM);
    constexpr uint64_t ETC2_DEVICE = formatBit(Format::R8G8B8A8_UNORM) | formatBit(Format::R8G8B8A8_SRGB) | formatBit(Format::ETC2_R8G8B8A8_UNORM) |
                                     formatBit(Format::ETC2_R8G8B8A8_SRGB) | formatBit(Format::EAC_R11_UNORM) | formatBit(Format::EAC_R11G11_UNORM);
    constexpr uint64_t RGBA8_DEVICE = formatBit(Format::R8G8B8A8_UNORM) | formatBit(Format::R8G8B8A8_SRGB);

    // The texel of level `level` at (x, y): smooth, so the codecs keep it close.
    unsigned char texelValue(uint32_t x, uint32_t y, uint32_t channel, uint32_t level)
    {
        return static_cast<unsigned char>(40 + 8 * level + 3 * x + 2 * y + 30 * channel);
    }

    // A KTX2 file made in memory by libktx: `components` channels of 8 bits, `levels` mips, encoded with `codec`.
    std::vector<unsigned char> makeKtx2(uint32_t width, uint32_t height, uint32_t components, uint32_t levels, Codec codec, bool srgb)
    {
        const uint32_t formats[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        const uint32_t srgbFormats[] = {VK_FORMAT_R8_SRGB, VK_FORMAT_R8G8_SRGB, VK_FORMAT_R8G8B8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
        ktxTextureCreateInfo createInfo = {};
        createInfo.vkFormat = srgb ? srgbFormats[components - 1] : formats[components - 1];
        createInfo.baseWidth = width;
        createInfo.baseHeight = height;
        createInfo.baseDepth = 1;
        createInfo.numDimensions = 2;
        createInfo.numLevels = levels;
        createInfo.numLayers = 1;
        createInfo.numFaces = 1;
        ktxTexture2 *texture = nullptr;
        REQUIRE(ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) == KTX_SUCCESS);

        for (uint32_t level = 0; level < levels; ++level)
        {
            const uint32_t w = std::max(1u, width >> level);
            const uint32_t h = std::max(1u, height >> level);
            std::vector<unsigned char> texels(static_cast<size_t>(w) * h * components);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    for (uint32_t c = 0; c < components; ++c)
                        texels[(static_cast<size_t>(y) * w + x) * components + c] = texelValue(x, y, c, level);
            REQUIRE(ktxTexture_SetImageFromMemory(ktxTexture(texture), level, 0, 0, texels.data(), texels.size()) == KTX_SUCCESS);
        }

        if (codec != Codec::NONE)
        {
            ktxBasisParams params = {};
            params.structSize = sizeof(params);
            params.uastc = codec == Codec::UASTC ? KTX_TRUE : KTX_FALSE;
            params.threadCount = 1;
            params.compressionLevel = 1;
            params.qualityLevel = 128;
            // Like ktx create --normal-mode: two channels as x in RGB and y in alpha.
            params.normalMap = components == 2 ? KTX_TRUE : KTX_FALSE;
            REQUIRE(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS);
            if (codec == Codec::UASTC)
                REQUIRE(ktxTexture2_DeflateZstd(texture, 3) == KTX_SUCCESS);
        }

        ktx_uint8_t *bytes = nullptr;
        ktx_size_t size = 0;
        REQUIRE(ktxTexture_WriteToMemory(ktxTexture(texture), &bytes, &size) == KTX_SUCCESS);
        std::vector<unsigned char> file(bytes, bytes + size);
        std::free(bytes);
        ktxTexture_Destroy(ktxTexture(texture));
        return file;
    }

    KtxInfo readInfo(const std::vector<unsigned char> &file)
    {
        KtxInfo info;
        std::string error;
        REQUIRE_MESSAGE(readKtxInfo(file.data(), file.size(), info, error), error);
        return info;
    }

    TextureRaw decode(const std::vector<unsigned char> &file, Format format)
    {
        TextureRaw raw;
        std::string error;
        REQUIRE_MESSAGE(decodeKtx(file.data(), file.size(), format, raw, error), error);
        return raw;
    }

    // Every level is there, tightly packed, at the size RenderContext::fillTexture checks.
    void checkLevels(const TextureRaw &raw, uint32_t width, uint32_t height, uint32_t levelCount)
    {
        REQUIRE(raw.levelSizes.size() == levelCount);
        uint64_t total = 0;
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            CHECK(raw.levelSizes[level] == rhi::getTextureLevelSize(raw.format, std::max(1u, width >> level), std::max(1u, height >> level)));
            total += raw.levelSizes[level];
        }
        CHECK(raw.levels.size() == total);
    }
}

TEST_CASE("isKtx2 knows a KTX2 file by its identifier")
{
    const auto file = makeKtx2(4, 4, 4, 1, Codec::NONE, false);
    CHECK(isKtx2(file.data(), file.size()));
    const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13};
    CHECK_FALSE(isKtx2(png, sizeof(png)));
    CHECK_FALSE(isKtx2(file.data(), 8));
}

TEST_CASE("readKtxInfo reads the size, the levels, the channels and whether the file is Basis")
{
    const KtxInfo uastc = readInfo(makeKtx2(32, 16, 4, 6, Codec::UASTC, false));
    CHECK(uastc.width == 32);
    CHECK(uastc.height == 16);
    CHECK(uastc.levelCount == 6);
    CHECK(uastc.components == 4);
    CHECK(uastc.basis);

    // A normal map's y is in alpha: ETC1S's second slice, UASTC's RRRG.
    for (const Codec codec : {Codec::ETC1S, Codec::UASTC})
    {
        const KtxInfo normals = readInfo(makeKtx2(16, 16, 2, 1, codec, false));
        CHECK(normals.components == 2);
        CHECK(normals.basis);
        CHECK_FALSE(normals.yInGreen);
    }

    const KtxInfo raw = readInfo(makeKtx2(8, 8, 4, 4, Codec::NONE, true));
    CHECK_FALSE(raw.basis);
    CHECK(raw.format == Format::R8G8B8A8_SRGB);

    KtxInfo info;
    std::string error;
    const unsigned char garbage[64] = {};
    CHECK_FALSE(readKtxInfo(garbage, sizeof(garbage), info, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("a Basis file goes to the best format the device samples, by slot and channels")
{
    const KtxInfo rgba = {.width = 256, .height = 256, .levelCount = 9, .components = 4, .basis = true};
    const KtxInfo rg = {.width = 256, .height = 256, .levelCount = 9, .components = 2, .basis = true};
    const KtxInfo r = {.width = 256, .height = 256, .levelCount = 9, .components = 1, .basis = true};

    CHECK(chooseKtxFormat(rgba, true, BC_DEVICE) == Format::BC7_SRGB);
    CHECK(chooseKtxFormat(rgba, false, BC_DEVICE) == Format::BC7_UNORM);
    CHECK(chooseKtxFormat(rg, false, BC_DEVICE) == Format::BC5_UNORM);
    CHECK(chooseKtxFormat(r, false, BC_DEVICE) == Format::BC4_UNORM);
    // A color slot reads one or two channels as gray, or gray and alpha.
    CHECK(chooseKtxFormat(rg, true, BC_DEVICE) == Format::BC7_SRGB);
    CHECK(chooseKtxFormat(r, true, BC_DEVICE) == Format::BC7_SRGB);

    CHECK(chooseKtxFormat(rgba, true, MOBILE_DEVICE) == Format::ASTC_4x4_SRGB);
    CHECK(chooseKtxFormat(rgba, false, MOBILE_DEVICE) == Format::ASTC_4x4_UNORM);
    CHECK(chooseKtxFormat(rg, false, MOBILE_DEVICE) == Format::EAC_R11G11_UNORM);
    CHECK(chooseKtxFormat(r, false, MOBILE_DEVICE) == Format::EAC_R11_UNORM);

    CHECK(chooseKtxFormat(rgba, true, ETC2_DEVICE) == Format::ETC2_R8G8B8A8_SRGB);
    CHECK(chooseKtxFormat(rgba, false, ETC2_DEVICE) == Format::ETC2_R8G8B8A8_UNORM);

    CHECK(chooseKtxFormat(rgba, true, RGBA8_DEVICE) == Format::R8G8B8A8_SRGB);
    CHECK(chooseKtxFormat(rg, false, RGBA8_DEVICE) == Format::R8G8B8A8_UNORM);
    CHECK(chooseKtxFormat(rgba, false, 0) == Format::UNKNOWN);

    CHECK(getKtxMipCount(rgba, Format::BC7_SRGB) == 9);

    // The transcoder reads a two-channel block format's y from alpha.
    const KtxInfo rgInGreen = {.width = 256, .height = 256, .levelCount = 9, .components = 2, .basis = true, .yInGreen = true};
    CHECK(chooseKtxFormat(rgInGreen, false, BC_DEVICE) == Format::R8G8B8A8_UNORM);
    CHECK(chooseKtxFormat(rgInGreen, true, BC_DEVICE) == Format::BC7_SRGB);
}

TEST_CASE("a Basis file without mips becomes RGBA8 with every mip, on any device")
{
    const KtxInfo single = {.width = 256, .height = 64, .levelCount = 1, .components = 4, .basis = true};
    CHECK(chooseKtxFormat(single, true, BC_DEVICE) == Format::R8G8B8A8_SRGB);
    CHECK(chooseKtxFormat(single, false, MOBILE_DEVICE) == Format::R8G8B8A8_UNORM);
    CHECK(getKtxMipCount(single, Format::R8G8B8A8_SRGB) == 9);

    // A single texel needs no mips.
    const KtxInfo texel = {.width = 1, .height = 1, .levelCount = 1, .components = 4, .basis = true};
    CHECK(chooseKtxFormat(texel, true, BC_DEVICE) == Format::BC7_SRGB);
}

TEST_CASE("a file that is not Basis keeps its format, read in the slot's color space")
{
    const KtxInfo bc7 = {.width = 64, .height = 64, .levelCount = 7, .components = 4, .format = Format::BC7_UNORM};
    CHECK(chooseKtxFormat(bc7, true, BC_DEVICE) == Format::BC7_SRGB);
    CHECK(chooseKtxFormat(bc7, false, BC_DEVICE) == Format::BC7_UNORM);
    CHECK(chooseKtxFormat(bc7, true, MOBILE_DEVICE) == Format::UNKNOWN);

    const KtxInfo rgba8 = {.width = 64, .height = 64, .levelCount = 1, .components = 4, .format = Format::R8G8B8A8_SRGB};
    CHECK(chooseKtxFormat(rgba8, false, BC_DEVICE) == Format::R8G8B8A8_UNORM);
    CHECK(getKtxMipCount(rgba8, Format::R8G8B8A8_UNORM) == 7);

    // BC5 has no sRGB form, so it stays UNORM in a color slot.
    const KtxInfo bc5 = {.width = 64, .height = 64, .levelCount = 7, .components = 2, .format = Format::BC5_UNORM};
    CHECK(chooseKtxFormat(bc5, true, BC_DEVICE) == Format::BC5_UNORM);
}

TEST_CASE("UASTC and ETC1S files transcode to block formats with the file's mips")
{
    const auto uastc = makeKtx2(64, 32, 4, 7, Codec::UASTC, true);
    const TextureRaw bc7 = decode(uastc, Format::BC7_SRGB);
    CHECK(bc7.format == Format::BC7_SRGB);
    CHECK(bc7.width == 64);
    CHECK(bc7.height == 32);
    checkLevels(bc7, 64, 32, 7);
    // 16 x 8 blocks of 16 bytes: a quarter of RGBA8.
    CHECK(bc7.levelSizes[0] == 64 * 32);

    checkLevels(decode(uastc, Format::ASTC_4x4_SRGB), 64, 32, 7);
    checkLevels(decode(uastc, Format::ETC2_R8G8B8A8_SRGB), 64, 32, 7);

    const auto normals = makeKtx2(32, 32, 2, 6, Codec::ETC1S, false);
    const TextureRaw bc5 = decode(normals, Format::BC5_UNORM);
    checkLevels(bc5, 32, 32, 6);
    checkLevels(decode(normals, Format::EAC_R11G11_UNORM), 32, 32, 6);

    const auto occlusion = makeKtx2(32, 32, 1, 6, Codec::UASTC, false);
    const TextureRaw bc4 = decode(occlusion, Format::BC4_UNORM);
    checkLevels(bc4, 32, 32, 6);
    CHECK(bc4.levelSizes[0] == 32 * 32 / 2);
}

TEST_CASE("UASTC transcoded to RGBA8 keeps the texels")
{
    const TextureRaw raw = decode(makeKtx2(16, 16, 4, 5, Codec::UASTC, false), Format::R8G8B8A8_UNORM);
    checkLevels(raw, 16, 16, 5);
    int worst = 0;
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x)
            for (uint32_t c = 0; c < 4; ++c)
                worst = std::max(worst, std::abs(raw.levels[(y * 16 + x) * 4 + c] - texelValue(x, y, c, 0)));
    CHECK(worst <= 8);
}

TEST_CASE("a two-channel normal map transcoded to RGBA8 gets x, y and its rebuilt z")
{
    for (const Codec codec : {Codec::UASTC, Codec::ETC1S})
    {
        CAPTURE(static_cast<int>(codec));
        const TextureRaw raw = decode(makeKtx2(8, 8, 2, 4, codec, false), Format::R8G8B8A8_UNORM);
        checkLevels(raw, 8, 8, 4);
        // ETC1S loses more than UASTC; the rebuilt z must fit the x and y it got either way.
        const int tolerance = codec == Codec::UASTC ? 8 : 24;
        for (uint32_t i = 0; i < 64; ++i)
        {
            const unsigned char *texel = raw.levels.data() + i * 4;
            CHECK(std::abs(texel[0] - texelValue(i % 8, i / 8, 0, 0)) <= tolerance);
            CHECK(std::abs(texel[1] - texelValue(i % 8, i / 8, 1, 0)) <= tolerance);
            const float x = texel[0] / 127.5f - 1.0f;
            const float y = texel[1] / 127.5f - 1.0f;
            const float z = texel[2] / 127.5f - 1.0f;
            CHECK(x * x + y * y + z * z == doctest::Approx(1.0f).epsilon(0.03));
            CHECK(texel[3] == 255);
        }
    }
}

TEST_CASE("a Basis file without mips decodes to RGBA8 with the mips made on the CPU")
{
    const auto file = makeKtx2(32, 8, 4, 1, Codec::ETC1S, true);
    const KtxInfo info = readInfo(file);
    const Format format = chooseKtxFormat(info, true, BC_DEVICE);
    REQUIRE(format == Format::R8G8B8A8_SRGB);
    const TextureRaw raw = decode(file, format);
    checkLevels(raw, 32, 8, getKtxMipCount(info, format));
    CHECK(raw.levelSizes.size() == 6);
}

TEST_CASE("a file that is not Basis is decoded as it is, and zstd is inflated")
{
    const auto file = makeKtx2(8, 8, 4, 4, Codec::NONE, false);
    const TextureRaw raw = decode(file, Format::R8G8B8A8_SRGB);
    checkLevels(raw, 8, 8, 4);
    CHECK(raw.levels[0] == texelValue(0, 0, 0, 0));
    CHECK(raw.levels[raw.levelSizes[0]] == texelValue(0, 0, 0, 1));

    TextureRaw wrong;
    std::string error;
    CHECK_FALSE(decodeKtx(file.data(), file.size(), Format::BC7_UNORM, wrong, error));
}

TEST_CASE("glTF KHR_texture_basisu images are transcoded on import, once per kind of slot")
{
    const std::string name = "rasm_tests_" + std::to_string(std::random_device{}()) + ".ktx2";
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    {
        const auto file = makeKtx2(16, 16, 4, 5, Codec::UASTC, true);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    }

    // Image 0 is the base color of material 0 and the occlusion of material 1, so it is transcoded for a color and
    // a data slot. Texture 1 has no basisu source.
    const GLTFRaw raw = importMeshText(R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_basisu"],"extensionsRequired":["KHR_texture_basisu"],
        "images":[{"uri":")" + name + R"(","mimeType":"image/ktx2"}],
        "textures":[{"extensions":{"KHR_texture_basisu":{"source":0}}},{"source":5}],
        "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},{"occlusionTexture":{"index":0},"normalTexture":{"index":1}}]})",
                                       ".gltf", BC_DEVICE);
    std::filesystem::remove(path);

    REQUIRE(raw.loaded);
    REQUIRE(raw.ktxImages.size() == 2);
    REQUIRE(raw.ktxImages.contains(1));
    CHECK(raw.ktxImages.at(1).format == Format::BC7_SRGB);
    checkLevels(raw.ktxImages.at(1), 16, 16, 5);
    REQUIRE(raw.ktxImages.contains(0));
    CHECK(raw.ktxImages.at(0).format == Format::BC7_UNORM);
    // The file bytes go once the images are transcoded.
    CHECK(raw.model->images[0].image.empty());
}
