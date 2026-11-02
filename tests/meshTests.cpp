#include "rasm/engine.h"

#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    bool objLoads(const std::string &contents)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "rasm_mesh_tests.obj";
        {
            std::ofstream file(path);
            file << contents;
        }
        rasm::Engine engine(rasm::EngineConfig{});

        const bool loaded = engine.loadMesh(path.string()).isValid();

        std::filesystem::remove(path);
        return loaded;
    }
}

TEST_CASE("an OBJ can mix faces with and without normals and texture coordinates")
{
    CHECK(objLoads("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1\nf 1 2 3\n"));
}

TEST_CASE("an OBJ can name normals and texture coordinates it never defines")
{
    CHECK(objLoads("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1/1 2/2/2 3/3/3\n"));
}
