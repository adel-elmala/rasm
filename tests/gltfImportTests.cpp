#include "assets/meshImport.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    // Appends `size` bytes to the buffer, 4-byte aligned, and returns the glTF bufferView that covers them.
    std::string addView(std::vector<unsigned char> &buffer, const void *data, size_t size)
    {
        buffer.resize((buffer.size() + 3) / 4 * 4, 0);
        const size_t offset = buffer.size();
        const unsigned char *bytes = static_cast<const unsigned char *>(data);
        buffer.insert(buffer.end(), bytes, bytes + size);
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(size) + "}";
    }

    std::string base64(const std::vector<unsigned char> &bytes)
    {
        const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3)
        {
            const uint32_t b0 = bytes[i];
            const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
            const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
            const uint32_t triple = b0 << 16 | b1 << 8 | b2;
            out += alphabet[triple >> 18 & 63];
            out += alphabet[triple >> 12 & 63];
            out += i + 1 < bytes.size() ? alphabet[triple >> 6 & 63] : '=';
            out += i + 2 < bytes.size() ? alphabet[triple & 63] : '=';
        }
        return out;
    }

    rasm::GLTFRaw importText(const std::string &json)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "rasm_gltf_import_tests.gltf";
        {
            std::ofstream file(path);
            file << json;
        }
        rasm::GLTFRaw raw = rasm::importMeshFile(path.string());

        std::filesystem::remove(path);
        return raw;
    }

    // `rest` holds the accessors, meshes, nodes and scenes; the buffer is embedded as a data URI.
    rasm::GLTFRaw importGltf(const std::vector<unsigned char> &buffer, const std::string &views, const std::string &rest)
    {
        return importText("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
                          ",\"uri\":\"data:application/octet-stream;base64," + base64(buffer) + "\"}],\"bufferViews\":[" + views + "]," + rest + "}");
    }

    rasm::GLTFRaw importTriangleMesh(const std::string &primitives)
    {
        const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
        const uint16_t indices[] = {0, 1, 2, 5};
        std::vector<unsigned char> buffer;
        const std::string views = addView(buffer, positions, sizeof(positions)) + "," + addView(buffer, indices, sizeof(indices));
        return importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},
                                                         {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
                                                         {"bufferView":1,"componentType":5123,"count":4,"type":"SCALAR"}],
                                            "meshes":[{"primitives":[)" +
                                             primitives + R"(]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}])");
    }

    bool isNear(const glm::vec3 &a, const glm::vec3 &b)
    {
        return glm::length(a - b) < 1e-5f;
    }
}

TEST_CASE("an indexed glTF triangle imports its vertices, indices and a generated normal")
{
    const rasm::GLTFRaw raw = importTriangleMesh(R"({"attributes":{"POSITION":0},"indices":1,"material":2})");

    REQUIRE(raw.loaded);
    REQUIRE(raw.subGltfRaws.size() == 1);
    const rasm::SubGLTFRaw &sub = raw.subGltfRaws[0];
    CHECK(sub.materialIndex == 2);
    CHECK(sub.transform == glm::mat4(1.0f));
    REQUIRE(sub.objRaw.vertices.size() == 4);
    CHECK(sub.objRaw.vertices[1].pos == glm::vec3(1, 0, 0));
    CHECK(sub.objRaw.indices == std::vector<uint32_t>{0, 1, 2});
    CHECK(isNear(sub.objRaw.vertices[0].normal, {0, 0, 1}));
    CHECK(sub.objRaw.vertices[0].tangent.w == 0.0f);
}

TEST_CASE("a glTF primitive without indices draws its vertices in order")
{
    const rasm::GLTFRaw raw = importTriangleMesh(R"({"attributes":{"POSITION":0}})");

    REQUIRE(raw.subGltfRaws.size() == 1);
    CHECK(raw.subGltfRaws[0].objRaw.indices == std::vector<uint32_t>{0, 1, 2});
    CHECK(raw.subGltfRaws[0].materialIndex == -1);
}

TEST_CASE("glTF triangle strips and fans become triangle lists with the same winding")
{
    const rasm::GLTFRaw strip = importTriangleMesh(R"({"attributes":{"POSITION":0},"mode":5})");
    const rasm::GLTFRaw fan = importTriangleMesh(R"({"attributes":{"POSITION":0},"mode":6})");

    REQUIRE(strip.subGltfRaws.size() == 1);
    CHECK(strip.subGltfRaws[0].objRaw.indices == std::vector<uint32_t>{0, 1, 2, 1, 3, 2});
    REQUIRE(fan.subGltfRaws.size() == 1);
    CHECK(fan.subGltfRaws[0].objRaw.indices == std::vector<uint32_t>{1, 2, 0, 2, 3, 0});
}

