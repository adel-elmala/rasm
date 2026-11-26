#include "renderer/shaderCompiler.h"
#include "platform/paths.h"

#include "doctest/doctest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    using Target = rasm::ShaderCompiler::Target;

    // A shader folder of its own (lib/ with one include, and passes/rasm.slang, which initialize looks for) and a
    // cache folder beside it.
    struct ShaderFolders
    {
        ShaderFolders() : root(fs::temp_directory_path() / ("rasm_shader_cache_tests_" + std::to_string(nextId++)))
        {
            fs::remove_all(root);
            fs::create_directories(root / "shaders" / "lib");
            fs::create_directories(root / "shaders" / "passes");
            write(root / "shaders" / "passes" / "rasm.slang", "");
            write(root / "shaders" / "lib" / "common.slang", "#ifndef SCALE\n#define SCALE 2\n#endif\nuint scale(uint x) { return x * SCALE; }\n");
        }
        ~ShaderFolders()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }

        static void write(const fs::path &path, const std::string &text)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << text;
        }

        std::string shaderDir() const { return rasm::pathToUtf8(root / "shaders"); }
        std::string cacheDir() const { return rasm::pathToUtf8(root / "cache"); }

        uint32_t entries() const
        {
            std::error_code error;
            uint32_t count = 0;
            for ([[maybe_unused]] const auto &entry : fs::directory_iterator(root / "cache" / "shaders", error))
                ++count;
            return count;
        }

        const fs::path root;
        static inline uint32_t nextId = 0;
    };

    constexpr const char *SOURCE = R"(#include "common.slang"
struct Item
{
    float4 value;
    uint count;
};
struct TestPushConstants
{
    Item *items;
    uint2 padding;
};
[[vk::push_constant]]
ConstantBuffer<TestPushConstants> pc;

[shader("compute")]
[numthreads(1, 1, 1)]
void mainA(uint3 id : SV_DispatchThreadID)
{
    pc.items[id.x].count = scale(id.x);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void mainB(uint3 id : SV_DispatchThreadID)
{
    pc.items[id.x].value = float4(1.0, 2.0, 3.0, 4.0);
}
)";

    // A new compiler for each run, as a new start of the engine would make.
    struct Run
    {
        rasm::ShaderCompiler compiler;
        std::vector<uint8_t> code;

        Run(const ShaderFolders &folders, const std::string &source, const char *entryPoint, const std::string &cacheDir)
        {
            REQUIRE(compiler.initialize(Target::SPIRV, folders.shaderDir(), cacheDir));
            code = compiler.compileFromString(source, "test.comp.slang", entryPoint).code;
            REQUIRE_FALSE(code.empty());
        }

        bool hit() const { return compiler.getStats().cacheHits == 1 && compiler.getStats().compiles == 0; }
        bool compiled() const { return compiler.getStats().cacheHits == 0 && compiler.getStats().compiles == 1; }
    };
}

TEST_CASE("a second start reads the shader from the cache, with the same code and struct layouts")
{
    ShaderFolders folders;
    Run first(folders, SOURCE, "mainA", folders.cacheDir());
    CHECK(first.compiled());
    CHECK(folders.entries() == 1);

    Run second(folders, SOURCE, "mainA", folders.cacheDir());
    CHECK(second.hit());
    CHECK(second.code == first.code);

    // The inspector reads the layouts Slang reported, so a hit has them too.
    const std::vector<rasm::StructField> *item = second.compiler.findStructLayout("Item");
    REQUIRE(item != nullptr);
    REQUIRE(item->size() == 2);
    CHECK((*item)[0].name == "value");
    CHECK((*item)[0].offset == 0);
    CHECK((*item)[1].name == "count");
    CHECK((*item)[1].offset == 16);
}

TEST_CASE("every input of the SPIR-V is in the key")
{
    ShaderFolders folders;
    Run first(folders, SOURCE, "mainA", folders.cacheDir());

    SUBCASE("another entry point")
    {
        CHECK(Run(folders, SOURCE, "mainB", folders.cacheDir()).compiled());
    }
    SUBCASE("a define before the source, like a variant's")
    {
        CHECK(Run(folders, std::string("#define SCALE 3\n#line 1\n") + SOURCE, "mainA", folders.cacheDir()).compiled());
    }
    SUBCASE("an edit of an included file")
    {
        ShaderFolders::write(folders.root / "shaders" / "lib" / "common.slang", "#define SCALE 5\nuint scale(uint x) { return x * SCALE; }\n");
        Run edited(folders, SOURCE, "mainA", folders.cacheDir());
        CHECK(edited.compiled());
        CHECK(edited.code != first.code);
    }
    SUBCASE("a new file in lib/")
    {
        ShaderFolders::write(folders.root / "shaders" / "lib" / "other.slang", "\n");
        CHECK(Run(folders, SOURCE, "mainA", folders.cacheDir()).compiled());
    }
}

TEST_CASE("a damaged entry is compiled again and rewritten")
{
    ShaderFolders folders;
    Run first(folders, SOURCE, "mainA", folders.cacheDir());
    for (const auto &entry : fs::directory_iterator(folders.root / "cache" / "shaders"))
        fs::resize_file(entry.path(), fs::file_size(entry.path()) / 2);

    CHECK(Run(folders, SOURCE, "mainA", folders.cacheDir()).compiled());
    CHECK(Run(folders, SOURCE, "mainA", folders.cacheDir()).hit());
}

