#include "importMeshText.h"

#include "doctest/doctest.h"
#include "assets/meshLod.h"
#include "assets/morphImport.h"
#include "renderer/animation.h"
#include "renderer/uberMaterialTypes.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // A triangle with two morph targets: target 0 moves every vertex and its normal (dense accessors), target 1 moves
    // vertex 2 only, through a sparse accessor without a buffer view. With a clip, a CUBICSPLINE weights channel of two
    // keys animates both weights.
    rasm::GLTFRaw importMorphedTriangle(const std::string &meshExtras, const std::string &nodeWeights, bool withClip)
    {
        const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const float target0Positions[] = {0, 0.5f, 0, 0, 0.5f, 0, 0, 0.5f, 0};
        const float target0Normals[] = {0.1f, 0, 0, 0.2f, 0, 0, 0.3f, 0, 0};
        const uint8_t sparseIndices[] = {2};
        const float sparseValues[] = {0, 0, 2};
        const float times[] = {0.0f, 1.0f};
        // Per key: both in-tangents, both values, both out-tangents.
        const float weights[] = {0.0f, 0.0f, 0.1f, 0.2f, 1.0f, 2.0f, 3.0f, 4.0f, 0.3f, 0.4f, 5.0f, 6.0f};
        std::vector<unsigned char> buffer;
        const std::string views = addView(buffer, positions, sizeof(positions)) + "," + addView(buffer, target0Positions, sizeof(target0Positions)) + "," +
                                  addView(buffer, target0Normals, sizeof(target0Normals)) + "," + addView(buffer, sparseIndices, sizeof(sparseIndices)) + "," +
                                  addView(buffer, sparseValues, sizeof(sparseValues)) + "," + addView(buffer, times, sizeof(times)) + "," +
                                  addView(buffer, weights, sizeof(weights));
        return importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                                                      {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
                                                      {"bufferView":2,"componentType":5126,"count":3,"type":"VEC3"},
                                                      {"componentType":5126,"count":3,"type":"VEC3",
                                                       "sparse":{"count":1,"indices":{"bufferView":3,"componentType":5121},"values":{"bufferView":4}}},
                                                      {"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR"},
                                                      {"bufferView":6,"componentType":5126,"count":12,"type":"SCALAR"}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0},"targets":[{"POSITION":1,"NORMAL":2},{"POSITION":3}]}],"weights":[0.25,0.75])" +
                                             meshExtras + R"(}],
            "nodes":[{"mesh":0,"translation":[1,2,3])" +
                                             nodeWeights + R"(}],
            "scenes":[{"nodes":[0]}])" + std::string(withClip ? R"(,
            "animations":[{"samplers":[{"input":4,"output":5,"interpolation":"CUBICSPLINE"}],"channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}])"
                                                              : ""));
    }

    // A flat grid of size x size quads, with every triangle corner its own vertex, as OBJ import makes it.
    rasm::ObjRaw makeUnweldedGrid(uint32_t size)
    {
        rasm::ObjRaw raw;
        for (uint32_t y = 0; y < size; ++y)
        {
            for (uint32_t x = 0; x < size; ++x)
            {
                const uint32_t corners[6][2] = {{x, y}, {x, y + 1}, {x + 1, y}, {x + 1, y}, {x, y + 1}, {x + 1, y + 1}};
                for (const auto &corner : corners)
                {
                    raw.indices.push_back(static_cast<uint32_t>(raw.vertices.size()));
                    raw.vertices.push_back({.pos = {static_cast<float>(corner[0]), 0.0f, static_cast<float>(corner[1])}, .normal = {0.0f, 1.0f, 0.0f}});
                }
            }
        }
        return raw;
    }
}

TEST_CASE("morph deltas keep positions as floats and normals and tangents as half floats")
{
    const glm::vec3 position(1234.5678f, -0.000123f, 0.5f);
    const glm::vec3 normal(0.123456f, -1.5f, 0.0f);
    const glm::vec3 tangent(-0.75f, 0.333333f, 2.0f);
    const rasm::MorphDelta delta = rasm::packMorphDelta(position, normal, tangent);
    CHECK(delta.position == position);
    // A half float keeps 11 significant bits: the error is at most 2^-11 of the value.
    for (int i = 0; i < 3; ++i)
    {
        CHECK(std::abs(rasm::getMorphNormal(delta)[i] - normal[i]) <= std::abs(normal[i]) / 2048.0f);
        CHECK(std::abs(rasm::getMorphTangent(delta)[i] - tangent[i]) <= std::abs(tangent[i]) / 2048.0f);
    }
}