TEST_CASE("glTF primitives that cannot be drawn are skipped and the rest still load")
{
    // Index 5 is past the 4 vertices; mode 0 is POINTS.
    const rasm::GLTFRaw raw = importTriangleMesh(R"({"attributes":{"POSITION":0},"indices":2},
                                                    {"attributes":{"POSITION":0},"mode":0},
                                                    {"attributes":{"POSITION":0},"indices":1})");

    REQUIRE(raw.loaded);
    REQUIRE(raw.subGltfRaws.size() == 1);
    CHECK(raw.subGltfRaws[0].objRaw.indices == std::vector<uint32_t>{0, 1, 2});
}

TEST_CASE("glTF node transforms combine parent first")
{
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<unsigned char> buffer;
    const std::string views = addView(buffer, positions, sizeof(positions));
    const rasm::GLTFRaw raw = importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],
                                                           "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
                                                           "nodes":[{"translation":[1,2,3],"children":[1]},{"scale":[2,2,2],"mesh":0}],
                                                           "scenes":[{"nodes":[0]}])");

    REQUIRE(raw.subGltfRaws.size() == 1);
    const glm::mat4 &transform = raw.subGltfRaws[0].transform;
    CHECK(isNear(glm::vec3(transform * glm::vec4(1, 0, 0, 1)), {3, 2, 3}));
}

TEST_CASE("a glTF node cycle does not hang the import")
{
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<unsigned char> buffer;
    const std::string views = addView(buffer, positions, sizeof(positions));
    const rasm::GLTFRaw raw = importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],
                                                           "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
                                                           "nodes":[{"mesh":0,"children":[1]},{"mesh":0,"children":[0]}],
                                                           "scenes":[{"nodes":[0]}])");

    REQUIRE(raw.loaded);
    CHECK(raw.subGltfRaws.size() == 2);
}

TEST_CASE("glTF interleaved, quantized and sparse attributes decode to floats")
{
    // Interleaved POSITION + NORMAL (stride 24), normalized unsigned-byte UVs, and TANGENT with an unsnapped w.
    const float interleaved[] = {0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
    const uint8_t uvs[] = {255, 0, 0, 255, 51, 102};
    const float tangents[] = {1, 0, 0, 0.5f, 1, 0, 0, -0.25f, 1, 0, 0, 1};
    const uint8_t sparseIndices[] = {2};
    const float sparseValues[] = {5, 6};
    std::vector<unsigned char> buffer;
    const std::string views = addView(buffer, interleaved, sizeof(interleaved)).insert(1, "\"byteStride\":24,") + "," +
                              addView(buffer, uvs, sizeof(uvs)) + "," + addView(buffer, tangents, sizeof(tangents)) + "," +
                              addView(buffer, sparseIndices, sizeof(sparseIndices)) + "," + addView(buffer, sparseValues, sizeof(sparseValues));
    const rasm::GLTFRaw raw = importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                                                                        {"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC3"},
                                                                        {"bufferView":1,"componentType":5121,"normalized":true,"count":3,"type":"VEC2"},
                                                                        {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
                                                                        {"componentType":5126,"count":3,"type":"VEC2",
                                                                         "sparse":{"count":1,"indices":{"bufferView":3,"componentType":5121},
                                                                                   "values":{"bufferView":4}}}],
                                                           "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2,
                                                                                                   "TEXCOORD_1":4,"TANGENT":3}}]}],
                                                           "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}])");

    REQUIRE(raw.subGltfRaws.size() == 1);
    const std::vector<rasm::Vertex> &vertices = raw.subGltfRaws[0].objRaw.vertices;
    REQUIRE(vertices.size() == 3);
    CHECK(vertices[1].pos == glm::vec3(1, 0, 0));
    CHECK(vertices[2].normal == glm::vec3(0, 1, 0));
    CHECK(vertices[0].uv[0] == glm::vec2(1, 0));
    CHECK(vertices[2].uv[0].x == doctest::Approx(0.2f));
    CHECK(vertices[2].uv[0].y == doctest::Approx(0.4f));
    CHECK(vertices[0].tangent.w == 1.0f);
    CHECK(vertices[1].tangent.w == -1.0f);
    // TEXCOORD_1 has no bufferView, so it is all zeros except the sparse element 2.
    CHECK(vertices[1].uv[1] == glm::vec2(0, 0));
    CHECK(vertices[2].uv[1] == glm::vec2(5, 6));
}

TEST_CASE("a missing glTF file does not load")
{
    CHECK_FALSE(rasm::importMeshFile((std::filesystem::temp_directory_path() / "rasm_missing.gltf").string()).loaded);
    CHECK_FALSE(importText("not json").loaded);
}
