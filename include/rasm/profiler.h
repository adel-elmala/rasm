// clang-format off
#pragma once

#include <chrono>
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
        VRAMStats gpu_memory_stats = {};
        float     deltaTime = 0.0f; // seconds since the previous frame

        void updateDeltaTime();

    private:
        std::chrono::steady_clock::time_point last_frame_time{};
        bool                                  has_last_frame_time = false;
    };

} // namespace rasm

// clang-format on