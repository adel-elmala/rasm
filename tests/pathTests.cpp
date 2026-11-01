#include "rasm/engine.h"

#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    class ScopedTempDir
    {
    public:
        ScopedTempDir() : dir(fs::temp_directory_path() / "rasm_path_tests")
        {
            fs::remove_all(dir);
            fs::create_directories(dir);
        }
        ~ScopedTempDir() { fs::remove_all(dir); }

        const fs::path dir;
    };

    class ScopedWorkingDir
    {
    public:
        explicit ScopedWorkingDir(const fs::path &dir) : previous(fs::current_path()) { fs::current_path(dir); }
        ~ScopedWorkingDir() { fs::current_path(previous); }

    private:
        const fs::path previous;
    };

    bool isSamePath(const std::string &a, const fs::path &b)
    {
        return fs::path(a) == b.lexically_normal();
    }

    void writeTriangleObj(const fs::path &path)
    {
        std::ofstream file(path);
        file << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }

    fs::path defaultShaderDir()
    {
        rasm::Engine engine(rasm::EngineConfig{});
        return engine.getConfig().shaderDir;
    }
}

TEST_CASE("shaders default to a folder beside the executable")
{
    CHECK(defaultShaderDir() == fs::path(RASM_TEST_EXECUTABLE_DIR) / "shaders");
}

TEST_CASE("the default shader directory ignores the working directory")
{
    const fs::path expected = defaultShaderDir();
    ScopedTempDir temp;
    ScopedWorkingDir workingDir(temp.dir);

    CHECK(defaultShaderDir() == expected);
}

TEST_CASE("a relative shader directory resolves against the executable")
{
    ScopedTempDir temp;
    ScopedWorkingDir workingDir(temp.dir);
    rasm::EngineConfig config;
    config.shaderDir = "data/../custom";

    rasm::Engine engine(config);

    CHECK(isSamePath(engine.getConfig().shaderDir, fs::path(RASM_TEST_EXECUTABLE_DIR) / "custom"));
}

TEST_CASE("an absolute shader directory is kept")
{
    ScopedTempDir temp;
    rasm::EngineConfig config;
    config.shaderDir = temp.dir.string();

    rasm::Engine engine(config);

    CHECK(isSamePath(engine.getConfig().shaderDir, temp.dir));
}

TEST_CASE("without an asset root, paths stay as given")
{
    rasm::Engine engine(rasm::EngineConfig{});

    CHECK(engine.resolveAssetPath("models/a.obj") == "models/a.obj");
}

TEST_CASE("relative asset paths resolve against the asset root")
{
    ScopedTempDir temp;
    rasm::EngineConfig config;
    config.assetRoot = temp.dir.string();

    rasm::Engine engine(config);

    CHECK(isSamePath(engine.resolveAssetPath("models/a.obj"), temp.dir / "models/a.obj"));
}

TEST_CASE("absolute asset paths ignore the asset root")
{
    ScopedTempDir temp;
    rasm::EngineConfig config;
    config.assetRoot = (temp.dir / "root").string();

    rasm::Engine engine(config);

    CHECK(isSamePath(engine.resolveAssetPath((temp.dir / "other.obj").string()), temp.dir / "other.obj"));
}

TEST_CASE("a relative asset root resolves against the executable")
{
    ScopedTempDir temp;
    ScopedWorkingDir workingDir(temp.dir);
    rasm::EngineConfig config;
    config.assetRoot = "assets";

    rasm::Engine engine(config);

    CHECK(isSamePath(engine.getConfig().assetRoot, fs::path(RASM_TEST_EXECUTABLE_DIR) / "assets"));
}

TEST_CASE("meshes load through the asset root from any working directory")
{
    ScopedTempDir temp;
    fs::create_directories(temp.dir / "root/models");
    fs::create_directories(temp.dir / "elsewhere");
    writeTriangleObj(temp.dir / "root/models/triangle.obj");
    ScopedWorkingDir workingDir(temp.dir / "elsewhere");

    rasm::EngineConfig config;
    config.assetRoot = (temp.dir / "root").string();
    rasm::Engine engine(config);

    CHECK(engine.loadMesh("models/triangle.obj").isValid());
}

TEST_CASE("relative and absolute paths to one file share a mesh")
{
    ScopedTempDir temp;
    writeTriangleObj(temp.dir / "triangle.obj");
    rasm::EngineConfig config;
    config.assetRoot = temp.dir.string();
    rasm::Engine engine(config);

    auto relative = engine.loadMesh("triangle.obj");
    auto absolute = engine.loadMesh((temp.dir / "triangle.obj").string());

    REQUIRE(relative.isValid());
    CHECK(relative == absolute);
}
