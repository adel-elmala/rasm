#include "assets/meshLod.h"

#include "doctest/doctest.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    // A bumpy grid of size x size quads, with every triangle corner its own vertex, as OBJ import makes it.
    rasm::ObjRaw makeUnweldedGrid(uint32_t size)
    {
        rasm::ObjRaw raw;
        auto corner = [&](uint32_t x, uint32_t y)
        {
            rasm::Vertex vertex{};
            vertex.pos = {static_cast<float>(x), 0.1f * std::sin(static_cast<float>(x)) * std::cos(static_cast<float>(y)), static_cast<float>(y)};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            raw.indices.push_back(static_cast<uint32_t>(raw.vertices.size()));
            raw.vertices.push_back(vertex);
        };
        for (uint32_t y = 0; y < size; ++y)
        {
            for (uint32_t x = 0; x < size; ++x)
            {
                corner(x, y);
                corner(x, y + 1);
                corner(x + 1, y);
                corner(x + 1, y);
                corner(x, y + 1);
                corner(x + 1, y + 1);
            }
        }
        return raw;
    }
}

TEST_CASE("a large mesh gets a chain of coarser LODs that share its welded vertices")
{
    rasm::ObjRaw raw = makeUnweldedGrid(64);
    const size_t indexCount = raw.indices.size();
    rasm::buildMeshLods(raw);

    CHECK(raw.vertices.size() == 65 * 65);
    CHECK(raw.indices.size() == indexCount);
    REQUIRE(!raw.lods.empty());
    CHECK(raw.lods.size() < rasm::MAX_MESH_LODS);

    size_t previousCount = raw.indices.size();
    float previousError = 0.0f;
    for (const auto &lod : raw.lods)
    {
        CHECK(lod.indices.size() % 3 == 0);
        CHECK(lod.indices.size() < previousCount);
        CHECK(lod.error >= previousError);
        for (uint32_t index : lod.indices)
            REQUIRE(index < raw.vertices.size());
        previousCount = lod.indices.size();
        previousError = lod.error;
    }
}

TEST_CASE("every LOD keeps the mesh's open border, so neighbouring sub-meshes meet at any LOD")
{
    rasm::ObjRaw raw = makeUnweldedGrid(64);
    rasm::buildMeshLods(raw);
    REQUIRE(!raw.lods.empty());

    for (const auto &lod : raw.lods)
    {
        std::vector<bool> used(raw.vertices.size(), false);
        for (uint32_t index : lod.indices)
            used[index] = true;
        for (size_t i = 0; i < raw.vertices.size(); ++i)
        {
            const glm::vec3 pos = raw.vertices[i].pos;
            if (pos.x == 0.0f || pos.x == 64.0f || pos.z == 0.0f || pos.z == 64.0f)
                REQUIRE(used[i]);
        }
    }
}

TEST_CASE("a small mesh gets no LODs and keeps its vertices")
{
    rasm::ObjRaw raw = makeUnweldedGrid(4);
    const size_t vertexCount = raw.vertices.size();
    rasm::buildMeshLods(raw);

    CHECK(raw.lods.empty());
    CHECK(raw.vertices.size() == vertexCount);
}
