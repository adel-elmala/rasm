// clang-format off
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace rasm::rhi
{
    // The timelines a release waits for: the graphics queue's frame fence, the compute queue's fence and the uploads'.
    constexpr uint32_t                          RELEASE_TIMELINES       = 3;

    // Frees that wait until the GPU can no longer use what they free. release() keeps one until the end of the frame,
    // when tag() gives it the values the frame's submits signal (and the uploads' last one); collect() runs it once
    // every timeline has reached them. Tagged values only grow, so the queue is in order and collect stops at the first
    // one that is not done.
    class ReleaseQueue
    {
    public:
        void                                    release(std::function<void()> free);
        void                                    tag(const uint64_t* values);
        bool                                    hasTagged() const;
        void                                    collect(const uint64_t* completed);
        void                                    releaseAll();
        uint32_t                                waitingCount() const;

    private:
        struct Entry
        {
            std::function<void()>               free;
            uint64_t                            values[RELEASE_TIMELINES]   = {};
        };

        std::vector<std::function<void()>>      untagged;
        std::deque<Entry>                       tagged;
    };
}
// clang-format on
