---
name: tracy-profile
description: Profile RASM's CPU side with Tracy from the command line. Capture an example, then get zone stats, frame-time percentiles, slow-frame breakdowns and before/after comparisons as text. Use for performance questions (why is it slow, frame spikes, load time, did my change make it faster), before and after optimizations, or when adding Tracy zones.
---

# Tracy profile

RASM is built with the Tracy client (vcpkg port, **on-demand** mode): it records nothing until
a profiler connects, so `capture` starts `tracy-capture` first and then launches the app.
`scripts/tracy.py` wraps everything and prints text tables.

## Setup (once per machine)

```sh
T=.claude/skills/tracy-profile/scripts/tracy.py
python $T setup     # vcpkg builds tracy[cli-tools] into build/tracy-tools (gitignored)
```

It uses the same vcpkg checkout as the engine, so the tools always match the client version
(a mismatched version refuses to connect). Works on Windows, Linux and macOS.

## Commands

| Command | What you get |
|---|---|
| `capture <exe> [--seconds 10] [--out f.tracy] [-- app args]` | A trace of the first N seconds after connect, startup included. The app is killed after. Fails fast if port 8086 is already taken. |
| `zones <trace> [--from S] [--to S] [--self] [--sort total\|mean\|p95\|max\|count] [--filter txt] [--top N]` | Per zone: count, total, % of time span, mean, p95, max, threads, source line. `--self` subtracts child zones, which is the one to use for "where does the time go". |
| `frames <trace> [--from S] [--spikes N] [--anchor zone]` | Frame times (from successive `Engine::beginFrame` starts): mean/fps, p50/p95/p99/max, count over 2× median, and the slowest frames broken into the self time of their zones plus untracked time. |
| `compare <a> <b> [--from S]` | Frame stats of both, then the self time per frame of each zone, sorted by change. Use it for before/after an optimization. |

Put traces in the scratchpad directory. `zones`/`frames` cache CSVs next to the trace.

## Workflow

```
1. build Release for real numbers   cmake --build --preset x64-release --target <example>   (macOS-release on macOS)
   (Debug is fine for "what runs and how often", not for timings)
2. capture (10 s)                   loading is in the first 1-3 s
3. frames --from 3                  steady state: is it CPU or GPU bound? any spikes?
4. zones --from 3 --self            where the CPU time goes
5. zones --to 3                     startup/loading cost (importMeshFile, fillTexture, createPipeline, the first syncGpuScene)
6. change code → capture again → compare old new --from 3
```

## Reading RASM traces

- **Thread 1 is the main thread.** Other threads are `std::async` loaders (`importMeshFile`, `decodeTexture`).
- **Frame zones**: `render` → `beginFrame` (contains `waitForFence`, `syncGpuScene` with `rebuildDrawList` and `GpuTable::upload` when something changed, `acquireNextImage`) → `drawScene` (`createPipeline` on first use) → `endFrame` (`recordCulling`, `submit`, `present`, then `FrameMark`).
- **`waitForFence` big = GPU bound.** The CPU waits for the frame-in-flight slot. Tracy has **no GPU zones** in RASM yet (`D16` in `docs/code_review.md`), so GPU cost only shows up as this wait. For GPU-side questions use the `frame-debug` skill, or propose adding Tracy Vulkan GPU zones (ask first, it is a new subsystem).
- **Present mode matters**: with vsync, `waitForFence`/`acquireNextImage` include waiting for the display, so frame time is capped (16.7 ms at 60 Hz).
- **Noise**: two runs of the same build differ by about 5-8% on sub-0.1 ms zones. Before calling a change faster, repeat the capture, or only trust differences clearly above that.
- **Untracked time** in `frames` = main-thread time inside the frame not covered by any zone. If it is large, add zones there.

## Adding zones

- `#include "tracy/Tracy.hpp"` in the `.cpp` only. Public headers must not include Tracy (see `docs/coding_style.md`).
- `ZoneScoped;` as the first line of a function. For a block inside a function, use a named zone: `ZoneNamedN(uploadZone, "uploadScene", true);` (see `src/renderer/gpuScene.cpp`).
- Don't put zones in functions that run per primitive or per object every frame (e.g. `fillBuffer`). Put one zone around the loop instead (`architecture.md` explains this).
- `FrameMark` lives only in `Engine::endFrame`. Don't add more.
- Update the zone list in `docs/architecture.md` (frame loop section) when zones change meaningfully.

## Opening a trace in the GUI

The user can open the `.tracy` file in the Tracy profiler GUI of the **same version** (the
Tracy GitHub release, or the port's `gui-tools` feature). Hand them the path.
