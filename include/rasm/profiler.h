// clang-format off
#pragma once

#include <chrono>
#include <cstdint>

namespace rasm
{

    struct VRAMStats
    {
        uint64_t vramBudget; // VRAM budget in bytes         - DEVICE_LOCAL
        uint64_t vramUsage;  // VRAM usage in bytes          - DEVICE_LOCAL
        uint64_t hostBudget; // Host memory budget in bytes  - HOST_VISIBLE
        uint64_t hostUsage;  // Host memory usage in bytes   - HOST_VISIBLE
    };

    struct Profiler
    {
        VRAMStats gpuMemoryStats = {};
        float     deltaTime = 0.0f; // seconds since the previous frame

        void updateDeltaTime();

    private:
        std::chrono::steady_clock::time_point lastFrameTime{};
        bool                                  hasLastFrameTime = false;
    };

} // namespace rasm

// clang-format on