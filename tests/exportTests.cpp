#include "renderer/export.h"
#include "renderer/imageWriter.h"
#include "renderer/labelPlacement.h"
#include "platform/paths.h"

#include "doctest/doctest.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/packing.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    std::vector<uint8_t> readFile(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    uint32_t readBigEndian(const uint8_t *bytes)
    {
        return static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 | static_cast<uint32_t>(bytes[2]) << 8 | bytes[3];
    }

    uint8_t paeth(int a, int b, int c)
    {
        const int p = a + b - c;
        const int pa = std::abs(p - a);
        const int pb = std::abs(p - b);
        const int pc = std::abs(p - c);
        return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
    }

    // The rows of a PNG as ImageWriter writes it (RGBA, every row Paeth filtered), samples as they are in the file.
    std::vector<uint8_t> decodePng(const std::vector<uint8_t> &file, uint32_t &width, uint32_t &height, uint32_t &bits)
    {
        std::vector<uint8_t> data;
        size_t at = 8;
        while (at + 12 <= file.size())
        {
            const uint32_t size = readBigEndian(&file[at]);
            const std::string type(reinterpret_cast<const char *>(&file[at + 4]), 4);
            const uLong crc = crc32(crc32(0L, Z_NULL, 0), &file[at + 4], size + 4);
            CHECK(crc == readBigEndian(&file[at + 8 + size]));
            if (type == "IHDR")
            {
                width = readBigEndian(&file[at + 8]);
                height = readBigEndian(&file[at + 12]);
                bits = file[at + 16];
            }
            if (type == "IDAT")
                data.insert(data.end(), file.begin() + static_cast<std::ptrdiff_t>(at + 8), file.begin() + static_cast<std::ptrdiff_t>(at + 8 + size));
            at += 12 + size;
        }
        const size_t pixelBytes = 4 * bits / 8;
        const size_t rowBytes = width * pixelBytes;
        std::vector<uint8_t> raw(height * (rowBytes + 1));
        uLongf rawSize = static_cast<uLongf>(raw.size());
        REQUIRE(uncompress(raw.data(), &rawSize, data.data(), static_cast<uLong>(data.size())) == Z_OK);
        std::vector<uint8_t> rows(height * rowBytes);
        for (uint32_t y = 0; y < height; ++y)
        {
            CHECK(raw[y * (rowBytes + 1)] == 4);
            for (size_t i = 0; i < rowBytes; ++i)
            {
                const int a = i >= pixelBytes ? rows[y * rowBytes + i - pixelBytes] : 0;
                const int b = y > 0 ? rows[(y - 1) * rowBytes + i] : 0;
                const int c = i >= pixelBytes && y > 0 ? rows[(y - 1) * rowBytes + i - pixelBytes] : 0;
                rows[y * rowBytes + i] = static_cast<uint8_t>(raw[y * (rowBytes + 1) + 1 + i] + paeth(a, b, c));
            }
        }
        return rows;
    }

    // The A, B, G, R planes of an EXR as ImageWriter writes it, back to R, G, B, A per pixel.
    std::vector<uint8_t> decodeExr(const std::vector<uint8_t> &file, uint32_t width, uint32_t height, uint32_t sampleBytes)
    {
        REQUIRE(file.size() > 8);
        CHECK(file[0] == 0x76);
        CHECK(file[3] == 0x01);
        size_t at = 8;
        while (file[at] != 0)
        {
            at += std::strlen(reinterpret_cast<const char *>(&file[at])) + 1;
            at += std::strlen(reinterpret_cast<const char *>(&file[at])) + 1;
            int32_t size = 0;
            std::memcpy(&size, &file[at], 4);
            at += 4 + static_cast<size_t>(size);
        }
        ++at;
        const uint32_t blocks = (height + 15) / 16;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4 * sampleBytes);
        for (uint32_t b = 0; b < blocks; ++b)
        {
            uint64_t offset = 0;
            std::memcpy(&offset, &file[at + static_cast<size_t>(b) * 8], 8);
            int32_t line = 0;
            int32_t size = 0;
            std::memcpy(&line, &file[offset], 4);
            std::memcpy(&size, &file[offset + 4], 4);
            CHECK(line == static_cast<int32_t>(b * 16));
            const uint32_t lines = std::min(16u, height - b * 16);
            const size_t rawSize = static_cast<size_t>(lines) * width * 4 * sampleBytes;
            std::vector<uint8_t> raw(rawSize);
            if (static_cast<size_t>(size) < rawSize)
            {
                std::vector<uint8_t> predicted(rawSize);
                uLongf outSize = static_cast<uLongf>(rawSize);
                REQUIRE(uncompress(predicted.data(), &outSize, &file[offset + 8], static_cast<uLong>(size)) == Z_OK);
                for (size_t i = 1; i < rawSize; ++i)
                    predicted[i] = static_cast<uint8_t>(predicted[i - 1] + predicted[i] - 128);
                const size_t half = (rawSize + 1) / 2;
                for (size_t i = 0; i < rawSize; ++i)
                    raw[i] = (i & 1) == 0 ? predicted[i / 2] : predicted[half + i / 2];
            }
            else
                std::memcpy(raw.data(), &file[offset + 8], rawSize);
            for (uint32_t y = 0; y < lines; ++y)
                for (uint32_t c = 0; c < 4; ++c)
                    for (uint32_t x = 0; x < width; ++x)
                        std::memcpy(&pixels[((static_cast<size_t>(b * 16 + y) * width + x) * 4 + (3 - c)) * sampleBytes], &raw[((static_cast<size_t>(y) * 4 + c) * width + x) * sampleBytes],
                                    sampleBytes);
        }
        return pixels;
    }

    std::filesystem::path writeImage(rasm::ExportFormat format, const std::vector<uint8_t> &rows, uint32_t width, uint32_t height, const char *name)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        rasm::ImageWriter writer;
        REQUIRE(writer.start(rasm::pathToUtf8(path), format, width, height));
        // In two pushes, as bands of tiles come.
        const size_t rowBytes = rows.size() / height;
        const uint32_t first = height / 2 + 1;
        writer.push(std::vector<uint8_t>(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(first * rowBytes)), first);
        writer.push(std::vector<uint8_t>(rows.begin() + static_cast<std::ptrdiff_t>(first * rowBytes), rows.end()), height - first);
        writer.finish();
        while (!writer.finished())
        {
        }
        CHECK_FALSE(writer.failed());
        return path;
    }
}

