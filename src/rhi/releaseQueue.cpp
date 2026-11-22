#include "rhi/releaseQueue.h"

#include <utility>

namespace rasm::rhi
{
    void ReleaseQueue::release(std::function<void()> free)
    {
        untagged.push_back(std::move(free));
    }

    void ReleaseQueue::tag(const uint64_t *values)
    {
        for (auto &free : untagged)
        {
            Entry entry{.free = std::move(free)};
            for (uint32_t i = 0; i < RELEASE_TIMELINES; ++i)
                entry.values[i] = values[i];
            tagged.push_back(std::move(entry));
        }
        untagged.clear();
    }

    bool ReleaseQueue::hasTagged() const
    {
        return !tagged.empty();
    }

    void ReleaseQueue::collect(const uint64_t *completed)
    {
        while (!tagged.empty())
        {
            const Entry &entry = tagged.front();
            for (uint32_t i = 0; i < RELEASE_TIMELINES; ++i)
            {
                if (completed[i] < entry.values[i])
                    return;
            }
            // Taken off first: a free may release more (a heap after the textures placed in it).
            std::function<void()> free = std::move(tagged.front().free);
            tagged.pop_front();
            free();
        }
    }

    // The device is idle, so nothing waits.
    void ReleaseQueue::releaseAll()
    {
        while (!tagged.empty() || !untagged.empty())
        {
            const uint64_t done[RELEASE_TIMELINES] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
            tag(done);
            collect(done);
        }
    }

    uint32_t ReleaseQueue::waitingCount() const
    {
        return static_cast<uint32_t>(untagged.size() + tagged.size());
    }
}
