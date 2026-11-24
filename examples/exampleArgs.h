// clang-format off
#pragma once

#include "rasm/rasm.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The flags that control a run of any example, which verify-change passes to all of them. An example's own feature
// flags (--aa, --bench, ...) stay in its main.cpp.
//   --fixed-step       an example that animates by the clock advances a fixed 1/60 s per frame instead, and so does the
//                      renderer's step for the cameras, the UI and the stats (RendererConfig::fixedDeltaTime)
//   --wait-for-loads   Renderer::waitForLoads before the first frame, so frame N shows the same scene on every run (C16)
//   --headless         no window and no input (PlatformType::HEADLESS, or a Renderer without a nativeWindow): render
//                      --frames frames, write the last to --out, exit
//   --frames N         frames a headless run renders (60)
//   --out <file.png>   where a headless run writes its last frame (frame.png in the working directory)
//   --no-cache         no disk caches (EngineConfig::cacheDir = ""): every shader is compiled and every pipeline built
//   --transfer-queue on|off   EngineConfig::transferQueue: loads copy on the GPU's copy queue, or on the graphics queue
// main returns exampleExit(args): a headless run that did not write its frame fails.
struct ExampleArgs
{
    bool                    fixedStep       = false;
    bool                    waitForLoads    = false;
    bool                    headless        = false;
    bool                    noCache         = false;
    int                     transferQueue   = -1;       // 1 on, 0 off, -1 the example's own setting
    uint32_t                frames          = 60;
    const char*             out             = "frame.png";
    uint32_t                frame           = 0;        // frames exampleRunning let start
    rasm::ReadbackHandle    readback;                   // the copy of the last frame
    bool                    written         = false;    // the last frame is in --out
};

inline ExampleArgs parseExampleArgs(int argc, char** argv)
{
    ExampleArgs args;
    for (int i = 1; i < argc; ++i)
    {
        args.fixedStep      = args.fixedStep    || std::strcmp(argv[i], "--fixed-step") == 0;
        args.waitForLoads   = args.waitForLoads || std::strcmp(argv[i], "--wait-for-loads") == 0;
        args.headless       = args.headless     || std::strcmp(argv[i], "--headless") == 0;
        args.noCache        = args.noCache      || std::strcmp(argv[i], "--no-cache") == 0;
        if (i + 1 < argc && std::strcmp(argv[i], "--frames") == 0)
            args.frames     = static_cast<uint32_t>(std::max(1, std::atoi(argv[++i])));
        else if (i + 1 < argc && std::strcmp(argv[i], "--out") == 0)
            args.out        = argv[++i];
        else if (i + 1 < argc && std::strcmp(argv[i], "--transfer-queue") == 0)
            args.transferQueue = std::strcmp(argv[++i], "on") == 0 ? 1 : 0;
    }
    return args;
}

// An Engine's config. A Renderer is headless when its config names no window, which the example decides.
inline void applyExampleArgs(const ExampleArgs& args, rasm::EngineConfig& config)
{
    if (args.headless)
        config.platform         = rasm::PlatformType::HEADLESS;
    if (args.fixedStep)
        config.fixedDeltaTime   = 1.0f / 60.0f;
    if (args.noCache)
        config.cacheDir         = "";
    if (args.transferQueue >= 0)
        config.transferQueue    = args.transferQueue == 1;
}

inline void applyExampleArgs(const ExampleArgs& args, rasm::RendererConfig& config)
{
    if (args.fixedStep)
        config.fixedDeltaTime   = 1.0f / 60.0f;
    if (args.noCache)
        config.cacheDir         = "";
    if (args.transferQueue >= 0)
        config.transferQueue    = args.transferQueue == 1;
}

// The frame loop's condition, in place of Renderer::running, for an Engine or a Renderer. Headless, it asks for a copy
// of the last frame before that frame starts (readFrame copies the next frame), and once it is rendered writes it to
// --out and ends the loop.
inline bool exampleRunning(rasm::Renderer& engine, ExampleArgs& args)
{
    if (!engine.running())
        return false;
    if (!args.headless)
        return true;
    if (args.frame > args.frames)
        return false;
    if (args.frame == args.frames)
    {
        ++args.frame;
        rasm::ReadbackImage image;
        args.written        = engine.takeReadback(args.readback, image) && rasm::writePng(args.out, image);
        if (args.written)
            std::printf("Wrote frame %u to %s.\n", args.frames, args.out);
        return false;
    }
    if (++args.frame == args.frames)
        args.readback       = engine.readFrame();
    return true;
}

// The exit status: 1 for a headless run whose frame is not in --out (the copy or the write failed, or the loop ended
// before frame N).
inline int exampleExit(const ExampleArgs& args)
{
    if (!args.headless || args.written)
        return 0;
    std::fprintf(stderr, "error: frame %u was not written to %s.\n", args.frames, args.out);
    return 1;
}
// clang-format on
