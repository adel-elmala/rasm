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
    // A triangle skinned by a skin of two joints (nodes 1 and 2), with two sets of influences, and a clip that turns
    // joint 2. Vertex 0 has weights that add up to 0.75, vertex 1 names joint 7, which the skin does not have, and
    // vertex 2 has five influences.
    rasm::GLTFRaw importSkinnedTriangle()
    {
        const float positions[] = {0, 0, 0, 1, 0, 0, 0, 2, 0};
        const uint8_t joints0[] = {0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1};
        const float weights0[] = {0.5f, 0.25f, 0, 0, 1, 0, 0, 0, 0.3f, 0.2f, 0, 0.1f};
        const uint8_t joints1[] = {0, 0, 0, 0, 7, 0, 0, 0, 1, 0, 0, 0};
        const float weights1[] = {0, 0, 0, 0, 0.5f, 0, 0, 0, 0.25f, 0.15f, 0, 0};
        const float inverseBinds[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
        const float times[] = {0.0f, 1.0f};
        const float rotations[] = {0, 0, 0, 1, 0, 0, 0.7071068f, 0.7071068f};
        std::vector<unsigned char> buffer;
        const std::string views = addView(buffer, positions, sizeof(positions)) + "," + addView(buffer, joints0, sizeof(joints0)) + "," +
                                  addView(buffer, weights0, sizeof(weights0)) + "," + addView(buffer, joints1, sizeof(joints1)) + "," +
                                  addView(buffer, weights1, sizeof(weights1)) + "," + addView(buffer, inverseBinds, sizeof(inverseBinds)) + "," +
                                  addView(buffer, times, sizeof(times)) + "," + addView(buffer, rotations, sizeof(rotations));
        return importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                                                      {"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},
                                                      {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
                                                      {"bufferView":3,"componentType":5121,"count":3,"type":"VEC4"},
                                                      {"bufferView":4,"componentType":5126,"count":3,"type":"VEC4"},
                                                      {"bufferView":5,"componentType":5126,"count":2,"type":"MAT4"},
                                                      {"bufferView":6,"componentType":5126,"count":2,"type":"SCALAR"},
                                                      {"bufferView":7,"componentType":5126,"count":2,"type":"VEC4"}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2,"JOINTS_1":3,"WEIGHTS_1":4}}]}],
            "skins":[{"joints":[1,2],"inverseBindMatrices":5}],
            "nodes":[{"mesh":0,"skin":0,"translation":[5,0,0]},{"children":[2]},{"translation":[0,1,0]}],
            "scenes":[{"nodes":[0,1]}],
            "animations":[{"samplers":[{"input":6,"output":7}],"channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}]}])");
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
    CHECK(sub.jointCount == 2);
    CHECK(sub.firstJoint == 0);
    REQUIRE(sub.objRaw.skin.size() == 3);

    // 0.5 and 0.25 become 2/3 and 1/3.
    const rasm::SkinVertex &first = sub.objRaw.skin[0];
    CHECK(sumWeights(first) == 65535);
    CHECK(first.joints[0] == 0);
    CHECK(first.weights[0] == 43690);
    CHECK(first.joints[1] == 1);
    CHECK(first.weights[1] == 21845);

    // Joint 7 is past the skin's two joints, so its weight goes.
    const rasm::SkinVertex &second = sub.objRaw.skin[1];
    CHECK(second.joints[0] == 0);
    CHECK(second.weights[0] == 65535);
    CHECK(second.weights[1] == 0);

    // 0.3, 0.25, 0.2 and 0.15 stay, 0.1 goes; they add up to 0.9 before they are scaled.
    const rasm::SkinVertex &third = sub.objRaw.skin[2];
    CHECK(sumWeights(third) == 65535);
    CHECK(third.weights[0] == doctest::Approx(65535.0 * 0.3 / 0.9).epsilon(0.001));
    CHECK(third.weights[3] == doctest::Approx(65535.0 * 0.15 / 0.9).epsilon(0.001));
    CHECK(third.joints[0] == 0);
    CHECK(third.joints[1] == 1);
}

TEST_CASE("a skin's joints are kept nodes, moved or not, with their inverse bind matrices")
{
    const rasm::GLTFRaw raw = importSkinnedTriangle();
    // Joint 1 is still and joint 2 turns; the mesh's own node is not kept.
    REQUIRE(raw.animation.nodes.size() == 2);
    CHECK(raw.animation.nodes[0].parent == -1);
    CHECK(raw.animation.nodes[1].parent == 0);
    REQUIRE(raw.animation.joints.size() == 2);
    CHECK(raw.animation.joints[0].node == 0);
    CHECK(raw.animation.joints[1].node == 1);
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