TEST_CASE("glTF morph targets import target-major, with sparse deltas, the node's weights and the target names")
{
    const rasm::GLTFRaw raw = importMorphedTriangle(R"(,"extras":{"targetNames":["Smile","Blink"]})", R"(,"weights":[0.5,0.0])", true);
    REQUIRE(raw.loaded);
    REQUIRE(raw.subGltfRaws.size() == 1);
    const rasm::SubGLTFRaw &sub = raw.subGltfRaws[0];
    REQUIRE(sub.objRaw.morphTargetCount == 2);
    REQUIRE(sub.objRaw.morphs.size() == 6);
    CHECK(sub.firstWeight == 0);
    // Target 0 first, its three vertices, then target 1, whose only delta is the sparse one of vertex 2.
    CHECK(sub.objRaw.morphs[1].position == glm::vec3(0.0f, 0.5f, 0.0f));
    CHECK(rasm::getMorphNormal(sub.objRaw.morphs[2]).x == doctest::Approx(0.3f).epsilon(0.001));
    CHECK(sub.objRaw.morphs[3].position == glm::vec3(0.0f));
    CHECK(sub.objRaw.morphs[5].position == glm::vec3(0.0f, 0.0f, 2.0f));
    CHECK(rasm::getMorphRadii(sub.objRaw) == std::vector<float>{0.5f, 2.0f});

    // The node's weights win over the mesh's.
    CHECK(raw.animation.morphWeights == std::vector<float>{0.5f, 0.0f});
    CHECK(raw.animation.morphTargetNames == std::vector<std::string>{"Smile", "Blink"});
    // A clip that moves only weights still keeps the node, which places the morphed vertices.
    REQUIRE(sub.animatedNode >= 0);
    CHECK(raw.animation.nodes[sub.animatedNode].translation == glm::vec3(1.0f, 2.0f, 3.0f));

    // One sampler per weight, its value in x: in-tangent, value, out-tangent per key.
    REQUIRE(raw.animation.clips.size() == 1);
    const rasm::AnimationClipRaw &clip = raw.animation.clips[0];
    CHECK(clip.info.duration == 1.0f);
    REQUIRE(clip.samplers.size() == 2);
    REQUIRE(clip.channels.size() == 2);
    CHECK(clip.channels[1].path == rasm::AnimationPath::WEIGHTS);
    CHECK(clip.channels[1].node == 1);
    const rasm::AnimationSampler &blink = clip.samplers[clip.channels[1].sampler];
    CHECK(blink.interpolation == rasm::AnimationInterpolation::CUBICSPLINE);
    REQUIRE(blink.values.size() == 6);
    CHECK(blink.values[0].x == 0.0f);
    CHECK(blink.values[1].x == 0.2f);
    CHECK(blink.values[2].x == 2.0f);
    CHECK(blink.values[4].x == 0.4f);
    CHECK(blink.values[5].x == 6.0f);
}

TEST_CASE("without node weights or names, a morphed mesh rests at its mesh's weights, its targets named by index")
{
    const rasm::GLTFRaw raw = importMorphedTriangle("", "", false);
    REQUIRE(raw.loaded);
    CHECK(raw.animation.clips.empty());
    CHECK(raw.animation.morphWeights == std::vector<float>{0.25f, 0.75f});
    CHECK(raw.animation.morphTargetNames == std::vector<std::string>{"target 0", "target 1"});
    // A morphed mesh is posed with no clip at all, so its node is kept.
    REQUIRE(raw.subGltfRaws.size() == 1);
    CHECK(raw.subGltfRaws[0].animatedNode >= 0);
    CHECK(raw.subGltfRaws[0].objRaw.morphTargetCount == 2);
}

