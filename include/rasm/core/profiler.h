// clang-format off
#pragma once

#include <cstdint>

namespace rasm {

    struct VRAMStats
    {
        uint64_t vram_budget; // VRAM budget in bytes         - DEVICE_LOCAL
        uint64_t vram_usage;  // VRAM usage in bytes          - DEVICE_LOCAL
        uint64_t host_budget; // Host memory budget in bytes  - HOST_VISIBLE
        uint64_t host_usage;  // Host memory usage in bytes   - HOST_VISIBLE
    };

    struct Profiler
    {
        VRAMStats gpu_memory_stats;
    };

} // namespace rasm

// clang-format on