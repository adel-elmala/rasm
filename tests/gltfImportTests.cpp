#include "importMeshText.h"

#include "doctest/doctest.h"
#include "platform/paths.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
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
    CHECK_FALSE(rasm::importMeshFile(rasm::pathToUtf8(std::filesystem::temp_directory_path() / "rasm_missing.gltf"), 0).loaded);
    CHECK_FALSE(importMeshText("not json", ".gltf").loaded);
}

TEST_CASE("vertex colors pack as RGBA8 with sRGB-encoded RGB and linear alpha")
{
    CHECK(rasm::packVertexColor(glm::vec4(1.0f)) == 0xFFFFFFFFu);
    CHECK(rasm::packVertexColor(glm::vec4(0.0f)) == 0u);
    // Linear 0.5 is sRGB 0.7354, so 188; alpha 0.5 stays linear, so 128. Values outside [0, 1] are clamped.
    CHECK(rasm::packVertexColor(glm::vec4(0.5f, 0.0f, 2.0f, 0.5f)) == (188u | 0u << 8 | 255u << 16 | 128u << 24));
}

TEST_CASE("glTF COLOR_0 is read into the color stream, and a primitive without it has none")
{
    // VEC4 normalized unsigned bytes, VEC3 normalized unsigned shorts (alpha 1) and VEC4 floats.
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint8_t bytes[] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0};
    const uint16_t shorts[] = {65535, 0, 0, 0, 65535, 0, 0, 0, 65535};
    const float floats[] = {1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0.25f};
    std::vector<unsigned char> buffer;
    const std::string views = addView(buffer, positions, sizeof(positions)) + "," + addView(buffer, bytes, sizeof(bytes)) + "," +
                              addView(buffer, shorts, sizeof(shorts)) + "," + addView(buffer, floats, sizeof(floats));
    const rasm::GLTFRaw raw = importGltf(buffer, views, R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                                                                        {"bufferView":1,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},
                                                                        {"bufferView":2,"componentType":5123,"normalized":true,"count":3,"type":"VEC3"},
                                                                        {"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"}],
                                                           "meshes":[{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1}},
                                                                                    {"attributes":{"POSITION":0,"COLOR_0":2}},
                                                                                    {"attributes":{"POSITION":0,"COLOR_0":3}},
                                                                                    {"attributes":{"POSITION":0}}]}],
                                                           "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}])");

    REQUIRE(raw.subGltfRaws.size() == 4);
    CHECK(raw.subGltfRaws[0].objRaw.colors == std::vector<uint32_t>{0xFF0000FFu, 0x8000FF00u, 0x00FF0000u});
    CHECK(raw.subGltfRaws[1].objRaw.colors == std::vector<uint32_t>{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u});
    CHECK(raw.subGltfRaws[2].objRaw.colors == std::vector<uint32_t>{0xFFFFFFFFu, 0xFF000000u, 0x400000FFu});
    CHECK(raw.subGltfRaws[3].objRaw.colors.empty());
}

TEST_CASE("KHR_mesh_quantization: integer positions, normals and UVs decode, and the file may require the extension")
{
    // Non-normalized short positions (dequantized by the node's scale), normalized byte normals and normalized
    // unsigned-short UVs.
    const int16_t positions[] = {0, 0, 0, 0, 100, 0, 0, 0, 0, 0, 100, 0};
    const int8_t normals[] = {0, 0, 127, 0, 0, 0, -127, 0, 0, 127, 0, 0};
    const uint16_t uvs[] = {0, 0, 65535, 0, 0, 65535};
    std::vector<unsigned char> buffer;
    const std::string views = addView(buffer, positions, sizeof(positions)).insert(1, "\"byteStride\":8,") + "," +
                              addView(buffer, normals, sizeof(normals)).insert(1, "\"byteStride\":4,") + "," + addView(buffer, uvs, sizeof(uvs));
    const rasm::GLTFRaw raw = importGltf(buffer, views, R"("extensionsUsed":["KHR_mesh_quantization"],"extensionsRequired":["KHR_mesh_quantization"],
                                                           "accessors":[{"bufferView":0,"componentType":5122,"count":3,"type":"VEC3"},
                                                                        {"bufferView":1,"componentType":5120,"normalized":true,"count":3,"type":"VEC3"},
                                                                        {"bufferView":2,"componentType":5123,"normalized":true,"count":3,"type":"VEC2"}],
                                                           "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2}}]}],
                                                           "nodes":[{"mesh":0,"scale":[0.01,0.01,0.01]}],"scenes":[{"nodes":[0]}])");

    REQUIRE(raw.loaded);
    REQUIRE(raw.subGltfRaws.size() == 1);
    const std::vector<rasm::Vertex> &vertices = raw.subGltfRaws[0].objRaw.vertices;
    REQUIRE(vertices.size() == 3);
    CHECK(vertices[1].pos == glm::vec3(100, 0, 0));
    CHECK(isNear(glm::vec3(raw.subGltfRaws[0].transform * glm::vec4(vertices[1].pos, 1.0f)), {1, 0, 0}));
    CHECK(isNear(vertices[0].normal, {0, 0, 1}));
    CHECK(isNear(vertices[1].normal, {0, 0, -1}));
    CHECK(vertices[1].uv[0] == glm::vec2(1, 0));
    CHECK(vertices[2].uv[0] == glm::vec2(0, 1));
}

TEST_CASE("a glTF that requires an unsupported extension does not load, one that only uses it does")
{
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<unsigned char> buffer;
    const std::string views = addView(buffer, positions, sizeof(positions));
    const std::string rest = R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],
                                "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}])";

    CHECK_FALSE(importGltf(buffer, views, R"("extensionsUsed":["EXT_meshopt_compression"],"extensionsRequired":["EXT_meshopt_compression"],)" + rest).loaded);
    CHECK(importGltf(buffer, views, R"("extensionsUsed":["EXT_meshopt_compression","KHR_materials_sheen"],)" + rest).loaded);
    CHECK(importGltf(buffer, views, R"("extensionsUsed":["KHR_texture_transform"],"extensionsRequired":["KHR_texture_transform"],)" + rest).loaded);
}
