// clang-format off
#pragma once

#include "rasm/rasm.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

// Helpers for the examples that export a still (Renderer::exportView): run one to its end while the application's views
// keep rendering, and compare two exports' images pixel by pixel.

// The differences of two RGBA8 images of one size: the largest channel difference and the pixels past a threshold.
struct ExportDiff
{
    uint32_t    maxDifference   = 0;
    uint64_t    over            = 0;        // pixels with a channel that differs by more than the threshold
    uint64_t    differing       = 0;        // pixels with any difference
    uint64_t    pixels          = 0;
};

// Renders frames until the export is DONE or FAILED, printing its progress every tenth; frame runs before each one
// (an application's own work: input, streaming). Returns false when it failed, and gives its image when it was kept in
// memory.
inline bool runExport(rasm::Renderer &engine, const rasm::View *views, uint32_t viewCount, rasm::ExportHandle exportHandle, const char *name, const std::function<void()> &frame,
                      rasm::ReadbackImage &image, double &seconds)
{
    const auto start = std::chrono::steady_clock::now();
    int lastTenth = -1;
    rasm::ExportProgress progress = engine.getExportProgress(exportHandle);
    while (engine.running() && progress.state == rasm::ExportState::RUNNING)
    {
        if (frame)
            frame();
        engine.render(views, viewCount);
        progress = engine.getExportProgress(exportHandle);
        const int tenth = static_cast<int>(progress.fraction * 10.0f);
        if (tenth != lastTenth)
        {
            std::printf("  %s: %3d %%, %u of %u tiles\n", name, tenth * 10, progress.tilesDone, progress.tileCount);
            lastTenth = tenth;
        }
    }
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (progress.state != rasm::ExportState::DONE)
    {
        std::printf("  %s: FAILED\n", name);
        return false;
    }
    return engine.takeExport(exportHandle, image);
}

// a and b must be RGBA8 images of one size; images of two sizes (or empty ones) give no pixels, which the callers count
// as a failure. diff, when not null, gets an image of the differences: gray where the
// pixels are equal, orange up to the threshold, red past it.
inline ExportDiff compareExports(const rasm::ReadbackImage &a, const rasm::ReadbackImage &b, uint32_t threshold, rasm::ReadbackImage *diff)
{
    ExportDiff result;
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size())
    {
        result.maxDifference = 255;
        return result;
    }
    result.pixels = static_cast<uint64_t>(a.width) * a.height;
    if (diff != nullptr)
    {
        *diff = {.width = a.width, .height = a.height, .format = rasm::ReadbackFormat::RGBA8_SRGB};
        diff->pixels.resize(a.pixels.size());
    }
    for (uint64_t p = 0; p < result.pixels; ++p)
    {
        uint32_t largest = 0;
        for (uint32_t c = 0; c < 4; ++c)
            largest = std::max(largest, static_cast<uint32_t>(std::abs(static_cast<int>(a.pixels[p * 4 + c]) - static_cast<int>(b.pixels[p * 4 + c]))));
        result.maxDifference = std::max(result.maxDifference, largest);
        result.differing += largest > 0 ? 1 : 0;
        result.over += largest > threshold ? 1 : 0;
        if (diff != nullptr)
        {
            const uint8_t gray = static_cast<uint8_t>(a.pixels[p * 4] / 4 + 40);
            const uint8_t same[4] = {gray, gray, gray, 255};
            const uint8_t small[4] = {255, 160, 0, 255};
            const uint8_t big[4] = {230, 0, 0, 255};
            const uint8_t *color = largest == 0 ? same : (largest <= threshold ? small : big);
            std::copy(color, color + 4, diff->pixels.begin() + static_cast<std::ptrdiff_t>(p * 4));
        }
    }
    return result;
}

inline void printExportDiff(const char *name, const ExportDiff &diff, uint32_t threshold)
{
    std::printf("  %s: largest difference %u, %llu of %llu pixels differ (%.4f %%), %llu by more than %u (%.4f %%)\n", name, diff.maxDifference, static_cast<unsigned long long>(diff.differing),
                static_cast<unsigned long long>(diff.pixels), 100.0 * static_cast<double>(diff.differing) / static_cast<double>(std::max<uint64_t>(diff.pixels, 1)),
                static_cast<unsigned long long>(diff.over), threshold, 100.0 * static_cast<double>(diff.over) / static_cast<double>(std::max<uint64_t>(diff.pixels, 1)));
}
// clang-format on
