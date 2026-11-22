#include "core/workerPool.h"

#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using rasm::LoadState;
using rasm::LoadTicket;
using rasm::WorkerJob;
using rasm::WorkerPool;

namespace
{
    // The order in which jobs ran, written from the workers.
    struct Order
    {
        std::mutex mutex;
        std::vector<int> ids;

        void add(int id)
        {
            std::lock_guard lock(mutex);
            ids.push_back(id);
        }

        std::vector<int> get()
        {
            std::lock_guard lock(mutex);
            return ids;
        }
    };

    // Keeps a worker busy until open() is called.
    struct Gate
    {
        std::promise<void> promise;
        std::shared_future<void> future = promise.get_future().share();

        std::function<void()> job()
        {
            return [f = future]()
            { f.wait(); };
        }

        void open()
        {
            promise.set_value();
        }
    };

    bool waitUntil(const std::function<bool()> &done)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done())
        {
            if (std::chrono::steady_clock::now() > end)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // Long enough for a worker that could start a job to have started it.
    void settle()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

TEST_CASE("one worker runs loads in the order they were pushed")
{
    WorkerPool pool(1, UINT64_MAX, "test");
    Order order;
    Gate gate;
    pool.push({.run = gate.job()});
    for (int i = 0; i < 8; ++i)
        pool.push({.run = [&order, i]()
                   { order.add(i); }});
    gate.open();
    REQUIRE(waitUntil([&]()
                      { return order.get().size() == 8; }));
    CHECK(order.get() == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7});
}