TEST_CASE("an export's tiles cover the image once, inside render rects of one size aligned to 64, with their guard")
{
    rasm::ExportLayout layout;
    REQUIRE(rasm::getExportLayout(1000, 700, 256, rasm::EXPORT_BLOOM_GUARD, 16384, layout));
    CHECK(layout.virtualWidth == 1024);
    CHECK(layout.virtualHeight == 704);
    CHECK(layout.columns == 4);
    CHECK(layout.rows == 3);
    CHECK(layout.renderWidth == 1024);
    CHECK(layout.renderHeight == 704);

    REQUIRE(rasm::getExportLayout(5000, 3000, 1000, 100, 16384, layout));
    CHECK(layout.tileSize == 960);
    CHECK(layout.guard == 128);
    CHECK(layout.renderWidth == 960 + 256);
    std::vector<uint32_t> covered(static_cast<size_t>(5000) * 3000, 0);
    for (uint32_t t = 0; t < layout.columns * layout.rows; ++t)
    {
        const rasm::ExportTile tile = rasm::getExportTile(layout, t);
        CHECK(tile.renderX % rasm::EXPORT_ALIGNMENT == 0);
        CHECK(tile.renderY % rasm::EXPORT_ALIGNMENT == 0);
        CHECK(tile.renderX + layout.renderWidth <= layout.virtualWidth);
        CHECK(tile.renderY + layout.renderHeight <= layout.virtualHeight);
        // The guard is inside the render rect on every side that is not the image's edge.
        CHECK((tile.x == 0 || tile.x >= tile.renderX + layout.guard));
        CHECK((tile.x + tile.width >= 5000 || tile.x + tile.width + layout.guard <= tile.renderX + layout.renderWidth));
        for (uint32_t y = tile.y; y < tile.y + tile.height; ++y)
            for (uint32_t x = tile.x; x < tile.x + tile.width; ++x)
                ++covered[static_cast<size_t>(y) * 5000 + x];
    }
    CHECK(std::count(covered.begin(), covered.end(), 1u) == static_cast<std::ptrdiff_t>(covered.size()));

    // The device's largest image bounds a tile and its guard.
    REQUIRE(rasm::getExportLayout(20000, 20000, 8192, 384, 4096, layout));
    CHECK(layout.renderWidth <= 4096);
    CHECK_FALSE(rasm::getExportLayout(20000, 20000, 8192, 384, 512, layout));
    CHECK_FALSE(rasm::getExportLayout(0, 100, 256, 64, 4096, layout));
}

