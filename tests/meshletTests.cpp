#include "assets/meshLod.h"
#include "assets/meshlets.h"

#include "doctest/doctest.h"
#include "glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    // A UV sphere with shared vertices: a closed mesh whose triangles face every way.
    rasm::ObjRaw makeSphere(uint32_t rings, uint32_t segments)
    {
        rasm::ObjRaw raw;
        for (uint32_t r = 0; r <= rings; ++r)
        {
            const float theta = static_cast<float>(r) / static_cast<float>(rings) * 3.14159265f;
            for (uint32_t s = 0; s <= segments; ++s)
            {
                const float phi = static_cast<float>(s) / static_cast<float>(segments) * 6.28318531f;
                rasm::Vertex vertex{};
                vertex.pos = {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
                vertex.normal = vertex.pos;
                raw.vertices.push_back(vertex);
            }
        }
        for (uint32_t r = 0; r < rings; ++r)
        {
            for (uint32_t s = 0; s < segments; ++s)
            {
                const uint32_t a = r * (segments + 1) + s;
                const uint32_t b = a + segments + 1;
                // Counter-clockwise seen from outside.
                for (uint32_t index : {a, a + 1, b, a + 1, b + 1, b})
                    raw.indices.push_back(index);
            }
        }
        return raw;
    }

    std::vector<std::array<uint32_t, 3>> sortedTriangles(const std::vector<uint32_t> &indices)
    {
        std::vector<std::array<uint32_t, 3>> triangles;
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            // Rotated so the smallest index comes first: the same triangle with the same winding compares equal.
            std::array<uint32_t, 3> triangle = {indices[i], indices[i + 1], indices[i + 2]};
            while (triangle[0] != std::min({triangle[0], triangle[1], triangle[2]}))
                triangle = {triangle[1], triangle[2], triangle[0]};
            triangles.push_back(triangle);
        }
        std::sort(triangles.begin(), triangles.end());
        return triangles;
    }

    // The shader's test (shaders/lib/meshletCulling.slang): the camera moved into object space.
    bool isMeshletBackFacing(const glm::mat4 &world, const rasm::Meshlet &meshlet, const glm::vec3 &camera)
    {
        const glm::vec3 local = glm::inverse(glm::mat3(world)) * (camera - glm::vec3(world[3]));
        const glm::vec3 toCenter = meshlet.center - local;
        return glm::dot(toCenter, meshlet.coneAxis) >= meshlet.coneCutoff * glm::length(toCenter) + meshlet.radius;
    }
}

TEST_CASE("meshlets keep every triangle of each LOD, with its winding, in ranges of at most 124 triangles")
{
    rasm::ObjRaw raw = makeSphere(48, 64);
    rasm::buildMeshLods(raw);
    std::vector<std::vector<uint32_t>> before = {raw.indices};
    for (const rasm::MeshLod &lod : raw.lods)
        before.push_back(lod.indices);
    rasm::buildMeshlets(raw);

    REQUIRE(raw.lodMeshletCounts.size() == raw.lods.size() + 1);
    size_t meshlet = 0;
    for (size_t lod = 0; lod < raw.lodMeshletCounts.size(); ++lod)
    {
        const std::vector<uint32_t> &indices = lod == 0 ? raw.indices : raw.lods[lod - 1].indices;
        CHECK(sortedTriangles(indices) == sortedTriangles(before[lod]));
        // The LOD's meshlets cover its indices one after another.
        uint32_t next = 0;
        for (uint32_t i = 0; i < raw.lodMeshletCounts[lod]; ++i, ++meshlet)
        {
            const rasm::Meshlet &m = raw.meshlets[meshlet];
            CHECK(m.firstIndex == next);
            CHECK(m.indexCount > 0);
            CHECK(m.indexCount <= rasm::MESHLET_MAX_TRIANGLES * 3);
            next += m.indexCount;
        }
        CHECK(next == indices.size());
    }
    CHECK(meshlet == raw.meshlets.size());
}

