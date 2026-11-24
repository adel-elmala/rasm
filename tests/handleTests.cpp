#include "rasm/handle.h"

#include "doctest/doctest.h"

#include <unordered_map>

TEST_CASE("a default handle is invalid")
{
    CHECK_FALSE(rasm::MeshHandle{}.isValid());
    rasm::HandlePool pool;
    CHECK_FALSE(pool.isAlive(0, 0));
}

TEST_CASE("the pool hands out indices from 1, at generation 1")
{
    rasm::HandlePool pool;

    const rasm::HandleSlot first = pool.allocate();
    const rasm::HandleSlot second = pool.allocate();

    CHECK(first.index == 1);
    CHECK(second.index == 2);
    CHECK(first.generation == 1);
    CHECK(pool.isAlive(first.index, first.generation));
    CHECK(pool.isAlive(second.index, second.generation));
    CHECK(pool.size() == 3);
    CHECK(pool.liveCount() == 2);
}

TEST_CASE("a released index is reused with a new generation, and old handles to it are stale")
{
    rasm::HandlePool pool;
    const rasm::HandleSlot a = pool.allocate();
    const rasm::HandleSlot b = pool.allocate();
    const rasm::HandleSlot c = pool.allocate();

    CHECK(pool.release(b.index, b.generation));
    CHECK_FALSE(pool.isAlive(b.index, b.generation));
    CHECK(pool.liveCount() == 2);

    const rasm::HandleSlot d = pool.allocate();
    CHECK(d.index == b.index);
    CHECK(d.generation == b.generation + 1);
    CHECK(pool.isAlive(d.index, d.generation));
    CHECK_FALSE(pool.isAlive(b.index, b.generation));
    CHECK(pool.isAlive(a.index, a.generation));
    CHECK(pool.isAlive(c.index, c.generation));
    // No new index: the table indexed by handle does not grow.
    CHECK(pool.size() == 4);
}

TEST_CASE("the last released index is reused first")
{
    rasm::HandlePool pool;
    const rasm::HandleSlot a = pool.allocate();
    const rasm::HandleSlot b = pool.allocate();
    pool.release(a.index, a.generation);
    pool.release(b.index, b.generation);

    CHECK(pool.allocate().index == b.index);
    CHECK(pool.allocate().index == a.index);
    CHECK(pool.allocate().index == 3);
}

TEST_CASE("releasing a stale, unknown or invalid handle fails and changes nothing")
{
    rasm::HandlePool pool;
    const rasm::HandleSlot a = pool.allocate();

    CHECK_FALSE(pool.release(a.index, a.generation + 1));
    CHECK_FALSE(pool.release(7, 1));
    CHECK_FALSE(pool.release(0, 0));
    CHECK(pool.isAlive(a.index, a.generation));

    CHECK(pool.release(a.index, a.generation));
    CHECK_FALSE(pool.release(a.index, a.generation));
    CHECK(pool.liveCount() == 0);
}

TEST_CASE("a free slot's generation matches no handle")
{
    rasm::HandlePool pool;
    const rasm::HandleSlot a = pool.allocate();
    pool.release(a.index, a.generation);

    // Neither the old generation nor the one the next handle will get is alive while the slot is free.
    CHECK_FALSE(pool.isAlive(a.index, a.generation));
    CHECK_FALSE(pool.isAlive(a.index, a.generation + 1));
}

TEST_CASE("a pool with a limit hands out limit - 1 live indices, and reuses released ones")
{
    rasm::HandlePool pool(4);
    const rasm::HandleSlot a = pool.allocate();
    pool.allocate();
    pool.allocate();

    const rasm::HandleSlot full = pool.allocate();
    CHECK(full.index == 0);
    CHECK(full.generation == 0);

    pool.release(a.index, a.generation);
    const rasm::HandleSlot reused = pool.allocate();
    CHECK(reused.index == a.index);
    CHECK(pool.liveCount() == 3);
}

TEST_CASE("many create and destroy cycles keep the table at its peak size")
{
    rasm::HandlePool pool;
    rasm::HandleSlot live[16];
    for (auto &slot : live)
        slot = pool.allocate();
    for (int round = 0; round < 1000; ++round)
    {
        const int i = round % 16;
        CHECK(pool.release(live[i].index, live[i].generation));
        live[i] = pool.allocate();
    }
    CHECK(pool.size() == 17);
    CHECK(pool.liveCount() == 16);
}

TEST_CASE("handles work as hash map keys")
{
    rasm::HandlePool pool;
    const rasm::HandleSlot a = pool.allocate();
    const rasm::HandleSlot b = pool.allocate();
    const rasm::TextureHandle first{a.index, a.generation};
    const rasm::TextureHandle second{b.index, b.generation};

    std::unordered_map<rasm::TextureHandle, int, rasm::HandleHash> map;
    map[first] = 1;
    map[second] = 2;

    CHECK(map.size() == 2);
    CHECK(map[rasm::TextureHandle{first.index, first.generation}] == 1);
    CHECK(rasm::HandleHash{}(first) == rasm::HandleHash{}(rasm::TextureHandle{first.index, first.generation}));
}

TEST_CASE("handles differing only in generation are distinct")
{
    rasm::TextureHandle first{1, 1};
    rasm::TextureHandle second{1, 2};

    CHECK_FALSE(first == second);
}

TEST_CASE("a generation never passes MAX_HANDLE_GENERATION, which fits in 32 bits")
{
    CHECK(rasm::MAX_HANDLE_GENERATION == UINT32_MAX);
}

TEST_CASE("a slot that reaches the pool's highest generation is retired, never reused")
{
    rasm::HandlePool pool(UINT32_MAX, 3);
    rasm::HandleSlot slot = pool.allocate();
    CHECK(slot.generation == 1);
    for (uint64_t generation = 2; generation <= 3; ++generation)
    {
        CHECK(pool.release(slot.index, slot.generation));
        slot = pool.allocate();
        CHECK(slot.index == 1);
        CHECK(slot.generation == generation);
    }
    // At generation 3 the release frees the handle but keeps the index off the free list.
    CHECK(pool.release(slot.index, slot.generation));
    CHECK_FALSE(pool.isAlive(slot.index, slot.generation));
    const rasm::HandleSlot next = pool.allocate();
    CHECK(next.index == 2);
    CHECK(next.generation == 1);
    CHECK(pool.liveCount() == 1);
}
