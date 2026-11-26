// clang-format off
#pragma once

namespace rasm
{
    // Runs the calling thread below the main thread, as RASM's own workers do, so a worker the main thread wakes takes a
    // free core instead of the main thread's.
    void                        lowerThreadPriority();
}
// clang-format on
