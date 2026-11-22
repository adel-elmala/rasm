// clang-format off
#pragma once

#include "core/cacheFile.h"
#include "rasm/types.h"

#include <slang.h>
#include <slang-com-ptr.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rasm
{
    enum class FieldType
    {
        FLOAT,
        INT,
        UINT,
        UINT64,
        POINTER,
        OTHER,
    };

    // A field of a GPU struct as the shaders lay it out: a nested struct's and an array's fields are flattened into
    // vectors, matrices and scalars, named like "material.normal.scale" or "previousWorld[2]".
    struct StructField
    {
        std::string             name;
        std::string             typeName;
        uint32_t                offset          = 0;
        uint32_t                size            = 0;
        FieldType               type            = FieldType::OTHER;
        uint32_t                count           = 1;    // scalars: a vector's or a matrix's
        uint32_t                columns         = 1;    // a vector's scalars, a matrix's columns; a matrix is stored column by column
    };

    struct StructMember
    {
        std::string             name;
        uint32_t                offset          = 0;
        std::string             structName;
    };

    struct StructLayout
    {
        uint32_t                    size        = 0;
        std::vector<StructMember>   members;
        std::vector<StructField>    fields;
    };

    using StructLayouts = std::unordered_map<std::string, StructLayout>;

    class ShaderCompiler
    {
    public:
        enum class Target
        {
            SPIRV,
            MSL,
            HLSL,
            GLSL,
            WGSL
        };

        struct CompiledShader
        {
            std::vector<uint8_t> code;
        };

        struct Stats
        {
            uint32_t            cacheHits       = 0;
            uint32_t            compiles        = 0;
            double              compileSeconds  = 0.0;
        };

                        ShaderCompiler() = default;
                        ~ShaderCompiler() = default;

        bool            initialize(Target target, const std::string& shaderDir, const std::string& cacheDir);
        void            shutdown();

        CompiledShader  compile(const std::string& sourcePath, const std::string& outputPath, const std::string& entryPointName);
        CompiledShader  compileFromString(const std::string& sourceCode, const std::string& outputPath, const std::string& entryPointName);
        // The layout of a struct some compiled program reached, or nullptr.
        const std::vector<StructField>* findStructLayout(const std::string& name) const;
        const Stats&    getStats() const;

    private:
        slang::TargetDesc getTargetDesc(Target target);
        bool            compileWithSlang(const std::string& sourceCode, const std::string& outputPath, const std::string& entryPointName, CompiledShader& shader,
                                         std::string& pushConstants, StructLayouts& layouts, bool& cacheable);

    private:
        Slang::ComPtr<slang::IGlobalSession>    globalSession;
        Slang::ComPtr<slang::ISession>          activeSession;
        std::string                             buildPath = "./build/shaders/";
        std::filesystem::path                   libDir;
        std::filesystem::path                   cacheFolder;
        CacheKey                                baseKey;
        StructLayouts                           structLayouts;
        Stats                                   stats;
    };

}
// clang-format on
