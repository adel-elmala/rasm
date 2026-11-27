// clang-format off
#pragma once

#include "rasm/export.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rasm
{

    // Writes an image file row by row on a thread of its own, so a huge image is never whole in memory: a PNG (8 or 16
    // bits, RGBA, sRGB, straight alpha) through one deflate stream, or an EXR (scanlines, A, B, G, R in half or float,
    // ZIP blocks of 16 lines, premultiplied). The rows come as the Export pass writes them (EXPORT_OUTPUT_*): RGBA8,
    // RGBA16, RGBA half or RGBA float per pixel, little-endian, rows top to bottom.
    class ImageWriter
    {
    public:
                                    ImageWriter() = default;
                                    ~ImageWriter();
                                    ImageWriter(const ImageWriter&) = delete;
        ImageWriter&                operator=(const ImageWriter&) = delete;
        // Opens the file at a UTF-8 path, writes its header and starts the thread. False when it can't be opened.
        bool                        start(const std::string& path, ExportFormat format, uint32_t width, uint32_t height);
        // The next rows, which the thread takes.
        void                        push(std::vector<uint8_t>&& rows, uint32_t rowCount);
        // Pushed rows the thread has not written yet, in pushes.
        uint32_t                    queued();
        // No more rows: the thread writes what is left and closes the file.
        void                        finish();
        // The file is closed: written, or failed. Destroyed before that, it stops and removes the file.
        bool                        finished();
        bool                        failed();

    private:
        struct Rows
        {
            std::vector<uint8_t>    bytes;
            uint32_t                count       = 0;
        };

        void                        run();
        bool                        writeRows(const uint8_t* rows, uint32_t count);
        bool                        writePngRows(const uint8_t* rows, uint32_t count);
        bool                        writeExrRows(const uint8_t* rows, uint32_t count);
        bool                        writeExrBlock();
        bool                        close();

        std::ofstream               file;
        std::string                 path;
        ExportFormat                format      = ExportFormat::PNG8;
        uint32_t                    width       = 0;
        uint32_t                    height      = 0;
        uint32_t                    rowsWritten = 0;
        void*                       stream      = nullptr;          // the PNG's z_stream
        std::vector<uint8_t>        filtered;                       // a PNG row: its filter byte, then the filtered bytes
        std::vector<uint8_t>        previous;                       // the row above, unfiltered
        std::vector<uint8_t>        compressed;                     // deflate's output, or an EXR block's
        std::vector<uint8_t>        block;                          // an EXR block's lines, planar
        std::vector<uint8_t>        scratch;
        std::vector<uint64_t>       offsets;                        // the EXR blocks' file offsets
        uint32_t                    blockLines  = 0;
        std::streamoff              tableOffset = 0;
        std::thread                 thread;
        std::mutex                  mutex;
        std::condition_variable     wake;
        std::deque<Rows>            pending;
        bool                        closing     = false;
        bool                        stopping    = false;            // destroyed before it finished: drop the rows, remove the file
        bool                        done        = false;
        bool                        error       = false;
    };

}
// clang-format on
