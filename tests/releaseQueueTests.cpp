#include "rhi/releaseQueue.h"

#include "doctest/doctest.h"

#include <vector>

using rasm::rhi::ReleaseQueue;

namespace
{
    // graphics, compute, uploads
    struct Timelines
    {
        uint64_t values[rasm::rhi::RELEASE_TIMELINES] = {};
    };
}

TEST_CASE("a release waits for the end of its frame, even when the GPU is ahead")
{
    ReleaseQueue queue;
    std::vector<int> freed;
    queue.release([&freed]()
                  { freed.push_back(1); });

    // Not tagged yet: the frame being recorded may still use it.
    const Timelines done{{100, 100, 100}};
    queue.collect(done.values);
    CHECK(freed.empty());
    CHECK(queue.waitingCount() == 1);

    const Timelines frame{{5, 0, 3}};
    queue.tag(frame.values);
    queue.collect(done.values);
    CHECK(freed == std::vector<int>{1});
    CHECK(queue.waitingCount() == 0);
}

TEST_CASE("a release is freed only when every timeline reached its value")
{
    ReleaseQueue queue;
    int freed = 0;
    queue.release([&freed]()
                  { ++freed; });
    const Timelines frame{{10, 4, 7}};
    queue.tag(frame.values);

    const Timelines graphicsOnly{{10, 3, 7}};
    queue.collect(graphicsOnly.values);
    CHECK(freed == 0);
    const Timelines uploadsBehind{{10, 4, 6}};
    queue.collect(uploadsBehind.values);
    CHECK(freed == 0);
    const Timelines all{{10, 4, 7}};
    queue.collect(all.values);
    CHECK(freed == 1);
}

TEST_CASE("releases are freed in the order of their frames")
{
    ReleaseQueue queue;
    std::vector<int> freed;
    queue.release([&freed]()
                  { freed.push_back(1); });
    queue.release([&freed]()
                  { freed.push_back(2); });
    const Timelines frame1{{1, 0, 0}};
    queue.tag(frame1.values);
    queue.release([&freed]()
                  { freed.push_back(3); });
    const Timelines frame2{{2, 0, 0}};
    queue.tag(frame2.values);

    const Timelines firstDone{{1, 0, 0}};
    queue.collect(firstDone.values);
    CHECK(freed == std::vector<int>{1, 2});
    const Timelines secondDone{{2, 0, 0}};
    queue.collect(secondDone.values);
    CHECK(freed == std::vector<int>{1, 2, 3});
}

TEST_CASE("a dropped frame tags its releases with the last values that were signaled")
{
    ReleaseQueue queue;
    int freed = 0;
    const Timelines frame1{{1, 0, 0}};
    queue.tag(frame1.values);
    queue.release([&freed]()
                  { ++freed; });
    // Frame 2 was dropped: nothing new was submitted, so the values are still frame 1's.
    queue.tag(frame1.values);
    const Timelines firstDone{{1, 0, 0}};
    queue.collect(firstDone.values);
    CHECK(freed == 1);
}

TEST_CASE("a free can release more, which waits for the next frame")
{
    ReleaseQueue queue;
    std::vector<int> freed;
    queue.release([&queue, &freed]()
                  {
                      freed.push_back(1);
                      queue.release([&freed]() { freed.push_back(2); }); });
    const Timelines frame{{1, 1, 1}};
    queue.tag(frame.values);
    queue.collect(frame.values);
    CHECK(freed == std::vector<int>{1});
    CHECK(queue.waitingCount() == 1);
    queue.tag(frame.values);
    queue.collect(frame.values);
    CHECK(freed == std::vector<int>{1, 2});
}

TEST_CASE("releaseAll frees everything, tagged or not, also what a free releases")
{
    ReleaseQueue queue;
    std::vector<int> freed;
    queue.release([&freed]()
                  { freed.push_back(1); });
    const Timelines frame{{50, 50, 50}};
    queue.tag(frame.values);
    queue.release([&queue, &freed]()
                  {
                      freed.push_back(2);
                      queue.release([&freed]() { freed.push_back(3); }); });
    queue.releaseAll();
    CHECK(freed == std::vector<int>{1, 2, 3});
    CHECK(queue.waitingCount() == 0);
    CHECK_FALSE(queue.hasTagged());
}