TEST_CASE("meshlets are the same on every build")
{
    rasm::ObjRaw a = makeSphere(32, 48);
    rasm::ObjRaw b = makeSphere(32, 48);
    rasm::buildMeshlets(a);
    rasm::buildMeshlets(b);
    REQUIRE(a.meshlets.size() == b.meshlets.size());
    CHECK(a.indices == b.indices);
    for (size_t i = 0; i < a.meshlets.size(); ++i)
    {
        CHECK(a.meshlets[i].firstIndex == b.meshlets[i].firstIndex);
        CHECK(a.meshlets[i].center == b.meshlets[i].center);
        CHECK(a.meshlets[i].coneCutoff == b.meshlets[i].coneCutoff);
    }
}

TEST_CASE("small, skinned and morphed sub-meshes get no meshlets")
{
    rasm::ObjRaw small = makeSphere(8, 8);
    const std::vector<uint32_t> indices = small.indices;
    rasm::buildMeshlets(small);
    CHECK(small.meshlets.empty());
    CHECK(small.indices == indices);

    rasm::ObjRaw skinned = makeSphere(32, 32);
    skinned.skin.resize(skinned.vertices.size());
    rasm::buildMeshlets(skinned);
    CHECK(skinned.meshlets.empty());

    rasm::ObjRaw morphed = makeSphere(32, 32);
    morphed.morphTargetCount = 1;
    morphed.morphs.resize(morphed.vertices.size());
    rasm::buildMeshlets(morphed);
    CHECK(morphed.meshlets.empty());
}

TEST_CASE("a meshlet the cone test culls faces away from the camera, also under uneven scale and mirrors")
{
    rasm::ObjRaw raw = makeSphere(32, 48);
    rasm::buildMeshlets(raw);
    REQUIRE(!raw.meshlets.empty());

    const glm::mat4 worlds[] = {
        glm::mat4(1.0f),
        glm::scale(glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -1.0f, 2.0f)), 0.7f, glm::vec3(0.3f, 1.0f, 0.2f)), glm::vec3(4.0f, 0.5f, 1.5f)),
        glm::rotate(glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 2.0f, 1.0f)), 1.1f, glm::vec3(1.0f, 0.0f, 0.0f)),
    };
    const glm::vec3 cameras[] = {{0.0f, 0.0f, 6.0f}, {5.0f, 3.0f, -4.0f}, {-2.0f, -7.0f, 1.0f}, {0.5f, 0.2f, 1.4f}};
    uint32_t culled = 0;
    for (const glm::mat4 &world : worlds)
    {
        // A mirroring matrix turns the authored front faces clockwise; the run draws it with FRONT culling.
        const float side = glm::determinant(glm::mat3(world)) < 0.0f ? -1.0f : 1.0f;
        for (const glm::vec3 &camera : cameras)
        {
            for (const rasm::Meshlet &meshlet : raw.meshlets)
            {
                if (!isMeshletBackFacing(world, meshlet, camera))
                    continue;
                ++culled;
                for (uint32_t i = meshlet.firstIndex; i < meshlet.firstIndex + meshlet.indexCount; i += 3)
                {
                    const glm::vec3 a = glm::vec3(world * glm::vec4(raw.vertices[raw.indices[i]].pos, 1.0f));
                    const glm::vec3 b = glm::vec3(world * glm::vec4(raw.vertices[raw.indices[i + 1]].pos, 1.0f));
                    const glm::vec3 c = glm::vec3(world * glm::vec4(raw.vertices[raw.indices[i + 2]].pos, 1.0f));
                    const glm::vec3 front = side * glm::cross(b - a, c - a);
                    REQUIRE(glm::dot(front, a - camera) >= -1e-5f);
                }
            }
        }
    }
    // The test does cull: about half of a sphere faces away from a camera outside it.
    CHECK(culled > 0);
}
