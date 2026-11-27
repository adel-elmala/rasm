#include "renderer/imageWriter.h"
#include "platform/paths.h"
#include "renderer/export.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace rasm
{
    namespace
    {
        // An EXR ZIP block holds 16 scanlines.
        constexpr uint32_t EXR_BLOCK_LINES = 16;
        // deflate's level: 1 to 9. Level 3 deflates a filtered photo about as small as 6 does, at about twice the speed.
        constexpr int DEFLATE_LEVEL = 3;
        constexpr size_t DEFLATE_CHUNK = size_t(1) << 18;
        constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        constexpr uint8_t PNG_FILTER_PAETH = 4;
        constexpr int32_t EXR_HALF = 1;
        constexpr int32_t EXR_FLOAT = 2;
        constexpr uint8_t EXR_ZIP_COMPRESSION = 3;
    }

    namespace
    {
        void putBigEndian(uint8_t *bytes, uint32_t value)
        {
            bytes[0] = static_cast<uint8_t>(value >> 24);
            bytes[1] = static_cast<uint8_t>(value >> 16);
            bytes[2] = static_cast<uint8_t>(value >> 8);
            bytes[3] = static_cast<uint8_t>(value);
        }

        void appendBytes(std::vector<uint8_t> &out, const void *data, size_t size)
        {
            const auto *bytes = static_cast<const uint8_t *>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        void appendInt(std::vector<uint8_t> &out, int32_t value)
        {
            appendBytes(out, &value, sizeof(value));
        }

        void appendFloat(std::vector<uint8_t> &out, float value)
        {
            appendBytes(out, &value, sizeof(value));
        }

        void appendString(std::vector<uint8_t> &out, const char *text)
        {
            appendBytes(out, text, std::strlen(text) + 1);
        }

        // An EXR header attribute: its name, its type's name, its size and its value.
        void appendAttribute(std::vector<uint8_t> &out, const char *name, const char *type, const std::vector<uint8_t> &value)
        {
            appendString(out, name);
            appendString(out, type);
            appendInt(out, static_cast<int32_t>(value.size()));
            appendBytes(out, value.data(), value.size());
        }

        // A PNG chunk: its length, type, data and the CRC of the type and the data.
        bool writeChunk(std::ofstream &file, const char *type, const uint8_t *data, uint32_t size)
        {
            uint8_t head[8];
            putBigEndian(head, size);
            std::memcpy(head + 4, type, 4);
            uLong crc = crc32(0L, Z_NULL, 0);
            crc = crc32(crc, head + 4, 4);
            if (size > 0)
                crc = crc32(crc, data, size);
            uint8_t tail[4];
            putBigEndian(tail, static_cast<uint32_t>(crc));
            file.write(reinterpret_cast<const char *>(head), sizeof(head));
            if (size > 0)
                file.write(reinterpret_cast<const char *>(data), size);
            file.write(reinterpret_cast<const char *>(tail), sizeof(tail));
            return file.good();
        }

        uint8_t paeth(uint8_t left, uint8_t up, uint8_t upLeft)
        {
            const int p = static_cast<int>(left) + static_cast<int>(up) - static_cast<int>(upLeft);
            const int pa = std::abs(p - static_cast<int>(left));
            const int pb = std::abs(p - static_cast<int>(up));
            const int pc = std::abs(p - static_cast<int>(upLeft));
            if (pa <= pb && pa <= pc)
                return left;
            return pb <= pc ? up : upLeft;
        }

        bool isExr(ExportFormat format)
        {
            return format == ExportFormat::EXR_HALF || format == ExportFormat::EXR_FLOAT;
        }
    }

    ImageWriter::~ImageWriter()
    {
        bool remove = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            remove = !done || error;
            stopping = !done;
            closing = true;
        }
        wake.notify_all();
        if (thread.joinable())
            thread.join();
        if (stream != nullptr)
        {
            deflateEnd(static_cast<z_stream *>(stream));
            delete static_cast<z_stream *>(stream);
        }
        if (file.is_open())
            file.close();
        if (remove && !path.empty())
        {
            std::error_code errorCode;
            std::filesystem::remove(pathFromUtf8(path), errorCode);
        }
    }

    bool ImageWriter::start(const std::string &filePath, ExportFormat fileFormat, uint32_t imageWidth, uint32_t imageHeight)
    {
        path = filePath;
        format = fileFormat;
        width = imageWidth;
        height = imageHeight;
        file.open(pathFromUtf8(path), std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            spdlog::error("Export: could not open '{}' for writing.", path);
            return false;
        }

        if (!isExr(format))
        {
            // IHDR: the size, 8 or 16 bits a sample, RGBA (color type 6), deflate, adaptive filters, no interlace.
            uint8_t header[13] = {};
            putBigEndian(header, width);
            putBigEndian(header + 4, height);
            header[8] = format == ExportFormat::PNG16 ? 16 : 8;
            header[9] = 6;
            file.write(reinterpret_cast<const char *>(PNG_SIGNATURE), sizeof(PNG_SIGNATURE));
            if (!writeChunk(file, "IHDR", header, sizeof(header)))
                return false;
            auto *zstream = new z_stream{};
            stream = zstream;
            if (deflateInit(zstream, DEFLATE_LEVEL) != Z_OK)
            {
                spdlog::error("Export: deflate could not start.");
                return false;
            }
            const size_t rowBytes = static_cast<size_t>(width) * getExportPixelBytes(format);
            filtered.resize(rowBytes + 1);
            previous.assign(rowBytes, 0);
            compressed.resize(DEFLATE_CHUNK);
        }
        else
        {
            // A single-part scanline file: the channels in name order, ZIP blocks of 16 lines, then the blocks' offsets,
            // which close writes once the blocks' sizes are known.
            std::vector<uint8_t> header = {0x76, 0x2F, 0x31, 0x01, 2, 0, 0, 0};
            std::vector<uint8_t> channels;
            const int32_t type = format == ExportFormat::EXR_HALF ? EXR_HALF : EXR_FLOAT;
            for (const char *name : {"A", "B", "G", "R"})
            {
                appendString(channels, name);
                appendInt(channels, type);
                const uint8_t linearAndReserved[4] = {};
                appendBytes(channels, linearAndReserved, sizeof(linearAndReserved));
                appendInt(channels, 1);
                appendInt(channels, 1);
            }
            channels.push_back(0);
            appendAttribute(header, "channels", "chlist", channels);
            appendAttribute(header, "compression", "compression", {EXR_ZIP_COMPRESSION});
            std::vector<uint8_t> window;
            appendInt(window, 0);
            appendInt(window, 0);
            appendInt(window, static_cast<int32_t>(width) - 1);
            appendInt(window, static_cast<int32_t>(height) - 1);
            appendAttribute(header, "dataWindow", "box2i", window);
            appendAttribute(header, "displayWindow", "box2i", window);
            appendAttribute(header, "lineOrder", "lineOrder", {0});
            std::vector<uint8_t> value;
            appendFloat(value, 1.0f);
            appendAttribute(header, "pixelAspectRatio", "float", value);
            value.clear();
            appendFloat(value, 0.0f);
            appendFloat(value, 0.0f);
            appendAttribute(header, "screenWindowCenter", "v2f", value);
            value.clear();
            appendFloat(value, 1.0f);
            appendAttribute(header, "screenWindowWidth", "float", value);
            header.push_back(0);
            file.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));
            tableOffset = file.tellp();
            offsets.assign((height + EXR_BLOCK_LINES - 1) / EXR_BLOCK_LINES, 0);
            file.write(reinterpret_cast<const char *>(offsets.data()), static_cast<std::streamsize>(offsets.size() * sizeof(uint64_t)));
            block.resize(static_cast<size_t>(width) * getExportPixelBytes(format) * EXR_BLOCK_LINES);
        }
        if (!file.good())
        {
            spdlog::error("Export: could not write the header of '{}'.", path);
            return false;
        }
        thread = std::thread([this]()
                             { run(); });
        return true;
    }

    void ImageWriter::push(std::vector<uint8_t> &&rows, uint32_t rowCount)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.push_back({.bytes = std::move(rows), .count = rowCount});
        }
        wake.notify_all();
    }

    uint32_t ImageWriter::queued()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<uint32_t>(pending.size());
    }

    void ImageWriter::finish()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
        }
        wake.notify_all();
    }

    bool ImageWriter::finished()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return done;
    }

    bool ImageWriter::failed()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return error;
    }

    // Takes the pushed rows one push at a time, outside the lock, and closes the file once finish was called and
    // nothing is left.
    void ImageWriter::run()
    {
        bool failure = false;
        while (true)
        {
            Rows rows;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this]()
                          { return !pending.empty() || closing; });
                if (stopping)
                    return;
                if (pending.empty())
                    break;
                rows = std::move(pending.front());
                pending.pop_front();
            }
            if (!failure && !writeRows(rows.bytes.data(), rows.count))
            {
                failure = true;
                spdlog::error("Export: writing '{}' failed.", path);
                std::lock_guard<std::mutex> lock(mutex);
                error = true;
            }
        }
        failure = failure || !close();
        std::lock_guard<std::mutex> lock(mutex);
        error = failure;
        done = true;
    }

    bool ImageWriter::writeRows(const uint8_t *rows, uint32_t count)
    {
        ZoneScoped;
        if (rowsWritten + count > height)
            return false;
        return isExr(format) ? writeExrRows(rows, count) : writePngRows(rows, count);
    }

    // Each row goes through the Paeth filter and into the one deflate stream; whatever deflate gives out is an IDAT chunk.
    bool ImageWriter::writePngRows(const uint8_t *rows, uint32_t count)
    {
        auto *zstream = static_cast<z_stream *>(stream);
        const uint32_t pixelBytes = getExportPixelBytes(format);
        const size_t rowBytes = static_cast<size_t>(width) * pixelBytes;
        scratch.resize(rowBytes);
        for (uint32_t r = 0; r < count; ++r)
        {
            const uint8_t *row = rows + r * rowBytes;
            // A 16-bit PNG's samples are big-endian.
            if (format == ExportFormat::PNG16)
            {
                for (size_t i = 0; i < rowBytes; i += 2)
                {
                    scratch[i] = row[i + 1];
                    scratch[i + 1] = row[i];
                }
                row = scratch.data();
            }
            filtered[0] = PNG_FILTER_PAETH;
            for (size_t i = 0; i < rowBytes; ++i)
            {
                const uint8_t left = i >= pixelBytes ? row[i - pixelBytes] : 0;
                const uint8_t upLeft = i >= pixelBytes ? previous[i - pixelBytes] : 0;
                filtered[i + 1] = static_cast<uint8_t>(row[i] - paeth(left, previous[i], upLeft));
            }
            std::memcpy(previous.data(), row, rowBytes);
            zstream->next_in = filtered.data();
            zstream->avail_in = static_cast<uInt>(filtered.size());
            while (zstream->avail_in > 0)
            {
                zstream->next_out = compressed.data();
                zstream->avail_out = static_cast<uInt>(compressed.size());
                if (deflate(zstream, Z_NO_FLUSH) == Z_STREAM_ERROR)
                    return false;
                const auto produced = static_cast<uint32_t>(compressed.size() - zstream->avail_out);
                if (produced > 0 && !writeChunk(file, "IDAT", compressed.data(), produced))
                    return false;
            }
        }
        rowsWritten += count;
        return true;
    }

    // Lines are made planar (A, B, G, R) into the block, which goes out every EXR_BLOCK_LINES lines.
    bool ImageWriter::writeExrRows(const uint8_t *rows, uint32_t count)
    {
        const uint32_t sampleBytes = format == ExportFormat::EXR_HALF ? 2 : 4;
        const size_t rowBytes = static_cast<size_t>(width) * 4 * sampleBytes;
        for (uint32_t r = 0; r < count; ++r)
        {
            const uint8_t *row = rows + r * rowBytes;
            uint8_t *line = block.data() + blockLines * rowBytes;
            // The pass writes R, G, B, A; the file's channels are A, B, G, R.
            for (uint32_t c = 0; c < 4; ++c)
            {
                const uint32_t source = 3 - c;
                uint8_t *plane = line + static_cast<size_t>(c) * width * sampleBytes;
                for (uint32_t x = 0; x < width; ++x)
                    std::memcpy(plane + static_cast<size_t>(x) * sampleBytes, row + (static_cast<size_t>(x) * 4 + source) * sampleBytes, sampleBytes);
            }
            ++blockLines;
            ++rowsWritten;
            if ((blockLines == EXR_BLOCK_LINES || rowsWritten == height) && !writeExrBlock())
                return false;
        }
        return true;
    }

    // ZIP: the bytes split into the even and the odd ones, each byte stored as its difference to the one before, then
    // deflate; a block that does not get smaller is stored as it is.
    bool ImageWriter::writeExrBlock()
    {
        const uint32_t sampleBytes = format == ExportFormat::EXR_HALF ? 2 : 4;
        const size_t rawSize = static_cast<size_t>(blockLines) * width * 4 * sampleBytes;
        scratch.resize(rawSize);
        size_t even = 0;
        size_t odd = (rawSize + 1) / 2;
        for (size_t i = 0; i < rawSize; ++i)
            scratch[(i & 1) == 0 ? even++ : odd++] = block[i];
        int previousByte = scratch[0];
        for (size_t i = 1; i < rawSize; ++i)
        {
            const int value = scratch[i];
            scratch[i] = static_cast<uint8_t>(value - previousByte + 128 + 256);
            previousByte = value;
        }
        uLongf compressedSize = compressBound(static_cast<uLong>(rawSize));
        compressed.resize(compressedSize);
        if (compress2(compressed.data(), &compressedSize, scratch.data(), static_cast<uLong>(rawSize), DEFLATE_LEVEL) != Z_OK)
            return false;
        const bool stored = compressedSize >= rawSize;
        const uint8_t *data = stored ? block.data() : compressed.data();
        const auto size = static_cast<int32_t>(stored ? rawSize : compressedSize);
        const auto firstLine = static_cast<int32_t>(rowsWritten - blockLines);
        offsets[firstLine / EXR_BLOCK_LINES] = static_cast<uint64_t>(file.tellp());
        file.write(reinterpret_cast<const char *>(&firstLine), sizeof(firstLine));
        file.write(reinterpret_cast<const char *>(&size), sizeof(size));
        file.write(reinterpret_cast<const char *>(data), size);
        blockLines = 0;
        return file.good();
    }

    bool ImageWriter::close()
    {
        if (rowsWritten != height)
            return false;
        if (!isExr(format))
        {
            auto *zstream = static_cast<z_stream *>(stream);
            int status = Z_OK;
            zstream->next_in = nullptr;
            zstream->avail_in = 0;
            while (status != Z_STREAM_END)
            {
                zstream->next_out = compressed.data();
                zstream->avail_out = static_cast<uInt>(compressed.size());
                status = deflate(zstream, Z_FINISH);
                if (status == Z_STREAM_ERROR)
                    return false;
                const auto produced = static_cast<uint32_t>(compressed.size() - zstream->avail_out);
                if (produced > 0 && !writeChunk(file, "IDAT", compressed.data(), produced))
                    return false;
            }
            if (!writeChunk(file, "IEND", nullptr, 0))
                return false;
        }
        else
        {
            file.seekp(tableOffset);
            file.write(reinterpret_cast<const char *>(offsets.data()), static_cast<std::streamsize>(offsets.size() * sizeof(uint64_t)));
        }
        file.close();
        return !file.fail();
    }
}
