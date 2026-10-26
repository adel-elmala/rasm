#include "rasm/profiler.h"
#include "rasm/engine.h"

#include <algorithm>

void rasm::Profiler::updateDeltaTime()
{
    const auto now = std::chrono::steady_clock::now();

    if (!has_last_frame_time)
    {
        last_frame_time = now;
        has_last_frame_time = true;
        deltaTime = 0.0f;
        return;
    }

    deltaTime = std::chrono::duration<float>(now - last_frame_time).count();
    deltaTime = std::min(deltaTime, 0.1f); // avoid huge steps after a pause
    last_frame_time = now;
}

void rasm::Engine::queryMemoryStats()
{
    profiler.gpu_memory_stats = ctx.queryVRAMStats();
}
