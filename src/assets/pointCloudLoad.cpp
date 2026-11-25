#include "rasm/assetLoader.h"
#include "platform/paths.h"

#include "laszip/laszip_api.h"
#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace rasm
{
    namespace
    {
        // The public header block (LAS 1.4 R15, table 3), the fields a reader needs.
        struct LasHeader
        {
            uint32_t versionMinor = 0;
            uint32_t headerSize = 0;
            uint32_t pointOffset = 0;
            uint32_t vlrCount = 0;
            uint32_t format = 0;
            uint32_t recordLength = 0;
            uint64_t pointCount = 0;
            glm::dvec3 scale = glm::dvec3(1.0);
            glm::dvec3 offset = glm::dvec3(0.0);
            glm::dvec3 boundsMin = glm::dvec3(0.0);
            glm::dvec3 boundsMax = glm::dvec3(0.0);
            uint64_t evlrStart = 0;
            uint32_t evlrCount = 0;
            bool compressed = false;
        };

        // Where a point format keeps its fields, in bytes from the record's start; -1: it has none.
        struct PointLayout
        {
            uint32_t size = 0;
            int32_t gpsTime = -1;
            int32_t rgb = -1;
            bool extended = false; // formats 6 to 10: a byte of 4-bit return numbers, the classification a byte of its own
        };

        constexpr size_t LAS_HEADER_14 = 375;
        constexpr size_t LAS_HEADER_10 = 227;
        constexpr size_t VLR_HEADER = 54;
        constexpr size_t EVLR_HEADER = 60;
        constexpr uint32_t WKT_RECORD = 2112;
        constexpr uint32_t LASZIP_RECORD = 22204;
        constexpr uint64_t BYTES_PER_READ = 16ull << 20;
        // A record's coordinates and the point count are trusted only this far: larger is a broken file.
        constexpr uint64_t MAX_POINTS = 0xFFFFFFFFull;
    }

    namespace
    {
        static_assert(std::endian::native == std::endian::little, "the LAS reader reads little-endian fields in place");

        uint16_t readU16(const uint8_t *bytes)
        {
            uint16_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        uint32_t readU32(const uint8_t *bytes)
        {
            uint32_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        int32_t readI32(const uint8_t *bytes)
        {
            int32_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        uint64_t readU64(const uint8_t *bytes)
        {
            uint64_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        double readF64(const uint8_t *bytes)
        {
            double value = 0.0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        glm::dvec3 readDvec3(const uint8_t *bytes, size_t stride)
        {
            return {readF64(bytes), readF64(bytes + stride), readF64(bytes + 2 * stride)};
        }

        bool getPointLayout(uint32_t format, PointLayout &layout)
        {
            // Sizes of formats 0 to 10 (LAS 1.4 R15, section 2.6).
            constexpr uint32_t SIZES[] = {20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};
            constexpr int32_t GPS[] = {-1, 20, -1, 20, 20, 20, 22, 22, 22, 22, 22};
            constexpr int32_t RGB[] = {-1, -1, 20, 28, -1, 28, -1, 30, 30, -1, 30};
            if (format > 10)
                return false;
            layout = {.size = SIZES[format], .gpsTime = GPS[format], .rgb = RGB[format], .extended = format >= 6};
            return true;
        }

        bool parseHeader(const uint8_t *bytes, size_t size, LasHeader &header, std::string &error)
        {
            if (size < LAS_HEADER_10 || std::memcmp(bytes, "LASF", 4) != 0)
            {
                error = "it is not a LAS file (no LASF signature)";
                return false;
            }
            if (bytes[24] != 1 || bytes[25] > 4)
            {
                error = "LAS " + std::to_string(bytes[24]) + "." + std::to_string(bytes[25]) + " is not read (1.0 to 1.4 are)";
                return false;
            }
            header.versionMinor = bytes[25];
            header.headerSize = readU16(bytes + 94);
            header.pointOffset = readU32(bytes + 96);
            header.vlrCount = readU32(bytes + 100);
            // LASzip marks a compressed file's format with bit 7 (and bit 6 in early versions).
            header.compressed = (bytes[104] & 0xC0) != 0;
            header.format = bytes[104] & 0x3F;
            header.recordLength = readU16(bytes + 105);
            header.pointCount = readU32(bytes + 107);
            header.scale = readDvec3(bytes + 131, 8);
            header.offset = readDvec3(bytes + 155, 8);
            header.boundsMax = readDvec3(bytes + 179, 16);
            header.boundsMin = readDvec3(bytes + 187, 16);
            if (header.versionMinor >= 4 && header.headerSize >= LAS_HEADER_14 && size >= LAS_HEADER_14)
            {
                header.evlrStart = readU64(bytes + 235);
                header.evlrCount = readU32(bytes + 243);
                // The 64-bit count, which the legacy one (0 for formats 6 to 10) does not replace.
                const uint64_t count = readU64(bytes + 247);
                header.pointCount = header.pointCount != 0 && count == 0 ? header.pointCount : count;
            }
            PointLayout layout;
            if (!getPointLayout(header.format, layout))
            {
                error = "point format " + std::to_string(header.format) + " is unknown (0 to 10 are)";
                return false;
            }
            if (header.recordLength < layout.size || header.headerSize < LAS_HEADER_10 || header.pointOffset < header.headerSize || header.pointCount > MAX_POINTS)
            {
                error = "its header is broken (record length " + std::to_string(header.recordLength) + " for format " + std::to_string(header.format) + ", point offset " +
                        std::to_string(header.pointOffset) + ", " + std::to_string(header.pointCount) + " points)";
                return false;
            }
            if (!(header.scale.x != 0.0 && header.scale.y != 0.0 && header.scale.z != 0.0))
            {
                error = "its scale has a zero";
                return false;
            }
            return true;
        }

        // A record's user id, NUL-padded to 16 bytes.
        bool isUser(const uint8_t *user, const char *name)
        {
            const size_t length = std::strlen(name);
            return std::memcmp(user, name, length) == 0 && (length == 16 || user[length] == 0);
        }

        // The WKT and LASzip records of the variable length records, and those of the extended ones (LAS 1.4).
        void readRecords(std::ifstream &file, const LasHeader &header, PointCloudData &cloud, bool &laszipRecord)
        {
            std::vector<uint8_t> block(header.pointOffset - header.headerSize);
            file.seekg(header.headerSize);
            if (!block.empty() && !file.read(reinterpret_cast<char *>(block.data()), static_cast<std::streamsize>(block.size())))
                return;
            size_t at = 0;
            for (uint32_t i = 0; i < header.vlrCount && at + VLR_HEADER <= block.size(); ++i)
            {
                const uint8_t *record = block.data() + at;
                const uint32_t id = readU16(record + 18);
                const size_t length = readU16(record + 20);
                if (at + VLR_HEADER + length > block.size())
                    break;
                if (isUser(record + 2, "LASF_Projection") && id == WKT_RECORD)
                {
                    const uint8_t *text = record + VLR_HEADER;
                    cloud.coordinateSystem.assign(reinterpret_cast<const char *>(text), static_cast<size_t>(std::find(text, text + length, 0) - text));
                }
                laszipRecord = laszipRecord || (isUser(record + 2, "laszip encoded") && id == LASZIP_RECORD);
                at += VLR_HEADER + length;
            }
            uint64_t evlr = header.evlrStart;
            for (uint32_t i = 0; i < header.evlrCount && evlr > 0; ++i)
            {
                uint8_t record[EVLR_HEADER] = {};
                file.clear();
                file.seekg(static_cast<std::streamoff>(evlr));
                if (!file.read(reinterpret_cast<char *>(record), EVLR_HEADER))
                    break;
                const uint64_t length = readU64(record + 20);
                if (isUser(record + 2, "LASF_Projection") && readU16(record + 18) == WKT_RECORD && length < (64u << 20))
                {
                    std::string text(static_cast<size_t>(length), '\0');
                    if (file.read(text.data(), static_cast<std::streamsize>(length)))
                        cloud.coordinateSystem.assign(text.c_str());
                }
                evlr += EVLR_HEADER + length;
            }
            file.clear();
        }

        float getScalar(PointCloudScalar scalar, uint16_t intensity, uint32_t classification, double elevation, double gpsTime, uint32_t returnNumber)
        {
            switch (scalar)
            {
            case PointCloudScalar::INTENSITY:
                return static_cast<float>(intensity);
            case PointCloudScalar::CLASSIFICATION:
                return static_cast<float>(classification);
            case PointCloudScalar::ELEVATION:
                return static_cast<float>(elevation);
            case PointCloudScalar::GPS_TIME:
                return static_cast<float>(gpsTime);
            case PointCloudScalar::RETURN_NUMBER:
                return static_cast<float>(returnNumber);
            case PointCloudScalar::NONE:
                break;
            }
            return 0.0f;
        }

        // A point's fields into the cloud; rgb16 keeps the 16-bit colors until the file says 8 or 16 bits.
        void addPoint(PointCloudData &cloud, std::vector<uint16_t> &rgb16, const PointCloudLoadOptions &options, const glm::dvec3 &stored, uint16_t intensity, uint32_t classification,
                      double gpsTime, uint32_t returnNumber, const uint16_t *rgb)
        {
            const glm::dvec3 position = options.zUpToYUp ? glm::dvec3(stored.x, stored.z, -stored.y) : stored;
            cloud.positions.push_back(position);
            if (options.scalar != PointCloudScalar::NONE)
                cloud.scalars.push_back(getScalar(options.scalar, intensity, classification, options.zUpToYUp ? position.y : position.z, gpsTime, returnNumber));
            if (rgb != nullptr)
                rgb16.insert(rgb16.end(), rgb, rgb + 3);
        }

        // LAS colors are 16 bits a channel, but many files keep 8-bit values in them: those are taken as they are.
        void packColors(PointCloudData &cloud, const std::vector<uint16_t> &rgb16)
        {
            if (rgb16.empty())
                return;
            const bool wide = *std::max_element(rgb16.begin(), rgb16.end()) > 255;
            const uint32_t shift = wide ? 8 : 0;
            cloud.colors.resize(rgb16.size() / 3);
            for (size_t i = 0; i < cloud.colors.size(); ++i)
                cloud.colors[i] = static_cast<uint32_t>(rgb16[3 * i] >> shift) | static_cast<uint32_t>(rgb16[3 * i + 1] >> shift) << 8 | static_cast<uint32_t>(rgb16[3 * i + 2] >> shift) << 16 | 0xFF000000u;
        }

        bool readLasPoints(std::ifstream &file, const LasHeader &header, const PointLayout &layout, const PointCloudLoadOptions &options, PointCloudData &cloud, std::vector<uint16_t> &rgb16,
                           std::string &error)
        {
            file.seekg(header.pointOffset);
            const auto perRead = static_cast<uint32_t>(std::max<uint64_t>(1, BYTES_PER_READ / header.recordLength));
            std::vector<uint8_t> block(static_cast<size_t>(perRead) * header.recordLength);
            for (uint64_t done = 0; done < header.pointCount;)
            {
                const auto records = static_cast<uint32_t>(std::min<uint64_t>(perRead, header.pointCount - done));
                if (!file.read(reinterpret_cast<char *>(block.data()), static_cast<std::streamsize>(records) * header.recordLength))
                {
                    error = "it ends after " + std::to_string(done) + " of its " + std::to_string(header.pointCount) + " points";
                    return false;
                }
                for (uint32_t r = 0; r < records; ++r)
                {
                    const uint8_t *record = block.data() + static_cast<size_t>(r) * header.recordLength;
                    const glm::dvec3 stored = glm::dvec3(readI32(record), readI32(record + 4), readI32(record + 8)) * header.scale + header.offset;
                    const uint32_t returnNumber = layout.extended ? record[14] & 0x0Fu : record[14] & 0x07u;
                    const uint32_t classification = layout.extended ? record[16] : record[15] & 0x1Fu;
                    const double gpsTime = layout.gpsTime >= 0 ? readF64(record + layout.gpsTime) : 0.0;
                    uint16_t rgb[3] = {};
                    if (layout.rgb >= 0)
                    {
                        for (uint32_t c = 0; c < 3; ++c)
                            rgb[c] = readU16(record + layout.rgb + static_cast<size_t>(2 * c));
                    }
                    addPoint(cloud, rgb16, options, stored, readU16(record + 12), classification, gpsTime, returnNumber, layout.rgb >= 0 ? rgb : nullptr);
                }
                done += records;
            }
            return true;
        }

        bool readLazPoints(const std::string &path, const LasHeader &header, const PointLayout &layout, const PointCloudLoadOptions &options, PointCloudData &cloud, std::vector<uint16_t> &rgb16,
                           std::string &error)
        {
            std::ifstream stream(pathFromUtf8(path), std::ios::binary);
            laszip_POINTER reader = nullptr;
            if (!stream || laszip_create(&reader) != 0)
            {
                error = "LASzip could not open it";
                return false;
            }
            laszip_BOOL compressed = 0;
            bool read = laszip_open_reader_stream(reader, stream, &compressed) == 0;
            laszip_point *point = nullptr;
            read = read && laszip_get_point_pointer(reader, &point) == 0;
            for (uint64_t i = 0; read && i < header.pointCount; ++i)
            {
                if (laszip_read_point(reader) != 0)
                {
                    read = false;
                    break;
                }
                const glm::dvec3 stored = glm::dvec3(point->X, point->Y, point->Z) * header.scale + header.offset;
                const uint32_t classification = layout.extended ? point->extended_classification : point->classification;
                const uint32_t returnNumber = layout.extended ? point->extended_return_number : point->return_number;
                addPoint(cloud, rgb16, options, stored, point->intensity, classification, point->gps_time, returnNumber, layout.rgb >= 0 ? point->rgb : nullptr);
            }
            if (!read)
            {
                laszip_CHAR *message = nullptr;
                laszip_get_error(reader, &message);
                error = "LASzip: " + std::string(message != nullptr ? message : "no message") + " (after " + std::to_string(cloud.positions.size()) + " points)";
            }
            laszip_close_reader(reader);
            laszip_destroy(reader);
            return read;
        }
    }

    bool AssetLoader::loadPointCloud(const std::string &path, PointCloudData &cloud, const PointCloudLoadOptions &options)
    {
        ZoneScoped;
        cloud = {};
        const std::string resolved = resolveAssetPath(path);
        std::ifstream file(pathFromUtf8(resolved), std::ios::binary);
        uint8_t bytes[LAS_HEADER_14] = {};
        file.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
        const auto size = static_cast<size_t>(file.gcount());
        file.clear();
        LasHeader header;
        std::string error = file ? "" : "it cannot be read";
        std::error_code sizeError;
        const uint64_t fileSize = std::filesystem::file_size(pathFromUtf8(resolved), sizeError);
        if (error.empty() && !parseHeader(bytes, size, header, error))
            file.close();
        // A broken header must not make the reader allocate what the file cannot hold: its records and, uncompressed,
        // its points must fit in it.
        const bool lazByHeader = header.compressed;
        if (error.empty() && (sizeError || header.pointOffset > fileSize ||
                              (!lazByHeader && header.pointOffset + header.pointCount * static_cast<uint64_t>(header.recordLength) > fileSize)))
            error = "it ends before the " + std::to_string(header.pointCount) + " points its header promises";
        PointLayout layout;
        bool laszipRecord = false;
        if (error.empty())
        {
            getPointLayout(header.format, layout);
            readRecords(file, header, cloud, laszipRecord);
            cloud.pointFormat = header.format;
            cloud.versionMinor = header.versionMinor;
            cloud.compressed = header.compressed || laszipRecord;
            const glm::dvec3 low = options.zUpToYUp ? glm::dvec3(header.boundsMin.x, header.boundsMin.z, -header.boundsMax.y) : header.boundsMin;
            const glm::dvec3 high = options.zUpToYUp ? glm::dvec3(header.boundsMax.x, header.boundsMax.z, -header.boundsMin.y) : header.boundsMax;
            cloud.boundsMin = low;
            cloud.boundsMax = high;
            const auto count = static_cast<size_t>(std::min<uint64_t>(header.pointCount, fileSize));
            cloud.positions.reserve(count);
            if (options.scalar != PointCloudScalar::NONE)
                cloud.scalars.reserve(count);
        }
        std::vector<uint16_t> rgb16;
        if (layout.rgb >= 0)
            rgb16.reserve(static_cast<size_t>(std::min<uint64_t>(header.pointCount, fileSize)) * 3);
        if (error.empty())
        {
            const bool read = cloud.compressed ? readLazPoints(resolved, header, layout, options, cloud, rgb16, error) : readLasPoints(file, header, layout, options, cloud, rgb16, error);
            if (read)
                packColors(cloud, rgb16);
        }
        if (!error.empty())
        {
            spdlog::error("loadPointCloud: '{}' was not read: {}.", resolved, error);
            cloud = {};
            return false;
        }
        return true;
    }
}
