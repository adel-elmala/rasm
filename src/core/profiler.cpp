#include "rasm/core/profiler.h"
#include "rasm/core/engine.h"

void rasm::Engine::queryMemoryStats()
{
    profiler.gpu_memory_stats = ctx.queryVRAMStats();
}
