#include "rasm/handle.h"

#include "doctest/doctest.h"

#include <unordered_map>

TEST_CASE("a default handle is invalid")
{
    CHECK_FALSE(rasm::MeshHandle{}.isValid());
}

TEST_CASE("the manager hands out valid handles with sequential indices")
{
    rasm::HandleManager manager;

    auto first = manager.getNextMeshHandle();
    auto second = manager.getNextMeshHandle();

    CHECK(first.isValid());
    CHECK(second.isValid());
    CHECK(first.index == 1);
    CHECK(second.index == 2);
    CHECK_FALSE(first == second);
}

TEST_CASE("each handle type counts independently")
{
    rasm::HandleManager manager;

    manager.getNextMeshHandle();
    manager.getNextMeshHandle();

    CHECK(manager.getNextTextureHandle().index == 1);
    CHECK(manager.getNextBufferHandle().index == 1);
}

TEST_CASE("handles work as hash map keys")
{
    rasm::HandleManager manager;
    auto first = manager.getNextTextureHandle();
    auto second = manager.getNextTextureHandle();

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