TEST_CASE("the pool data has a channel per morph weight after the nodes' channels, the rest weights and the radii")
{
    rasm::AnimationRaw animation;
    animation.nodes.resize(2);
    animation.morphWeights = {0.25f, 0.75f, 1.0f};
    rasm::AnimationClipRaw clip;
    clip.samplers.push_back({.interpolation = rasm::AnimationInterpolation::LINEAR, .times = {0.0f, 1.0f}, .values = {glm::vec4(0.0f), glm::vec4(1.0f)}});
    clip.channels.push_back({.node = 2, .path = rasm::AnimationPath::WEIGHTS, .sampler = 0});
    animation.clips.push_back(clip);
    const rasm::gfx::AnimationSubMeshData subMeshes[] = {{.node = 1, .vertexCount = 3, .firstSkinnedVertex = 64, .firstWeight = 1, .firstMorphRadius = 0}};
    const float radii[] = {0.5f, 2.0f};
    const uint64_t address = 0x10000;
    const std::vector<uint8_t> bytes = rasm::buildAnimationPoolData(animation, subMeshes, 1, nullptr, 0, radii, 2, address);
    REQUIRE(bytes.size() == rasm::getAnimationPoolBytes(animation, 1, 0, 2));

    rasm::gfx::AnimationMeshData header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    CHECK(header.weightCount == 3);
    CHECK(header.skinnedVertexCount == 67);
    CHECK(header.weights % 16 == 0);

    // A channel row: 2 nodes * 3 paths, then 3 weights. Clip 0, then all clips, then the rest row.
    const size_t row = 2 * 3 + 3;
    std::vector<int32_t> channels(3 * row);
    std::memcpy(channels.data(), bytes.data() + (header.channels - address), channels.size() * sizeof(int32_t));
    CHECK(channels[6 + 2] == 0);
    CHECK(channels[row + 6 + 2] == 0);
    CHECK(channels[6] == -1);
    CHECK(channels[2 * row + 6 + 2] == -1);

    float weights[3];
    std::memcpy(weights, bytes.data() + (header.weights - address), sizeof(weights));
    CHECK(weights[2] == 1.0f);
    float storedRadii[2];
    std::memcpy(storedRadii, bytes.data() + (header.morphRadii - address), sizeof(storedRadii));
    CHECK(storedRadii[1] == 2.0f);
}

TEST_CASE("welding moves the morph deltas with the vertices, and keeps corners with other deltas apart")
{
    // A 32 x 32 grid, unwelded: every corner is its own vertex. One target lifts each corner by its x.
    const uint32_t size = 32;
    rasm::ObjRaw raw = makeUnweldedGrid(size);
    raw.morphTargetCount = 1;
    for (size_t v = 0; v < raw.vertices.size(); ++v)
        raw.morphs.push_back(rasm::packMorphDelta(glm::vec3(0.0f, raw.vertices[v].pos.x, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f)));
    rasm::buildMeshLods(raw);

    CHECK(raw.vertices.size() == (size + 1) * (size + 1));
    REQUIRE(raw.morphs.size() == raw.vertices.size());
    for (size_t v = 0; v < raw.vertices.size(); ++v)
        CHECK(raw.morphs[v].position.y == raw.vertices[v].pos.x);

    // The same grid, where every other copy of a corner moves: the copies of a corner no longer weld into one.
    rasm::ObjRaw split = makeUnweldedGrid(size);
    split.morphTargetCount = 1;
    for (size_t v = 0; v < split.vertices.size(); ++v)
        split.morphs.push_back(rasm::packMorphDelta(glm::vec3(0.0f, static_cast<float>(v % 2), 0.0f), glm::vec3(0.0f), glm::vec3(0.0f)));
    rasm::buildMeshLods(split);
    CHECK(split.vertices.size() > (size + 1) * (size + 1));
    CHECK(split.morphs.size() == split.vertices.size());
}

TEST_CASE("a mesh with more morph targets than the weld can compare gets no LODs")
{
    rasm::ObjRaw raw;
    for (uint32_t i = 0; i < 3 * 2048; ++i)
    {
        raw.indices.push_back(i);
        raw.vertices.push_back({.pos = {static_cast<float>(i % 97), static_cast<float>(i % 89), static_cast<float>(i % 83)}});
    }
    raw.morphTargetCount = 14;
    raw.morphs.assign(raw.morphTargetCount * raw.vertices.size(), rasm::packMorphDelta(glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(0.0f)));
    rasm::buildMeshLods(raw);
    CHECK(raw.lods.empty());
    CHECK(raw.vertices.size() == 3 * 2048);
}