TEST_CASE("a tile's clip matrix moves a point of the image to the same pixel of its render rect, for any projection")
{
    rasm::ExportLayout layout;
    REQUIRE(rasm::getExportLayout(3000, 2000, 512, 64, 16384, layout));
    const glm::dmat4 perspective = glm::perspective(glm::radians(50.0), 1.5, 0.1, 100.0);
    const glm::dmat4 orthographic = glm::ortho(-30.0, 30.0, -20.0, 20.0, 0.1, 100.0);
    for (const glm::dmat4 &projection : {perspective, orthographic})
    {
        for (uint32_t t : {0u, 7u, 13u, layout.columns * layout.rows - 1})
        {
            const rasm::ExportTile tile = rasm::getExportTile(layout, t);
            const glm::dmat4 clip = rasm::getExportTileClip(layout, tile);
            for (const glm::dvec3 point : {glm::dvec3(1.0, 2.0, -10.0), glm::dvec3(-4.0, -1.5, -7.0), glm::dvec3(0.3, 0.1, -50.0)})
            {
                const glm::dvec4 whole = projection * glm::dvec4(point, 1.0);
                const glm::dvec4 inTile = clip * whole;
                // Pixels from the top-left corner: x right, y down (clip y is up).
                const glm::dvec2 imagePixel((whole.x / whole.w * 0.5 + 0.5) * 3000.0, (0.5 - whole.y / whole.w * 0.5) * 2000.0);
                const glm::dvec2 tilePixel((inTile.x / inTile.w * 0.5 + 0.5) * layout.renderWidth, (0.5 - inTile.y / inTile.w * 0.5) * layout.renderHeight);
                CHECK(tilePixel.x + tile.renderX == doctest::Approx(imagePixel.x).epsilon(1e-9));
                CHECK(tilePixel.y + tile.renderY == doctest::Approx(imagePixel.y).epsilon(1e-9));
                CHECK(inTile.z == doctest::Approx(whole.z));
                CHECK(inTile.w == doctest::Approx(whole.w));
            }
        }
    }
}

TEST_CASE("an export's jitter is a stratified grid for a square count and Halton otherwise, inside the pixel")
{
    CHECK(rasm::getExportJitter(0, 1) == glm::vec2(0.0f));
    CHECK(rasm::getExportJitter(0, 16) == glm::vec2(-0.375f, -0.375f));
    CHECK(rasm::getExportJitter(15, 16) == glm::vec2(0.375f, 0.375f));
    glm::vec2 sum(0.0f);
    for (uint32_t i = 0; i < 16; ++i)
        sum += rasm::getExportJitter(i, 16);
    CHECK(sum.x == doctest::Approx(0.0f));
    CHECK(sum.y == doctest::Approx(0.0f));
    for (uint32_t i = 0; i < 7; ++i)
    {
        const glm::vec2 jitter = rasm::getExportJitter(i, 7);
        CHECK(std::abs(jitter.x) < 0.5f);
        CHECK(std::abs(jitter.y) < 0.5f);
    }
    CHECK(rasm::getExportJitter(0, 7) != rasm::getExportJitter(1, 7));
}

TEST_CASE("an export's survey tests each label in the one tile that keeps its anchor, and its tiles keep the fades")
{
    rasm::ExportLayout layout;
    REQUIRE(rasm::getExportLayout(1024, 1024, 256, 64, 16384, layout));
    rasm::ExportViewFrame frame{.survey = true, .imageWidth = 1024, .imageHeight = 1024};
    const glm::vec4 probe(300.5f, 260.25f, 0.4f, rasm::LABEL_PROBE_DEPTH);
    uint32_t tested = 0;
    for (uint32_t t = 0; t < layout.columns * layout.rows; ++t)
    {
        frame.tile = rasm::getExportTile(layout, t);
        const glm::vec4 result = rasm::getExportLabelProbe(frame, probe);
        if (result.w == rasm::LABEL_PROBE_NONE)
            continue;
        ++tested;
        CHECK(result.w == rasm::LABEL_PROBE_DEPTH + rasm::LABEL_PROBE_RESET);
        CHECK(result.x == doctest::Approx(300.5f - static_cast<float>(frame.tile.renderX)));
        CHECK(result.y == doctest::Approx(260.25f - static_cast<float>(frame.tile.renderY)));
        CHECK(frame.tile.x == 256);
        CHECK(frame.tile.y == 256);
    }
    CHECK(tested == 1);
    // An anchor past the image's edge is tested where a single render's test clamps it.
    frame.tile = rasm::getExportTile(layout, layout.columns * layout.rows - 1);
    CHECK(rasm::getExportLabelProbe(frame, glm::vec4(1024.7f, 1030.0f, 0.4f, rasm::LABEL_PROBE_OVERLAY + rasm::LABEL_PROBE_RESET)).w == rasm::LABEL_PROBE_OVERLAY + rasm::LABEL_PROBE_RESET);
    // In the tiles a depth label takes its fade, an overlay stays as placed, and nothing starts over.
    frame.survey = false;
    frame.frozen = true;
    CHECK(rasm::getExportLabelProbe(frame, probe + glm::vec4(0.0f, 0.0f, 0.0f, rasm::LABEL_PROBE_RESET)).w == rasm::LABEL_PROBE_DEPTH);
    CHECK(rasm::getExportLabelProbe(frame, glm::vec4(1.0f, 1.0f, 0.4f, rasm::LABEL_PROBE_OVERLAY)).w == rasm::LABEL_PROBE_NONE);
    CHECK(rasm::getExportLabelProbe(frame, glm::vec4(0.0f)).w == rasm::LABEL_PROBE_NONE);
}

