// clang-format off
#pragma once

#include "rasm/types.h"

#include <slang.h>
#include <slang-com-ptr.h>

#include <cstdint>
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
        uint32_t                columns         = 1;    // scalars per line: a matrix's row, in memory order
    };

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

                        ShaderCompiler() = default;
                        ~ShaderCompiler() = default;

        bool            initialize(Target target, const std::string& shaderDir);
        void            shutdown();

        CompiledShader  compile(const std::string& sourcePath, const std::string& outputPath, const std::string& entryPointName);
        CompiledShader  compileFromString(const std::string& sourceCode, const std::string& outputPath, const std::string& entryPointName);
        // From now on, each program compiled keeps the layouts of the structs its push constants reach.
        void            keepStructLayouts();
        // The layout of a struct some compiled program reached, or nullptr.
        const std::vector<StructField>* findStructLayout(const std::string& name) const;

    private:
        slang::TargetDesc getTargetDesc(Target target);

    private:
        Slang::ComPtr<slang::IGlobalSession>    globalSession;
        Slang::ComPtr<slang::ISession>          activeSession;
        std::string                             buildPath = "./build/shaders/";
        bool                                    keepLayouts = false;
        std::unordered_map<std::string, std::vector<StructField>>   structLayouts;
    };

}
// clang-format on
