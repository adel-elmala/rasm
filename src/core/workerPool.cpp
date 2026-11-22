#include "core/workerPool.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace rasm
{
    WorkerPool::WorkerPool(uint32_t threadCount, uint64_t byteBudget, const std::string &name) : name(name), budget(byteBudget)
    {
        threads.reserve(threadCount);
        for (uint32_t i = 0; i < threadCount; ++i)
            threads.emplace_back([this, i]()
                                 { work(i); });
    }

    WorkerPool::~WorkerPool()
    {
        shutdown();
    }

    void WorkerPool::push(WorkerJob job)
    {
        {
            std::lock_guard lock(mutex);
            if (!stopping)
                loads.push_back(std::move(job));
        }
        wake.notify_one();
    }

    // A started load's next step: first in the load queue, so it goes before loads that have not started, but it
    // waits for the budget like them. Its bytes add to what its ticket already holds.
    void WorkerPool::pushNext(WorkerJob job)
    {
        {
            std::lock_guard lock(mutex);
            if (!stopping)
                loads.push_front(std::move(job));
        }
        wake.notify_one();
    }

    void WorkerPool::pushChild(std::function<void()> run)
    {
        {
            std::lock_guard lock(mutex);
            if (!stopping)
                children.push_back(std::move(run));
        }
        wake.notify_one();
    }

    // Runs on the job's thread while the load runs; the last call before finishLoad sets what the result holds.
    void WorkerPool::setLoadBytes(LoadTicket &ticket, uint64_t loadBytes)
    {
        {
            std::lock_guard lock(mutex);
            bytes = bytes - ticket.bytes.load() + loadBytes;
            peakBytes = std::max(peakBytes, bytes);
            ticket.bytes = loadBytes;
        }
        wake.notify_all();
    }

    // The result is written. If the main thread dropped the load meanwhile, nobody will read it, so its bytes go now.
    void WorkerPool::finishLoad(LoadTicket &ticket)
    {
        LoadState expected = LoadState::RUNNING;
        if (!ticket.state.compare_exchange_strong(expected, LoadState::DONE, std::memory_order_acq_rel))
            releaseBytes(ticket.bytes.exchange(0));
        {
            std::lock_guard lock(mutex);
            ++completionCount;
        }
        completed.notify_all();
    }

    // Main thread: a queued load is dropped when a worker reaches it, and a running one gives its bytes back when it
    // ends. A finished one's bytes are returned, for the caller to give back once it has freed the result (P26).
    uint64_t WorkerPool::dropLoad(LoadTicket &ticket)
    {
        ticket.cancelled = true;
        LoadState expected = LoadState::RUNNING;
        const uint64_t finished = ticket.state.compare_exchange_strong(expected, LoadState::DROPPED, std::memory_order_acq_rel) ? 0 : ticket.bytes.exchange(0);
        // A dropped load at the head of the queue may hold back the ones behind it.
        wake.notify_all();
        return finished;
    }

    void WorkerPool::releaseBytes(uint64_t released)
    {
        if (released == 0)
            return;
        {
            std::lock_guard lock(mutex);
            bytes -= std::min(bytes, released);
        }
        wake.notify_all();
    }

    uint64_t WorkerPool::completions() const
    {
        std::lock_guard lock(mutex);
        return completionCount;
    }

    // Returns once a load finished after completions() returned seen, or after a short time, so the caller looks again.
    void WorkerPool::waitForCompletion(uint64_t seen)
    {
        std::unique_lock lock(mutex);
        completed.wait_for(lock, std::chrono::milliseconds(100), [&]()
                           { return completionCount != seen || stopping; });
    }

    WorkerPoolStats WorkerPool::stats() const
    {
        std::lock_guard lock(mutex);
        return {.workers = static_cast<uint32_t>(threads.size()),
                .queued = static_cast<uint32_t>(children.size() + loads.size()),
                .running = running,
                .bytes = bytes,
                .peakBytes = peakBytes};
    }

    // Queued jobs are dropped; running ones finish (a load checks its ticket between steps), then the threads end.
    void WorkerPool::shutdown()
    {
        std::deque<std::function<void()>> droppedChildren;
        std::deque<WorkerJob> droppedLoads;
        {
            std::lock_guard lock(mutex);
            stopping = true;
            droppedChildren.swap(children);
            droppedLoads.swap(loads);
        }
        wake.notify_all();
        completed.notify_all();
        for (std::thread &thread : threads)
        {
            if (thread.joinable())
                thread.join();
        }
    }

    void WorkerPool::work(uint32_t index)
    {
        const std::string threadName = name + " " + std::to_string(index);
        tracy::SetThreadName(threadName.c_str());

        for (;;)
        {
            std::function<void()> run;
            std::vector<WorkerJob> dropped;
            {
                std::unique_lock lock(mutex);
                for (;;)
                {
                    if (stopping)
                        return;
                    if (!children.empty())
                    {
                        run = std::move(children.front());
                        children.pop_front();
                        break;
                    }
                    // A dropped load's job never runs, and what its earlier steps held goes back now.
                    while (!loads.empty() && loads.front().ticket && loads.front().ticket->cancelled)
                    {
                        bytes -= std::min(bytes, loads.front().ticket->bytes.exchange(0));
                        dropped.push_back(std::move(loads.front()));
                        loads.pop_front();
                    }
                    // The bytes a load's earlier steps hold do not hold back its next step, or a load bigger than the
                    // budget would wait for itself.
                    const uint64_t own = !loads.empty() && loads.front().ticket ? loads.front().ticket->bytes.load() : 0;
                    if (!loads.empty() && (bytes == own || bytes + loads.front().bytes <= budget))
                    {
                        WorkerJob &job = loads.front();
                        bytes += job.bytes;
                        peakBytes = std::max(peakBytes, bytes);
                        if (job.ticket)
                            job.ticket->bytes += job.bytes;
                        run = std::move(job.run);
                        loads.pop_front();
                        break;
                    }
                    wake.wait(lock);
                }
                ++running;
            }
            // Outside the lock: a dropped job's captures may be large.
            dropped.clear();
            run();
            run = nullptr;
            {
                std::lock_guard lock(mutex);
                --running;
            }
        }
    }
}