TEST_CASE("a job's children run before the loads queued after it")
{
    WorkerPool pool(1, UINT64_MAX, "test");
    Order order;
    Gate gate;
    pool.push({.run = gate.job()});
    pool.push({.run = [&order, &pool]()
               {
                   order.add(1);
                   pool.pushChild([&order]()
                                  { order.add(2); });
                   pool.pushChild([&order]()
                                  { order.add(3); });
               }});
    pool.push({.run = [&order]()
               { order.add(4); }});
    gate.open();
    REQUIRE(waitUntil([&]()
                      { return order.get().size() == 4; }));
    CHECK(order.get() == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("with one worker, a load split into child jobs is finished by its last child")
{
    WorkerPool pool(1, UINT64_MAX, "test");
    auto ticket = std::make_shared<LoadTicket>();
    auto remaining = std::make_shared<std::atomic<uint32_t>>(0);
    auto done = std::make_shared<std::atomic<uint32_t>>(0);
    pool.push({.run = [&pool, ticket, remaining, done]()
               {
                   // Nested: each child spawns a child of its own, and nobody waits.
                   *remaining = 6;
                   for (int i = 0; i < 3; ++i)
                       pool.pushChild([&pool, ticket, remaining, done]()
                                      {
                                          ++*done;
                                          pool.pushChild([&pool, ticket, remaining, done]()
                                                         {
                                                             ++*done;
                                                             if (remaining->fetch_sub(1) == 1)
                                                                 pool.finishLoad(*ticket);
                                                         });
                                          if (remaining->fetch_sub(1) == 1)
                                              pool.finishLoad(*ticket); });
               },
               .ticket = ticket});

    const uint64_t seen = 0;
    REQUIRE(waitUntil([&]()
                      {
                          pool.waitForCompletion(seen);
                          return ticket->state == LoadState::DONE; }));
    CHECK(*done == 6);
    CHECK(pool.completions() == 1);
}

TEST_CASE("a load dropped while queued never runs and takes no bytes")
{
    WorkerPool pool(1, 1000, "test");
    Gate gate;
    std::atomic<bool> ran = false;
    std::atomic<bool> after = false;
    pool.push({.run = gate.job()});
    auto ticket = std::make_shared<LoadTicket>();
    pool.push({.run = [&ran]()
               { ran = true; },
               .bytes = 400,
               .ticket = ticket});
    pool.push({.run = [&after]()
               { after = true; }});
    pool.releaseBytes(pool.dropLoad(*ticket));
    gate.open();
    REQUIRE(waitUntil([&]()
                      { return after.load(); }));
    CHECK_FALSE(ran);
    CHECK(ticket->state == LoadState::DROPPED);
    CHECK(pool.stats().bytes == 0);
    CHECK(pool.stats().peakBytes == 0);
}

TEST_CASE("a load dropped while it runs gives its bytes back when it ends; a finished one when it is dropped")
{
    WorkerPool pool(2, 1000, "test");

    SUBCASE("running")
    {
        Gate gate;
        auto ticket = std::make_shared<LoadTicket>();
        pool.push({.run = [&pool, ticket, f = gate.future]()
                   {
                       f.wait();
                       pool.setLoadBytes(*ticket, 300);
                       pool.finishLoad(*ticket);
                   },
                   .bytes = 200,
                   .ticket = ticket});
        REQUIRE(waitUntil([&]()
                          { return pool.stats().running == 1; }));
        CHECK(pool.stats().bytes == 200);
        pool.releaseBytes(pool.dropLoad(*ticket));
        CHECK(ticket->cancelled);
        gate.open();
        REQUIRE(waitUntil([&]()
                          { return pool.stats().running == 0 && pool.stats().bytes == 0; }));
        CHECK(ticket->state == LoadState::DROPPED);
        CHECK(pool.stats().peakBytes == 300);
    }

    SUBCASE("finished")
    {
        auto ticket = std::make_shared<LoadTicket>();
        pool.push({.run = [&pool, ticket]()
                   { pool.finishLoad(*ticket); },
                   .bytes = 200,
                   .ticket = ticket});
        REQUIRE(waitUntil([&]()
                          { return ticket->state == LoadState::DONE; }));
        // A finished load holds its bytes until the main thread takes or drops it.
        CHECK(pool.stats().bytes == 200);
        pool.releaseBytes(pool.dropLoad(*ticket));
        CHECK(pool.stats().bytes == 0);
    }
}

TEST_CASE("a load that does not fit in the budget waits in the queue until bytes come back")
{
    WorkerPool pool(2, 100, "test");
    auto first = std::make_shared<LoadTicket>();
    std::atomic<bool> secondRan = false;
    pool.push({.run = [&pool, first]()
               { pool.finishLoad(*first); },
               .bytes = 60,
               .ticket = first});
    REQUIRE(waitUntil([&]()
                      { return first->state == LoadState::DONE; }));
    pool.push({.run = [&secondRan]()
               { secondRan = true; },
               .bytes = 60,
               .ticket = std::make_shared<LoadTicket>()});
    settle();
    CHECK_FALSE(secondRan);
    CHECK(pool.stats().queued == 1);

    // The main thread took the first result: its bytes go back, and the second starts.
    pool.releaseBytes(first->bytes.exchange(0));
    REQUIRE(waitUntil([&]()
                      { return secondRan.load(); }));
    CHECK(pool.stats().peakBytes == 60);
}

TEST_CASE("a load bigger than the budget runs alone, and children are not held back by it")
{
    WorkerPool pool(1, 100, "test");
    auto ticket = std::make_shared<LoadTicket>();
    std::atomic<bool> childRan = false;
    pool.push({.run = [&pool, ticket, &childRan]()
               {
                   pool.pushChild([&pool, ticket, &childRan]()
                                  {
                                      childRan = true;
                                      pool.finishLoad(*ticket); });
               },
               .bytes = 500,
               .ticket = ticket});
    REQUIRE(waitUntil([&]()
                      { return ticket->state == LoadState::DONE; }));
    CHECK(childRan);
    CHECK(pool.stats().bytes == 500);

    // While it holds more than the budget, nothing else starts.
    std::atomic<bool> nextRan = false;
    pool.push({.run = [&nextRan]()
               { nextRan = true; },
               .bytes = 1,
               .ticket = std::make_shared<LoadTicket>()});
    settle();
    CHECK_FALSE(nextRan);
    pool.releaseBytes(pool.dropLoad(*ticket));
    REQUIRE(waitUntil([&]()
                      { return nextRan.load(); }));
}

TEST_CASE("shutdown drops queued jobs and waits for running ones")
{
    std::atomic<int> started = 0;
    std::atomic<int> finished = 0;
    std::atomic<int> queuedRan = 0;
    Gate gate;
    {
        WorkerPool pool(2, UINT64_MAX, "test");
        for (int i = 0; i < 2; ++i)
            pool.push({.run = [&started, &finished, f = gate.future]()
                       {
                           ++started;
                           f.wait();
                           ++finished;
                       }});
        for (int i = 0; i < 10; ++i)
            pool.push({.run = [&queuedRan]()
                       { ++queuedRan; }});
        REQUIRE(waitUntil([&]()
                          { return started == 2; }));

        std::thread opener([&gate]()
                           {
                               std::this_thread::sleep_for(std::chrono::milliseconds(50));
                               gate.open(); });
        pool.shutdown();
        opener.join();
        CHECK(finished == 2);

        // Work pushed after the shutdown is dropped too.
        pool.push({.run = [&queuedRan]()
                   { ++queuedRan; }});
        pool.pushChild([&queuedRan]()
                       { ++queuedRan; });
    }
    CHECK(queuedRan == 0);
}

// Engine::freeOnWorker hands a load's result to a child job: what it holds is freed when the job runs, when shutdown
// drops it from the queue, or at once when the pool is stopping, never leaked (P26).
TEST_CASE("a child job's captures are freed whether it runs, is dropped by shutdown, or comes after it")
{
    auto ran = std::make_shared<int>(0);
    auto queued = std::make_shared<int>(0);
    auto late = std::make_shared<int>(0);
    Gate gate;
    {
        WorkerPool pool(1, UINT64_MAX, "test");
        pool.pushChild([ran]() {});
        REQUIRE(waitUntil([&]()
                          { return ran.use_count() == 1; }));

        // The only worker waits, so this child is still queued when the pool stops. The gate opens on every path, so a
        // failed check cannot leave the worker blocked and the test hung.
        std::atomic<bool> started = false;
        pool.push({.run = [&started, f = gate.future]()
                   {
                       started = true;
                       f.wait();
                   }});
        std::thread opener([&gate]()
                           {
                               std::this_thread::sleep_for(std::chrono::milliseconds(200));
                               gate.open(); });
        CHECK(waitUntil([&]()
                        { return started.load(); }));
        pool.pushChild([queued]() {});
        pool.shutdown();
        opener.join();
        CHECK(queued.use_count() == 1);

        pool.pushChild([late]() {});
        CHECK(late.use_count() == 1);
    }
}

TEST_CASE("the destructor stops a pool that still has work")
{
    std::atomic<int> ran = 0;
    {
        WorkerPool pool(3, UINT64_MAX, "test");
        for (int i = 0; i < 100; ++i)
            pool.push({.run = [&ran]()
                       {
                           std::this_thread::sleep_for(std::chrono::microseconds(100));
                           ++ran;
                       }});
    }
    CHECK(ran <= 100);
}

TEST_CASE("a load's next step goes before queued loads, adds to its bytes, and is not held back by them")
{
    WorkerPool pool(1, 100, "test");
    Order order;
    Gate gate;
    auto ticket = std::make_shared<LoadTicket>();
    pool.push({.run = gate.job()});
    pool.push({.run = [&pool, &order, ticket]()
               {
                   order.add(1);
                   // The first step holds 80 of 100; the next one needs 50 more, which only its own bytes stand against.
                   pool.setLoadBytes(*ticket, 80);
                   pool.pushNext({.run = [&pool, &order, ticket]()
                                  {
                                      order.add(2);
                                      pool.finishLoad(*ticket);
                                  },
                                  .bytes = 50,
                                  .ticket = ticket});
               },
               .ticket = ticket});
    pool.push({.run = [&order]()
               { order.add(3); }});
    gate.open();
    REQUIRE(waitUntil([&]()
                      { return ticket->state == LoadState::DONE; }));
    CHECK(ticket->bytes == 130);
    CHECK(pool.stats().peakBytes == 130);
    // The other load waits until the first gives its bytes back.
    settle();
    CHECK(order.get() == std::vector<int>{1, 2});
    pool.releaseBytes(pool.dropLoad(*ticket));
    REQUIRE(waitUntil([&]()
                      { return order.get().size() == 3; }));
    CHECK(order.get() == std::vector<int>{1, 2, 3});
}

TEST_CASE("a load dropped while its next step waits gives back what its first step held")
{
    WorkerPool pool(1, 100, "test");
    Gate gate;
    std::atomic<bool> nextRan = false;
    auto ticket = std::make_shared<LoadTicket>();
    pool.push({.run = [&pool, &nextRan, ticket, f = gate.future]()
               {
                   pool.setLoadBytes(*ticket, 40);
                   pool.pushNext({.run = [&nextRan]()
                                  { nextRan = true; },
                                  .bytes = 10,
                                  .ticket = ticket});
                   // Keeps the only worker busy, so the next step stays queued.
                   f.wait();
               },
               .ticket = ticket});
    REQUIRE(waitUntil([&]()
                      { return pool.stats().queued == 1 && pool.stats().running == 1; }));
    REQUIRE(waitUntil([&]()
                      { return pool.stats().bytes == 40; }));
    pool.releaseBytes(pool.dropLoad(*ticket));
    gate.open();
    REQUIRE(waitUntil([&]()
                      { return pool.stats().bytes == 0 && pool.stats().queued == 0; }));
    settle();
    CHECK_FALSE(nextRan);
}
