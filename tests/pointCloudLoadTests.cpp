#include "rasm/assetLoader.h"
#include "rasm/renderer.h"

#include "doctest/doctest.h"
#include "laszip/laszip_api.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    struct TestPoint
    {
        glm::dvec3 position = glm::dvec3(0.0);
        uint16_t intensity = 0;
        uint8_t classification = 0;
        uint8_t returnNumber = 1;
        double gpsTime = 0.0;
        uint16_t rgb[3] = {};
    };

    struct LasFile
    {
        uint32_t format = 0;
        uint32_t versionMinor = 2;
        uint32_t extraBytes = 0;
        glm::dvec3 scale = glm::dvec3(0.001);
        glm::dvec3 offset = glm::dvec3(4197000.0, 168000.0, 4786000.0);
        std::string wkt;
    };

    // Point formats 0 to 10: record sizes and where the GPS time and the colors are (LAS 1.4 R15, section 2.6).
    constexpr uint32_t SIZES[] = {20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};
    constexpr int32_t GPS[] = {-1, 20, -1, 20, 20, 20, 22, 22, 22, 22, 22};
    constexpr int32_t RGB[] = {-1, -1, 20, 28, -1, 28, -1, 30, 30, -1, 30};

    void put(std::vector<uint8_t> &bytes, size_t at, const void *value, size_t size)
    {
        if (bytes.size() < at + size)
            bytes.resize(at + size, 0);
        std::memcpy(bytes.data() + at, value, size);
    }

    void putU8(std::vector<uint8_t> &bytes, size_t at, uint8_t v) { put(bytes, at, &v, 1); }
    void putU16(std::vector<uint8_t> &bytes, size_t at, uint16_t v) { put(bytes, at, &v, 2); }
    void putU32(std::vector<uint8_t> &bytes, size_t at, uint32_t v) { put(bytes, at, &v, 4); }
    void putU64(std::vector<uint8_t> &bytes, size_t at, uint64_t v) { put(bytes, at, &v, 8); }
    void putF64(std::vector<uint8_t> &bytes, size_t at, double v) { put(bytes, at, &v, 8); }

    // A LAS file as the spec lays it out, written byte by byte.
    std::vector<uint8_t> writeLas(const LasFile &file, const std::vector<TestPoint> &points)
    {
        const bool v14 = file.versionMinor >= 4;
        const uint32_t headerSize = v14 ? 375 : 227;
        const uint32_t vlrSize = file.wkt.empty() ? 0 : 54 + static_cast<uint32_t>(file.wkt.size()) + 1;
        const uint32_t recordLength = SIZES[file.format] + file.extraBytes;
        std::vector<uint8_t> bytes(headerSize, 0);
        std::memcpy(bytes.data(), "LASF", 4);
        putU8(bytes, 24, 1);
        putU8(bytes, 25, static_cast<uint8_t>(file.versionMinor));
        putU16(bytes, 94, static_cast<uint16_t>(headerSize));
        putU32(bytes, 96, headerSize + vlrSize);
        putU32(bytes, 100, file.wkt.empty() ? 0 : 1);
        putU8(bytes, 104, static_cast<uint8_t>(file.format));
        putU16(bytes, 105, static_cast<uint16_t>(recordLength));
        putU32(bytes, 107, file.format >= 6 ? 0u : static_cast<uint32_t>(points.size()));
        for (int a = 0; a < 3; ++a)
        {
            putF64(bytes, 131 + 8 * a, file.scale[a]);
            putF64(bytes, 155 + 8 * a, file.offset[a]);
            putF64(bytes, 179 + 16 * a, 1e9);
            putF64(bytes, 187 + 16 * a, -1e9);
        }
        if (v14)
            putU64(bytes, 247, points.size());
        if (!file.wkt.empty())
        {
            const size_t at = headerSize;
            put(bytes, at + 2, "LASF_Projection", 15);
            putU16(bytes, at + 18, 2112);
            putU16(bytes, at + 20, static_cast<uint16_t>(file.wkt.size() + 1));
            put(bytes, at + 54, file.wkt.c_str(), file.wkt.size() + 1);
        }
        for (const TestPoint &point : points)
        {
            const size_t at = bytes.size();
            bytes.resize(at + recordLength, 0xAB);
            const glm::dvec3 steps = glm::round((point.position - file.offset) / file.scale);
            for (int a = 0; a < 3; ++a)
                putU32(bytes, at + 4 * a, static_cast<uint32_t>(static_cast<int32_t>(steps[a])));
            putU16(bytes, at + 12, point.intensity);
            if (file.format >= 6)
            {
                putU8(bytes, at + 14, static_cast<uint8_t>(point.returnNumber | 0x30));
                putU8(bytes, at + 15, 0);
                putU8(bytes, at + 16, point.classification);
            }
            else
            {
                putU8(bytes, at + 14, static_cast<uint8_t>(point.returnNumber | 0x18));
                putU8(bytes, at + 15, static_cast<uint8_t>(point.classification | 0x80));
            }
            if (GPS[file.format] >= 0)
                putF64(bytes, at + GPS[file.format], point.gpsTime);
            if (RGB[file.format] >= 0)
            {
                for (int c = 0; c < 3; ++c)
                    putU16(bytes, at + RGB[file.format] + 2 * c, point.rgb[c]);
            }
        }
        return bytes;
    }

    std::string writeFile(const std::string &name, const std::vector<uint8_t> &bytes)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / ("rasm_" + name);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return path.string();
    }

    std::vector<TestPoint> makePoints(bool wideColors)
    {
        std::vector<TestPoint> points;
        for (uint32_t i = 0; i < 300; ++i)
        {
            TestPoint point;
            point.position = glm::dvec3(4197000.0 + 0.125 * i, 168000.0 - 0.5 * i, 4786000.0 + 0.001 * i);
            point.intensity = static_cast<uint16_t>(i * 7);
            point.classification = static_cast<uint8_t>(i % 20);
            point.returnNumber = static_cast<uint8_t>(1 + i % 5);
            point.gpsTime = 1000.0 + 0.25 * i;
            const uint16_t scale = wideColors ? 257 : 1;
            point.rgb[0] = static_cast<uint16_t>((i % 256) * scale);
            point.rgb[1] = static_cast<uint16_t>(((i * 3) % 256) * scale);
            point.rgb[2] = static_cast<uint16_t>(((i * 5) % 256) * scale);
            points.push_back(point);
        }
        return points;
    }

    void checkCloud(const rasm::PointCloudData &cloud, const std::vector<TestPoint> &points, uint32_t format)
    {
        REQUIRE(cloud.positions.size() == points.size());
        CHECK(cloud.pointFormat == format);
        const bool rgb = RGB[format] >= 0;
        CHECK(cloud.colors.size() == (rgb ? points.size() : 0));
        for (size_t i = 0; i < points.size(); ++i)
        {
            CHECK(glm::length(cloud.positions[i] - points[i].position) < 1e-6);
            CHECK(cloud.scalars[i] == static_cast<float>(points[i].intensity));
            if (rgb)
                CHECK(cloud.colors[i] == (static_cast<uint32_t>(i % 256) | static_cast<uint32_t>((i * 3) % 256) << 8 | static_cast<uint32_t>((i * 5) % 256) << 16 | 0xFF000000u));
        }
    }
}

