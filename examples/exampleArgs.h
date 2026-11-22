// clang-format off
#pragma once

#include <cstring>

// The flags that control a run of any example, which verify-change passes to all of them. An example's own feature
// flags (--aa, --bench, ...) stay in its main.cpp.
//   --fixed-step       an example that animates by the clock advances a fixed 1/60 s per frame instead
//   --wait-for-loads   Engine::waitForLoads before the first frame, so frame N shows the same scene on every run (C16)
struct ExampleArgs
{
    bool            fixedStep       = false;
    bool            waitForLoads    = false;
};

inline ExampleArgs parseExampleArgs(int argc, char** argv)
{
    ExampleArgs args;
    for (int i = 1; i < argc; ++i)
    {
        args.fixedStep      = args.fixedStep    || std::strcmp(argv[i], "--fixed-step") == 0;
        args.waitForLoads   = args.waitForLoads || std::strcmp(argv[i], "--wait-for-loads") == 0;
    }
    return args;
}
// clang-format on
