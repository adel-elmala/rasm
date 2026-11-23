// clang-format off
#pragma once

#include "rasm/rasm.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// --bench in 05_city and 18_meshlets: the GPU's time per pass and what culling kept, averaged over the frames after
// BENCH_WARMUP_FRAMES, printed when the run ends. Run it headless in Release (--headless --frames 600 --bench): the
// engine then runs without validation and without the UI, whose passes would change the timings.
constexpr uint32_t BENCH_WARMUP_FRAMES = 120;

struct ExampleBench
{
    bool                            on                  = false;
    uint32_t                        frame               = 0;
    uint32_t                        measured            = 0;
    std::vector<std::string>        names;              // the passes, in the order of their first frame
    std::map<std::string, double>   sums;
    double                          gpuMs               = 0.0;
    double                          triangles           = 0.0;
    double                          draws               = 0.0;
    double                          meshlets            = 0.0;
    double                          meshletsCulled      = 0.0;
    double                          meshletDrawsWhole   = 0.0;
};

inline bool hasBenchFlag(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--bench") == 0)
            return true;
    }
    return false;
}

// --meshlets off|vp: EngineConfig::meshletPath.
inline void applyMeshletFlag(int argc, char** argv, rasm::EngineConfig& config)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--meshlets") == 0)
            config.meshletPath = std::strcmp(argv[i + 1], "off") == 0 ? rasm::MeshletPath::OFF : rasm::MeshletPath::VERTEX_PULLING;
    }
}

// Every frame: frames past the warm-up add their stats. Several passes of one name (a view's two culling phases) add up.
inline void addBenchFrame(ExampleBench& bench, const rasm::FrameStats& stats)
{
    if (!bench.on || bench.frame++ < BENCH_WARMUP_FRAMES)
        return;
    ++bench.measured;
    for (uint32_t i = 0; i < stats.gpuTimerCount; ++i)
    {
        const std::string& name = stats.gpuTimers[i].name;
        if (bench.sums.find(name) == bench.sums.end())
        {
            bench.names.push_back(name);
            bench.sums[name] = 0.0;
        }
        bench.sums[name] += stats.gpuTimers[i].milliseconds;
    }
    bench.gpuMs             += stats.gpuFrameMs;
    bench.triangles         += static_cast<double>(stats.trianglesVisible);
    bench.draws             += stats.drawsVisible;
    bench.meshlets          += stats.meshletsVisible;
    bench.meshletsCulled    += stats.meshletsCulled;
    bench.meshletDrawsWhole += stats.meshletDrawsWhole;
}

inline void printBench(const ExampleBench& bench, const rasm::EngineConfig& config)
{
    if (!bench.on || bench.measured == 0)
        return;
    const double n = bench.measured;
    double culling = 0.0;
    double meshletCulling = 0.0;
    for (const std::string& name : bench.names)
    {
        if (name.find("Meshlet") != std::string::npos)
            meshletCulling += bench.sums.at(name);
        else if (name.find("Cull") != std::string::npos)
            culling += bench.sums.at(name);
    }
    std::printf("bench, %u frames at %dx%d, meshlets %s:\n", bench.measured, config.windowWidth, config.windowHeight,
                config.meshletPath == rasm::MeshletPath::OFF ? "off" : "vertex pulling");
    for (const std::string& name : bench.names)
        std::printf("  %-22s %8.3f ms\n", name.c_str(), bench.sums.at(name) / n);
    std::printf("  %-22s %8.3f ms\n", "GPU frame", bench.gpuMs / n);
    std::printf("  %-22s %8.3f ms (of it meshlets %.3f ms)\n", "culling", culling / n, meshletCulling / n);
    std::printf("  %-22s %8.2f M\n", "triangles drawn", bench.triangles / n / 1e6);
    std::printf("  %-22s %8.0f\n", "draws", bench.draws / n);
    std::printf("  %-22s %8.0f drawn, %.0f culled, %.0f draws over budget\n", "meshlets", bench.meshlets / n, bench.meshletsCulled / n, bench.meshletDrawsWhole / n);
    std::fflush(stdout);
}
// clang-format on