TEST_CASE("loadPointCloud reads LAS point formats 0 to 10 in doubles, with their colors, scalars and coordinate system")
{
    rasm::Renderer renderer(rasm::RendererConfig{.cacheDir = ""});
    rasm::AssetLoader loader(renderer, {.loadThreads = 1});
    const std::vector<TestPoint> points = makePoints(true);
    for (uint32_t format = 0; format <= 10; ++format)
    {
        CAPTURE(format);
        const LasFile file{.format = format, .versionMinor = format >= 6 ? 4u : 2u, .extraBytes = format == 3 ? 5u : 0u, .wkt = format == 7 ? "GEOCCS[\"WGS 84\"]" : ""};
        const std::string path = writeFile("format" + std::to_string(format) + ".las", writeLas(file, points));
        rasm::PointCloudData cloud;
        REQUIRE(loader.loadPointCloud(path, cloud));
        checkCloud(cloud, points, format);
        CHECK(cloud.versionMinor == file.versionMinor);
        CHECK_FALSE(cloud.compressed);
        CHECK(cloud.coordinateSystem == file.wkt);
        std::filesystem::remove(path);
    }

    // 8-bit values in the 16-bit colors are taken as they are; the scalars and axes as asked.
    const std::vector<TestPoint> narrow = makePoints(false);
    const std::string path = writeFile("narrow.las", writeLas({.format = 3}, narrow));
    rasm::PointCloudData cloud;
    REQUIRE(loader.loadPointCloud(path, cloud));
    checkCloud(cloud, narrow, 3);
    for (const rasm::PointCloudScalar scalar : {rasm::PointCloudScalar::CLASSIFICATION, rasm::PointCloudScalar::GPS_TIME, rasm::PointCloudScalar::RETURN_NUMBER, rasm::PointCloudScalar::ELEVATION})
    {
        REQUIRE(loader.loadPointCloud(path, cloud, {.scalar = scalar, .zUpToYUp = true}));
        const glm::dvec3 &stored = narrow[42].position;
        CHECK(glm::length(cloud.positions[42] - glm::dvec3(stored.x, stored.z, -stored.y)) < 1e-6);
        const float expected[] = {static_cast<float>(narrow[42].classification), static_cast<float>(narrow[42].gpsTime), static_cast<float>(narrow[42].returnNumber),
                                  static_cast<float>(stored.z)};
        const size_t which = scalar == rasm::PointCloudScalar::CLASSIFICATION ? 0 : (scalar == rasm::PointCloudScalar::GPS_TIME ? 1 : (scalar == rasm::PointCloudScalar::RETURN_NUMBER ? 2 : 3));
        CHECK(cloud.scalars[42] == expected[which]);
    }
    REQUIRE(loader.loadPointCloud(path, cloud, {.scalar = rasm::PointCloudScalar::NONE}));
    CHECK(cloud.scalars.empty());
    std::filesystem::remove(path);
}

