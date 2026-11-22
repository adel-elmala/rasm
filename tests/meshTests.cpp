#include "importMeshText.h"

#include "rasm/engine.h"
#include "platform/paths.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t MESH_FILES = 12;

    // A folder of small OBJ files, removed with it. The random name keeps test processes that run at once apart.
    struct ObjFolder
    {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / ("rasm_tests_meshes_" + std::to_string(std::random_device{}()));
        std::vector<std::string> paths;

        explicit ObjFolder(uint32_t count)
        {
            std::filesystem::create_directories(dir);
            for (uint32_t i = 0; i < count; ++i)
            {
                const std::filesystem::path path = dir / ("mesh" + std::to_string(i) + ".obj");
                std::ofstream(path) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
                paths.push_back(rasm::pathToUtf8(path));
            }
        }

        ~ObjFolder()
        {
            std::error_code error;
            std::filesystem::remove_all(dir, error);
        }
    };
}

TEST_CASE("an OBJ can mix faces with and without normals and texture coordinates")
{
    CHECK(importMeshText("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1\nf 1 2 3\n", ".obj").loaded);
}

TEST_CASE("an OBJ can name normals and texture coordinates it never defines")
{
    CHECK(importMeshText("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1/1 2/2/2 3/3/3\n", ".obj").loaded);
}

// The engines below have no GPU (zero window dimensions) and run no frame, so no load finishes, but a file gets its
// handle at the call. Before D33 the 8th call returned an invalid handle.
TEST_CASE("an engine gives a mesh handle to every file it loads, with no cap on the count")
{
    const ObjFolder folder(MESH_FILES);
    rasm::Engine engine(rasm::EngineConfig{.cacheDir = ""});
    std::vector<rasm::MeshHandle> handles;
    for (const std::string &path : folder.paths)
    {
        const rasm::MeshHandle handle = engine.loadMesh(path);
        REQUIRE(handle.isValid());
        for (const rasm::MeshHandle &other : handles)
            CHECK_FALSE(other == handle);
        handles.push_back(handle);
    }
    CHECK(engine.loadMesh(folder.paths[0]) == handles[0]);
}

TEST_CASE("a mesh that does not fit the geometry pool gets no handle and uses no index")
{
    const ObjFolder folder(1);
    rasm::Engine engine(rasm::EngineConfig{.cacheDir = ""});
    // Without a GPU the geometry pool has no room at all.
    const std::vector<rasm::Vertex> vertices(3, rasm::Vertex{});
    CHECK_FALSE(engine.createMesh(vertices, {0, 1, 2}).isValid());
    CHECK_FALSE(engine.createMesh(vertices, {0, 1, 2}).isValid());
    const rasm::MeshHandle loaded = engine.loadMesh(folder.paths[0]);
    REQUIRE(loaded.isValid());
    CHECK(loaded.index == 1);
}
