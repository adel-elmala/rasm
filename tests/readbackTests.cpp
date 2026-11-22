#include "renderer/readback.h"
#include "platform/paths.h"

#include "doctest/doctest.h"

#include "glm/gtc/packing.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{
    float pixelFloat(const rasm::ReadbackImage &image, size_t pixel, int channel)
    {
        float value = 0.0f;
        std::memcpy(&value, image.pixels.data() + pixel * 16 + channel * 4, 4);
        return value;
    }
}

TEST_CASE("readbacks convert the formats they know and refuse the others")
{
    CHECK(rasm::getReadbackTexelSize(rasm::Format::R8G8B8A8_SRGB) == 4);
    CHECK(rasm::getReadbackTexelSize(rasm::Format::B8G8R8A8_UNORM) == 4);
    CHECK(rasm::getReadbackTexelSize(rasm::Format::R16G16B16A16_SFLOAT) == 8);
    CHECK(rasm::getReadbackTexelSize(rasm::Format::R32G32B32A32_SFLOAT) == 16);
    CHECK(rasm::getReadbackTexelSize(rasm::Format::D32_SFLOAT) == 4);
    CHECK(rasm::getReadbackTexelSize(rasm::Format::BC7_SRGB) == 0);
    CHECK(rasm::getReadbackTexelSize(rasm::Format::R8G8B8_UNORM) == 0);

    rasm::ReadbackImage image;
    const uint8_t texel[16] = {};
    CHECK_FALSE(rasm::convertReadback(rasm::Format::BC7_SRGB, texel, 1, 1, rasm::ReadbackFormat::RGBA8_SRGB, image));
    // A depth has no color to encode: it reads only as floats.
    CHECK_FALSE(rasm::convertReadback(rasm::Format::D32_SFLOAT, texel, 1, 1, rasm::ReadbackFormat::RGBA8_SRGB, image));
}

TEST_CASE("8-bit texels keep their bytes as RGBA8, with BGRA swizzled")
{
    const uint8_t bgra[8] = {10, 20, 30, 40, 200, 150, 100, 255};
    rasm::ReadbackImage image;
    REQUIRE(rasm::convertReadback(rasm::Format::B8G8R8A8_SRGB, bgra, 2, 1, rasm::ReadbackFormat::RGBA8_SRGB, image));
    CHECK(image.width == 2);
    CHECK(image.height == 1);
    REQUIRE(image.pixels.size() == 8);
    const uint8_t expected[8] = {30, 20, 10, 40, 100, 150, 200, 255};
    CHECK(std::memcmp(image.pixels.data(), expected, 8) == 0);

    const uint8_t rgba[4] = {1, 2, 3, 4};
    REQUIRE(rasm::convertReadback(rasm::Format::R8G8B8A8_UNORM, rgba, 1, 1, rasm::ReadbackFormat::RGBA8_SRGB, image));
    CHECK(std::memcmp(image.pixels.data(), rgba, 4) == 0);
}

TEST_CASE("sRGB texels decode to linear floats, UNORM ones only scale")
{
    const uint8_t texels[8] = {188, 0, 255, 128, 188, 0, 255, 128};
    rasm::ReadbackImage srgb;
    REQUIRE(rasm::convertReadback(rasm::Format::R8G8B8A8_SRGB, texels, 1, 1, rasm::ReadbackFormat::RGBA32_FLOAT, srgb));
    REQUIRE(srgb.pixels.size() == 16);
    CHECK(pixelFloat(srgb, 0, 0) == doctest::Approx(0.5029f).epsilon(0.001));
    CHECK(pixelFloat(srgb, 0, 1) == doctest::Approx(0.0f));
    CHECK(pixelFloat(srgb, 0, 2) == doctest::Approx(1.0f));
    CHECK(pixelFloat(srgb, 0, 3) == doctest::Approx(128.0f / 255.0f)); // alpha is linear

    rasm::ReadbackImage unorm;
    REQUIRE(rasm::convertReadback(rasm::Format::R8G8B8A8_UNORM, texels + 4, 1, 1, rasm::ReadbackFormat::RGBA32_FLOAT, unorm));
    CHECK(pixelFloat(unorm, 0, 0) == doctest::Approx(188.0f / 255.0f));
}

