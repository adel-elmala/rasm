// clang-format off
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rasm
{
    enum class LoadState : uint32_t
    {
        RUNNING,        // queued or running
        DONE,           // the result is written, the main thread may read it
        DROPPED,        // the main thread let go of it: the job gives its bytes back
    };

    // One load's link between the main thread and its jobs. bytes is the part of the pool's budget the load holds, from
    // the start of its job until whoever drops the result gives it back.
    struct LoadTicket
    {
        std::atomic<bool>                       cancelled               = false;
        std::atomic<LoadState>                  state                   = LoadState::RUNNING;
        std::atomic<uint64_t>                   bytes                   = 0;
    };

    struct WorkerJob
    {
        std::function<void()>                   run;
        uint64_t                                bytes                   = 0;    // the budget it takes when it starts
        std::shared_ptr<LoadTicket>             ticket                  = {};   // cancelled before it starts: dropped, never run
    };

    struct WorkerPoolStats
    {
        uint32_t                                workers                 = 0;
        uint32_t                                queued                  = 0;
        uint32_t                                running                 = 0;
        uint64_t                                bytes                   = 0;
        uint64_t                                peakBytes               = 0;
    };

    // A fixed set of threads that run jobs from two queues: the jobs a running job spawned first (so a started load
    // finishes before new ones start), then the loads in order. A load starts only when its bytes fit in the budget
    // next to what the started loads hold, or when nothing is held. No job waits for another: a load that splits into
    // child jobs is finished by the last of them.
    class WorkerPool
    {
    public:
        WorkerPool(uint32_t threadCount, uint64_t byteBudget, const std::string& name);
        ~WorkerPool();

        void                                    push(WorkerJob job);
        void                                    pushChild(std::function<void()> run);
        void                                    setLoadBytes(LoadTicket& ticket, uint64_t bytes);
        void                                    finishLoad(LoadTicket& ticket);
        void                                    dropLoad(LoadTicket& ticket);
        void                                    addBytes(uint64_t bytes);
        void                                    releaseBytes(uint64_t bytes);
        uint64_t                                completions() const;
        void                                    waitForCompletion(uint64_t seen);
        WorkerPoolStats                         stats() const;
        void                                    shutdown();

    private:
        void                                    work(uint32_t index);

        mutable std::mutex                      mutex;
        std::condition_variable                 wake;
        std::condition_variable                 completed;
        std::deque<std::function<void()>>       children;
        std::deque<WorkerJob>                   loads;
        std::vector<std::thread>                threads;
        std::string                             name;
        uint64_t                                budget                  = 0;
        uint64_t                                bytes                   = 0;
        uint64_t                                peakBytes               = 0;
        uint64_t                                completionCount         = 0;
        uint32_t                                running                 = 0;
        bool                                    stopping                = false;
    };
}
// clang-format on
