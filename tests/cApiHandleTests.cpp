#include "c/cApi.h"
#include "rasm/handle.h"

#include "doctest/doctest.h"

// The C API's handle: one uint64, generation << 32 | index (src/c/cApi.h).

TEST_CASE("a C handle round-trips every index and generation a pool hands out")
{
    const uint64_t indices[] = {1, 2, 0xFFFFu, UINT32_MAX - 1};
    const uint64_t generations[] = {1, 2, 0x12345678u, rasm::MAX_HANDLE_GENERATION};
    for (const uint64_t index : indices)
    {
        for (const uint64_t generation : generations)
        {
            const rasm_mesh mesh = rasm::c::toC(rasm::MeshHandle{index, generation});
            CHECK(mesh.id != 0);
            const rasm::MeshHandle back = rasm::c::toCpp(mesh);
            CHECK(back.index == index);
            CHECK(back.generation == generation);
        }
    }
}

TEST_CASE("only the invalid handle packs to 0, and 0 unpacks to an invalid handle")
{
    CHECK(rasm::c::toC(rasm::EntityHandle{}).id == 0);
    CHECK_FALSE(rasm::c::toCpp(rasm_entity{0}).isValid());
    // Generations start at 1, so a valid handle with index 0 still has bits set.
    CHECK(rasm::c::packHandle(0, 1) != 0);
}

TEST_CASE("a handle that does not fit in 32 + 32 bits packs to 0 instead of to another handle")
{
    CHECK(rasm::c::packHandle(1, rasm::MAX_HANDLE_GENERATION + 1) == 0);
    CHECK(rasm::c::packHandle(uint64_t{UINT32_MAX} + 1, 1) == 0);
}

TEST_CASE("the handles of a pool's whole life fit, up to the slot's retirement")
{
    rasm::HandlePool pool(UINT32_MAX, 4);
    rasm::HandleSlot slot = pool.allocate();
    for (int round = 0; round < 3; ++round)
    {
        const rasm::LightHandle light{slot.index, slot.generation};
        CHECK(rasm::c::toCpp(rasm::c::toC(light)) == light);
        pool.release(slot.index, slot.generation);
        slot = pool.allocate();
    }
    CHECK(slot.generation == 4);
    CHECK(rasm::c::toC(rasm::LightHandle{slot.index, slot.generation}).id == (uint64_t{4} << 32 | slot.index));
}