TEST_CASE("a shader that includes a file outside lib/ is not cached")
{
    ShaderFolders folders;
    ShaderFolders::write(folders.root / "outside.slang", "uint outside(uint x) { return x + 1; }\n");
    const std::string source = "#include \"" + rasm::pathToUtf8(folders.root / "outside.slang") + "\"\n" + SOURCE;
    CHECK(Run(folders, source, "mainA", folders.cacheDir()).compiled());
    CHECK(folders.entries() == 0);
    CHECK(Run(folders, source, "mainA", folders.cacheDir()).compiled());
}

TEST_CASE("without a cache folder every start compiles and writes nothing")
{
    ShaderFolders folders;
    CHECK(Run(folders, SOURCE, "mainA", "").compiled());
    CHECK(Run(folders, SOURCE, "mainA", "").compiled());
    CHECK_FALSE(fs::exists(folders.root / "cache"));
}

namespace
{
    std::vector<uint8_t> compileAlone(const ShaderFolders &folders, const std::string &source, const char *entryPoint)
    {
        rasm::ShaderCompiler compiler;
        REQUIRE(compiler.initialize(Target::SPIRV, folders.shaderDir(), ""));
        return compiler.compileFromString(source, "test.comp.slang", entryPoint).code;
    }

    // Asks without waiting until the workers have it; false after 30 s.
    bool pollUntilReady(rasm::ShaderCompiler &compiler, const std::string &source, const char *entryPoint, std::vector<uint8_t> &code)
    {
        for (uint32_t i = 0; i < 3000; ++i)
        {
            rasm::ShaderCompiler::CompiledShader shader = compiler.compileFromString(source, "test.comp.slang", entryPoint, false);
            if (!shader.pending)
            {
                code = shader.code;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool waitForNoSessions(const rasm::ShaderCompiler &compiler)
    {
        for (uint32_t i = 0; i < 3000 && compiler.getStats().workerSessions > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return compiler.getStats().workerSessions == 0;
    }
}

TEST_CASE("prefetched shaders compile once, on the workers or the caller, to the code a lone compile makes")
{
    ShaderFolders folders;
    const std::vector<uint8_t> a = compileAlone(folders, SOURCE, "mainA");
    const std::vector<uint8_t> b = compileAlone(folders, SOURCE, "mainB");
    REQUIRE_FALSE(a.empty());

    rasm::ShaderCompiler compiler;
    REQUIRE(compiler.initialize(Target::SPIRV, folders.shaderDir(), folders.cacheDir(), 2));
    const rasm::ShaderCompiler::Request requests[] = {{.source = SOURCE, .outputPath = "test.comp.slang", .entryPoint = "mainA"},
                                                      {.source = SOURCE, .outputPath = "test.comp.slang", .entryPoint = "mainB"},
                                                      {.source = SOURCE, .outputPath = "test.comp.slang", .entryPoint = "mainA"}};
    compiler.prefetch(requests, 3, false);
    compiler.prefetch(requests, 2, true);
    // Waited for while a worker has it, compiled here while none has started it: either way once.
    CHECK(compiler.compileFromString(SOURCE, "test.comp.slang", "mainB").code == b);
    CHECK(compiler.compileFromString(SOURCE, "test.comp.slang", "mainA").code == a);
    CHECK(compiler.compileFromString(SOURCE, "test.comp.slang", "mainA").code == a);
    CHECK(compiler.getStats().compiles == 2);
    CHECK(compiler.getStats().cacheHits == 0);
    CHECK(folders.entries() == 2);
    // The struct layouts come with a prefetched shader too.
    CHECK(compiler.findStructLayout("Item") != nullptr);
}

TEST_CASE("without wait a shader no worker has finished is pending, and the sessions go once the queue is empty")
{
    ShaderFolders folders;
    const std::vector<uint8_t> a = compileAlone(folders, SOURCE, "mainA");
    const std::vector<uint8_t> b = compileAlone(folders, SOURCE, "mainB");

    rasm::ShaderCompiler compiler;
    REQUIRE(compiler.initialize(Target::SPIRV, folders.shaderDir(), "", 1));
    CHECK(compiler.compileFromString(SOURCE, "test.comp.slang", "mainA", false).pending);
    std::vector<uint8_t> code;
    REQUIRE(pollUntilReady(compiler, SOURCE, "mainA", code));
    CHECK(code == a);
    CHECK(waitForNoSessions(compiler));

    // A shader asked for later makes a session again, which goes again.
    REQUIRE(pollUntilReady(compiler, SOURCE, "mainB", code));
    CHECK(code == b);
    CHECK(waitForNoSessions(compiler));
    CHECK(compiler.getStats().compiles == 2);
    CHECK(compiler.getStats().mainCompiles == 0);
}

TEST_CASE("a compiler shut down while its workers compile ends cleanly")
{
    ShaderFolders folders;
    std::vector<rasm::ShaderCompiler::Request> requests;
    requests.reserve(16);
    for (uint32_t i = 0; i < 16; ++i)
        requests.push_back({.source = "#define SCALE " + std::to_string(i + 3) + "\n#line 1\n" + SOURCE, .outputPath = "test.comp.slang", .entryPoint = "mainA"});
    {
        rasm::ShaderCompiler compiler;
        REQUIRE(compiler.initialize(Target::SPIRV, folders.shaderDir(), folders.cacheDir(), 4));
        compiler.prefetch(requests.data(), static_cast<uint32_t>(requests.size()), false);
        compiler.shutdown();
        // What no worker started is compiled by the caller after the shutdown.
        CHECK_FALSE(compiler.compileFromString(requests.back().source, "test.comp.slang", "mainA").code.empty());
    }
    {
        rasm::ShaderCompiler compiler;
        REQUIRE(compiler.initialize(Target::SPIRV, folders.shaderDir(), folders.cacheDir(), 4));
        compiler.prefetch(requests.data(), static_cast<uint32_t>(requests.size()), false);
    }
}