TEST_CASE("the image writer's PNGs and EXRs hold the rows it was given")
{
    constexpr uint32_t WIDTH = 37;
    constexpr uint32_t HEIGHT = 41;
    constexpr size_t VALUES = static_cast<size_t>(WIDTH) * HEIGHT * 4;
    std::vector<uint8_t> rgba8(VALUES);
    std::vector<uint16_t> rgba16(VALUES);
    std::vector<float> rgba32(VALUES);
    std::vector<uint16_t> halves(VALUES);
    for (size_t i = 0; i < VALUES; ++i)
    {
        rgba8[i] = static_cast<uint8_t>(i * 7 + i / 13);
        rgba16[i] = static_cast<uint16_t>(i * 1031 + i / 7);
        rgba32[i] = static_cast<float>(i % 97) * 0.37f - 3.0f;
        halves[i] = glm::packHalf1x16(rgba32[i]);
    }

    SUBCASE("PNG8")
    {
        const auto path = writeImage(rasm::ExportFormat::PNG8, rgba8, WIDTH, HEIGHT, "rasm_export_test8.png");
        rasm::ReadbackImage image;
        REQUIRE(rasm::readPng(rasm::pathToUtf8(path), image));
        CHECK(image.width == WIDTH);
        CHECK(image.pixels == rgba8);
        std::filesystem::remove(path);
    }
    SUBCASE("PNG16")
    {
        std::vector<uint8_t> bytes(rgba16.size() * 2);
        std::memcpy(bytes.data(), rgba16.data(), bytes.size());
        const auto path = writeImage(rasm::ExportFormat::PNG16, bytes, WIDTH, HEIGHT, "rasm_export_test16.png");
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bits = 0;
        const std::vector<uint8_t> rows = decodePng(readFile(path), width, height, bits);
        CHECK(width == WIDTH);
        CHECK(height == HEIGHT);
        CHECK(bits == 16);
        for (size_t i = 0; i < rgba16.size(); ++i)
            CHECK(static_cast<uint16_t>(rows[i * 2] << 8 | rows[i * 2 + 1]) == rgba16[i]);
        std::filesystem::remove(path);
    }
    SUBCASE("EXR half and float")
    {
        std::vector<uint8_t> halfBytes(halves.size() * 2);
        std::memcpy(halfBytes.data(), halves.data(), halfBytes.size());
        auto path = writeImage(rasm::ExportFormat::EXR_HALF, halfBytes, WIDTH, HEIGHT, "rasm_export_test_half.exr");
        CHECK(decodeExr(readFile(path), WIDTH, HEIGHT, 2) == halfBytes);
        std::filesystem::remove(path);
        std::vector<uint8_t> floatBytes(rgba32.size() * 4);
        std::memcpy(floatBytes.data(), rgba32.data(), floatBytes.size());
        path = writeImage(rasm::ExportFormat::EXR_FLOAT, floatBytes, WIDTH, HEIGHT, "rasm_export_test_float.exr");
        CHECK(decodeExr(readFile(path), WIDTH, HEIGHT, 4) == floatBytes);
        std::filesystem::remove(path);
    }
    SUBCASE("a writer destroyed before it finished removes its file")
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "rasm_export_cancelled.png";
        {
            rasm::ImageWriter writer;
            REQUIRE(writer.start(rasm::pathToUtf8(path), rasm::ExportFormat::PNG8, WIDTH, HEIGHT));
            writer.push(std::vector<uint8_t>(rgba8.begin(), rgba8.begin() + static_cast<std::ptrdiff_t>(WIDTH) * 4 * 3), 3);
        }
        CHECK_FALSE(std::filesystem::exists(path));
    }
}