TEST_CASE("half floats read as floats, and as RGBA8 clamped and sRGB-encoded")
{
    const uint16_t halfs[8] = {glm::packHalf1x16(0.5f), glm::packHalf1x16(2.0f), glm::packHalf1x16(-1.0f), glm::packHalf1x16(1.0f),
                               glm::packHalf1x16(0.0f), glm::packHalf1x16(0.0031308f), glm::packHalf1x16(1.0f), glm::packHalf1x16(0.5f)};
    rasm::ReadbackImage floats;
    REQUIRE(rasm::convertReadback(rasm::Format::R16G16B16A16_SFLOAT, reinterpret_cast<const uint8_t *>(halfs), 2, 1, rasm::ReadbackFormat::RGBA32_FLOAT, floats));
    CHECK(pixelFloat(floats, 0, 0) == doctest::Approx(0.5f));
    CHECK(pixelFloat(floats, 0, 1) == doctest::Approx(2.0f));
    CHECK(pixelFloat(floats, 0, 2) == doctest::Approx(-1.0f));

    rasm::ReadbackImage bytes;
    REQUIRE(rasm::convertReadback(rasm::Format::R16G16B16A16_SFLOAT, reinterpret_cast<const uint8_t *>(halfs), 2, 1, rasm::ReadbackFormat::RGBA8_SRGB, bytes));
    CHECK(bytes.pixels[0] == 188); // 0.5 linear is 188 in sRGB
    CHECK(bytes.pixels[1] == 255); // clamped
    CHECK(bytes.pixels[2] == 0);
    CHECK(bytes.pixels[3] == 255);
    CHECK(bytes.pixels[7] == 128); // alpha is not encoded
}

TEST_CASE("two-channel and depth texels fill the missing channels with 0, 0, 1")
{
    const uint16_t motion[2] = {glm::packHalf1x16(0.25f), glm::packHalf1x16(-0.125f)};
    rasm::ReadbackImage image;
    REQUIRE(rasm::convertReadback(rasm::Format::R16G16_SFLOAT, reinterpret_cast<const uint8_t *>(motion), 1, 1, rasm::ReadbackFormat::RGBA32_FLOAT, image));
    CHECK(pixelFloat(image, 0, 0) == doctest::Approx(0.25f));
    CHECK(pixelFloat(image, 0, 1) == doctest::Approx(-0.125f));
    CHECK(pixelFloat(image, 0, 2) == doctest::Approx(0.0f));
    CHECK(pixelFloat(image, 0, 3) == doctest::Approx(1.0f));

    const float depth = 0.75f;
    REQUIRE(rasm::convertReadback(rasm::Format::D32_SFLOAT, reinterpret_cast<const uint8_t *>(&depth), 1, 1, rasm::ReadbackFormat::RGBA32_FLOAT, image));
    CHECK(pixelFloat(image, 0, 0) == doctest::Approx(0.75f));
    CHECK(pixelFloat(image, 0, 3) == doctest::Approx(1.0f));
}

TEST_CASE("writePng writes an RGBA8 image that readPng reads back the same, and both refuse what they can't do")
{
    rasm::ReadbackImage image{.width = 3, .height = 2, .format = rasm::ReadbackFormat::RGBA8_SRGB};
    for (uint8_t i = 0; i < 24; ++i)
        image.pixels.push_back(static_cast<uint8_t>(i * 10));
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "rasm_readback_test.png";
    const std::string utf8 = rasm::pathToUtf8(path);
    REQUIRE(rasm::writePng(utf8, image));

    rasm::ReadbackImage read;
    REQUIRE(rasm::readPng(utf8, read));
    CHECK(read.width == 3);
    CHECK(read.height == 2);
    CHECK(read.format == rasm::ReadbackFormat::RGBA8_SRGB);
    CHECK(read.pixels == image.pixels);
    std::filesystem::remove(path);
    CHECK_FALSE(rasm::readPng(utf8, read));

    image.format = rasm::ReadbackFormat::RGBA32_FLOAT;
    CHECK_FALSE(rasm::writePng(utf8, image));
}
