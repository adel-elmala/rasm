#include "importMeshText.h"

#include "doctest/doctest.h"
#include "assets/skinImport.h"
#include "renderer/animation.h"
#include "renderer/uberMaterialTypes.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // A triangle skinned by a skin of five joints (nodes 1 to 5), with two sets of influences, and a clip that turns
    // joint 1 (node 2). Vertex 0 has weights that add up to 0.75, vertex 1 names joint 7, which the skin does not
    // have, and vertex 2 has five joints, one of them in both sets.
    rasm::GLTFRaw importSkinnedTriangle(bool withClip = true)
    {
        const float positions[] = {0, 0, 0, 1, 0, 0, 0, 2, 0};
        const uint8_t joints0[] = {0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3};
        const float weights0[] = {0.5f, 0.25f, 0, 0, 1, 0, 0, 0, 0.3f, 0.2f, 0.1f, 0.15f};
        const uint8_t joints1[] = {0, 0, 0, 0, 7, 0, 0, 0, 4, 1, 0, 0};
        const float weights1[] = {0, 0, 0, 0, 0.5f, 0, 0, 0, 0.22f, 0.05f, 0, 0};
        std::vector<float> inverseBinds;
        for (int j = 0; j < 5; ++j)
        {
            // The inverse of each joint's rest world matrix: node 1 at the origin, nodes 2 to 5 one up.
            const float translationY = j == 0 ? 0.0f : -1.0f;
            inverseBinds.insert(inverseBinds.end(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, translationY, 0, 1});
        }
        const float times[] = {0.0f, 1.0f};
        const float rotations[] = {0, 0, 0, 1, 0, 0, 0.7071068f, 0.7071068f};
        std::vector<unsigned char> buffer;
        const std::string views = addView(buffer, positions, sizeof(positions)) + "," + addView(buffer, joints0, sizeof(joints0)) + "," +
                                  addView(buffer, weights0, sizeof(weights0)) + "," + addView(buffer, joints1, sizeof(joints1)) + "," +
                                  addView(buffer, weights1, sizeof(weights1)) + "," + addView(buffer, inverseBinds.data(), inverseBinds.size() * sizeof(float)) + "," +
                                  addView(buffer, times, sizeof(times)) + "," + addView(buffer, rotations, sizeof(rotations));
        return importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                                                      {"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},
                                                      {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
                                                      {"bufferView":3,"componentType":5121,"count":3,"type":"VEC4"},
                                                      {"bufferView":4,"componentType":5126,"count":3,"type":"VEC4"},
                                                      {"bufferView":5,"componentType":5126,"count":5,"type":"MAT4"},
                                                      {"bufferView":6,"componentType":5126,"count":2,"type":"SCALAR"},
                                                      {"bufferView":7,"componentType":5126,"count":2,"type":"VEC4"}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2,"JOINTS_1":3,"WEIGHTS_1":4}}]}],
            "skins":[{"joints":[1,2,3,4,5],"inverseBindMatrices":5}],
            "nodes":[{"mesh":0,"skin":0,"translation":[5,0,0]},{"children":[2]},{"translation":[0,1,0],"children":[3,4,5]},{},{},{}],
            "scenes":[{"nodes":[0,1]}])" + std::string(withClip ? R"(,
            "animations":[{"samplers":[{"input":6,"output":7}],"channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}]}])"
                                                                : ""));
    }

    // A kept node's world matrix at rest: its rest matrix after its parents', as the Animate pass walks them.
    glm::mat4 getRestWorld(const rasm::AnimationRaw &animation, int32_t node)
    {
        glm::mat4 world = animation.nodes[node].rest;
        for (int32_t parent = animation.nodes[node].parent; parent >= 0; parent = animation.nodes[parent].parent)
            world = animation.nodes[parent].rest * world;
        return world;
    }

    uint32_t sumWeights(const rasm::SkinVertex &vertex)
    {
        return uint32_t{vertex.weights[0]} + vertex.weights[1] + vertex.weights[2] + vertex.weights[3];
    }
}

TEST_CASE("a skinned primitive keeps its 4 largest weights, renormalized, and ignores its node's matrix")
{
    const rasm::GLTFRaw raw = importSkinnedTriangle();
    REQUIRE(raw.subGltfRaws.size() == 1);
    const rasm::SubGLTFRaw &sub = raw.subGltfRaws[0];
    CHECK(sub.transform == glm::mat4(1.0f));
    CHECK(sub.animatedNode == -1);
    CHECK(sub.jointCount == 5);
    CHECK(sub.firstJoint == 0);
    REQUIRE(sub.objRaw.skin.size() == 3);

    // 0.5 and 0.25 become 2/3 and 1/3.
    const rasm::SkinVertex &first = sub.objRaw.skin[0];
    CHECK(sumWeights(first) == 65535);
    CHECK(first.joints[0] == 0);
    CHECK(first.weights[0] == 43690);
    CHECK(first.joints[1] == 1);
    CHECK(first.weights[1] == 21845);

    // Joint 7 is past the skin's five joints, so its weight goes.
    const rasm::SkinVertex &second = sub.objRaw.skin[1];
    CHECK(second.joints[0] == 0);
    CHECK(second.weights[0] == 65535);
    CHECK(second.weights[1] == 0);

    // Joint 1 is in both sets: 0.2 + 0.05. Then joints 0 (0.3), 1 (0.25), 4 (0.22) and 3 (0.15) stay and joint 2
    // (0.1) goes; they add up to 0.92 before they are scaled.
    const rasm::SkinVertex &third = sub.objRaw.skin[2];
    CHECK(sumWeights(third) == 65535);
    CHECK(third.joints[0] == 0);
    CHECK(third.joints[1] == 1);
    CHECK(third.joints[2] == 4);
    CHECK(third.joints[3] == 3);
    CHECK(third.weights[0] == doctest::Approx(65535.0 * 0.3 / 0.92).epsilon(0.001));
    CHECK(third.weights[1] == doctest::Approx(65535.0 * 0.25 / 0.92).epsilon(0.001));
    CHECK(third.weights[3] == doctest::Approx(65535.0 * 0.15 / 0.92).epsilon(0.001));
}

