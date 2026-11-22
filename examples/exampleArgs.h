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
//                      engine's step for the cameras, the UI and the stats (EngineConfig::fixedDeltaTime)
//   --wait-for-loads   Engine::waitForLoads before the first frame, so frame N shows the same scene on every run (C16)
//   --headless         no window and no input (PlatformType::HEADLESS): render --frames frames, write the last to --out, exit
//   --frames N         frames a headless run renders (60)
//   --out <file.png>   where a headless run writes its last frame (frame.png in the working directory)
struct ExampleArgs
{
    bool                    fixedStep       = false;
    bool                    waitForLoads    = false;
    bool                    headless        = false;
    uint32_t                frames          = 60;
    const char*             out             = "frame.png";
    uint32_t                frame           = 0;        // frames exampleRunning let start
    rasm::ReadbackHandle    readback;                   // the copy of the last frame
};

inline ExampleArgs parseExampleArgs(int argc, char** argv)
{
    ExampleArgs args;
    for (int i = 1; i < argc; ++i)
    {
        args.fixedStep      = args.fixedStep    || std::strcmp(argv[i], "--fixed-step") == 0;
        args.waitForLoads   = args.waitForLoads || std::strcmp(argv[i], "--wait-for-loads") == 0;
        args.headless       = args.headless     || std::strcmp(argv[i], "--headless") == 0;
        if (i + 1 < argc && std::strcmp(argv[i], "--frames") == 0)
            args.frames     = static_cast<uint32_t>(std::max(1, std::atoi(argv[++i])));
        else if (i + 1 < argc && std::strcmp(argv[i], "--out") == 0)
            args.out        = argv[++i];
    }
    return args;
}

inline void applyExampleArgs(const ExampleArgs& args, rasm::EngineConfig& config)
{
    if (args.headless)
        config.platform         = rasm::PlatformType::HEADLESS;
    if (args.fixedStep)
        config.fixedDeltaTime   = 1.0f / 60.0f;
}

// The frame loop's condition, in place of Engine::running. Headless, it asks for a copy of the last frame before that
// frame starts (readFrame copies the next frame), and once it is rendered writes it to --out and ends the loop.
inline bool exampleRunning(rasm::Engine& engine, ExampleArgs& args)
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
        if (!engine.takeReadback(args.readback, image) || !rasm::writePng(args.out, image))
            std::fprintf(stderr, "Could not write frame %u to %s.\n", args.frames, args.out);
        else
            std::printf("Wrote frame %u to %s.\n", args.frames, args.out);
        return false;
    }
    if (++args.frame == args.frames)
        args.readback       = engine.readFrame();
    return true;
}
// clang-format on