TEST_CASE("loadPointCloud refuses a file that is not LAS, an unknown format and a cut file")
{
    rasm::Renderer renderer(rasm::RendererConfig{.cacheDir = ""});
    rasm::AssetLoader loader(renderer, {.loadThreads = 1});
    rasm::PointCloudData cloud;
    CHECK_FALSE(loader.loadPointCloud((std::filesystem::temp_directory_path() / "rasm_missing.las").string(), cloud));
    const std::vector<TestPoint> points = makePoints(true);
    std::vector<uint8_t> bytes = writeLas({.format = 1}, points);
    std::vector<uint8_t> broken = bytes;
    broken[0] = 'X';
    const std::string notLas = writeFile("notlas.las", broken);
    CHECK_FALSE(loader.loadPointCloud(notLas, cloud));
    broken = bytes;
    broken[104] = 11;
    const std::string unknown = writeFile("format11.las", broken);
    CHECK_FALSE(loader.loadPointCloud(unknown, cloud));
    broken = bytes;
    broken.resize(broken.size() - 100);
    const std::string cut = writeFile("cut.las", broken);
    CHECK_FALSE(loader.loadPointCloud(cut, cloud));
    CHECK(cloud.positions.empty());
    for (const std::string &path : {notLas, unknown, cut})
        std::filesystem::remove(path);
}

TEST_CASE("loadPointCloud reads a LAZ file through LASzip")
{
    rasm::Renderer renderer(rasm::RendererConfig{.cacheDir = ""});
    rasm::AssetLoader loader(renderer, {.loadThreads = 1});
    const std::vector<TestPoint> points = makePoints(true);
    for (const uint32_t format : {3u, 7u})
    {
        CAPTURE(format);
        const std::string path = (std::filesystem::temp_directory_path() / ("rasm_format" + std::to_string(format) + ".laz")).string();
        laszip_POINTER writer = nullptr;
        REQUIRE(laszip_create(&writer) == 0);
        laszip_header *header = nullptr;
        REQUIRE(laszip_get_header_pointer(writer, &header) == 0);
        header->version_major = 1;
        header->version_minor = format >= 6 ? 4 : 2;
        header->header_size = format >= 6 ? 375 : 227;
        header->offset_to_point_data = header->header_size;
        header->point_data_format = static_cast<laszip_U8>(format);
        header->point_data_record_length = static_cast<laszip_U16>(SIZES[format]);
        header->number_of_point_records = format >= 6 ? 0 : static_cast<laszip_U32>(points.size());
        header->extended_number_of_point_records = points.size();
        header->x_scale_factor = header->y_scale_factor = header->z_scale_factor = 0.001;
        header->x_offset = 4197000.0;
        header->y_offset = 168000.0;
        header->z_offset = 4786000.0;
        REQUIRE(laszip_open_writer(writer, path.c_str(), 1) == 0);
        laszip_point *point = nullptr;
        REQUIRE(laszip_get_point_pointer(writer, &point) == 0);
        for (const TestPoint &source : points)
        {
            const glm::dvec3 steps = glm::round((source.position - glm::dvec3(4197000.0, 168000.0, 4786000.0)) / 0.001);
            point->X = static_cast<laszip_I32>(steps.x);
            point->Y = static_cast<laszip_I32>(steps.y);
            point->Z = static_cast<laszip_I32>(steps.z);
            point->intensity = source.intensity;
            point->gps_time = source.gpsTime;
            if (format >= 6)
            {
                point->extended_point_type = 1;
                point->extended_classification = source.classification;
                point->extended_return_number = source.returnNumber;
                point->extended_number_of_returns = 5;
            }
            else
            {
                point->classification = source.classification;
                point->return_number = source.returnNumber;
                point->number_of_returns = 5;
            }
            for (int c = 0; c < 3; ++c)
                point->rgb[c] = source.rgb[c];
            REQUIRE(laszip_write_point(writer) == 0);
            REQUIRE(laszip_update_inventory(writer) == 0);
        }
        REQUIRE(laszip_close_writer(writer) == 0);
        laszip_destroy(writer);

        rasm::PointCloudData cloud;
        REQUIRE(loader.loadPointCloud(path, cloud, {.scalar = rasm::PointCloudScalar::INTENSITY}));
        CHECK(cloud.compressed);
        checkCloud(cloud, points, format);
        REQUIRE(loader.loadPointCloud(path, cloud, {.scalar = rasm::PointCloudScalar::CLASSIFICATION}));
        CHECK(cloud.scalars[19] == 19.0f);
        std::filesystem::remove(path);
    }
}