TEST_CASE("a skin's joints are kept nodes, moved or not, with their inverse bind matrices")
{
    const rasm::GLTFRaw raw = importSkinnedTriangle();
    // Node 2 turns and the other joints are still; the mesh's own node is not kept.
    REQUIRE(raw.animation.nodes.size() == 5);
    CHECK(raw.animation.nodes[0].parent == -1);
    CHECK(raw.animation.nodes[1].parent == 0);
    CHECK(raw.animation.nodes[4].parent == 1);
    REQUIRE(raw.animation.joints.size() == 5);
    CHECK(raw.animation.joints[0].node == 0);
    CHECK(raw.animation.joints[1].node == 1);
    CHECK(raw.animation.joints[4].node == 4);
    CHECK(raw.animation.joints[1].inverseBind[3] == glm::vec4(0, -1, 0, 1));
}

TEST_CASE("a joint's sphere holds every vertex it moves, and a joint that moves none has none")
{
    const std::vector<rasm::Vertex> vertices = {{.pos = glm::vec3(0, 0, 0)}, {.pos = glm::vec3(2, 0, 0)}, {.pos = glm::vec3(0, 4, 0)}};
    std::vector<rasm::SkinVertex> skin(3);
    skin[0] = {.joints = {0, 0, 0, 0}, .weights = {65535, 0, 0, 0}};
    skin[1] = {.joints = {0, 2, 0, 0}, .weights = {30000, 35535, 0, 0}};
    skin[2] = {.joints = {2, 0, 0, 0}, .weights = {65535, 0, 0, 0}};
    const std::vector<glm::vec4> spheres = rasm::getJointSpheres(vertices, skin, 3);
    REQUIRE(spheres.size() == 3);
    CHECK(spheres[0] == glm::vec4(1, 0, 0, 1));
    CHECK(spheres[1].w < 0.0f);
    CHECK(glm::vec3(spheres[2]) == glm::vec3(1, 2, 0));
    CHECK(spheres[2].w == doctest::Approx(glm::sqrt(5.0f)));
}

TEST_CASE("the pool data has the skinned sub-mesh, the joints' inverse bind rows and the spheres")
{
    rasm::AnimationRaw animation;
    animation.nodes.resize(2);
    animation.nodes[1].parent = 0;
    animation.joints.push_back({.node = 0});
    animation.joints.push_back({.node = 1, .inverseBind = glm::mat4(glm::vec4(1, 0, 0, 0), glm::vec4(0, 1, 0, 0), glm::vec4(0, 0, 1, 0), glm::vec4(3, 0, 0, 1))});
    const rasm::gfx::AnimationSubMeshData subMeshes[] = {{.node = 1}, {.jointCount = 2, .firstSphere = 0, .vertexCount = 100, .firstSkinnedVertex = 0}};
    const glm::vec4 spheres[] = {glm::vec4(0, 0, 0, 1), glm::vec4(0, 1, 0, -1)};
    const uint64_t address = 0x20000;
    const std::vector<uint8_t> bytes = rasm::buildAnimationPoolData(animation, subMeshes, 2, spheres, 2, address);
    REQUIRE(bytes.size() == rasm::getAnimationPoolBytes(animation, 2, 2));

    rasm::gfx::AnimationMeshData header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    CHECK(header.jointCount == 2);
    CHECK(header.subMeshCount == 2);
    CHECK(header.skinnedVertexCount == 100);
    CHECK(header.joints % 16 == 0);

    rasm::gfx::AnimationJointData joint;
    std::memcpy(&joint, bytes.data() + (header.joints - address) + sizeof(joint), sizeof(joint));
    CHECK(joint.node == 1);
    // The rows of the matrix: the translation is the last column.
    CHECK(joint.inverseBind[0] == glm::vec4(1, 0, 0, 3));
    glm::vec4 sphere;
    std::memcpy(&sphere, bytes.data() + (header.jointSpheres - address) + sizeof(sphere), sizeof(sphere));
    CHECK(sphere == glm::vec4(0, 1, 0, -1));
}

TEST_CASE("at rest, a joint matrix is the identity when the bind pose is the rest pose, also without clips")
{
    for (const bool withClip : {true, false})
    {
        const rasm::GLTFRaw raw = importSkinnedTriangle(withClip);
        REQUIRE(raw.subGltfRaws[0].jointCount == 5);
        REQUIRE(raw.animation.joints.size() == 5);
        CHECK(raw.animation.clips.size() == (withClip ? 1u : 0u));
        for (const rasm::AnimationJoint &joint : raw.animation.joints)
        {
            const glm::mat4 jointMatrix = getRestWorld(raw.animation, static_cast<int32_t>(joint.node)) * joint.inverseBind;
            for (int c = 0; c < 4; ++c)
            {
                for (int r = 0; r < 4; ++r)
                    CHECK(jointMatrix[c][r] == doctest::Approx(c == r ? 1.0f : 0.0f));
            }
        }
    }
}
