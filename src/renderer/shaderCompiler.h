// clang-format off
#pragma once

#include "rasm/types.h"

#include <slang.h>
#include <slang-com-ptr.h>

#include <string>
#include <vector>

namespace rasm
{
    class ShaderCompiler
    {
    public:
        enum class Target
        {
            SPIRV,
            MSL,
            HLSL,
            GLSL
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

    private:
        slang::TargetDesc getTargetDesc(Target target);

    private:
        Slang::ComPtr<slang::IGlobalSession>    globalSession;
        Slang::ComPtr<slang::ISession>          activeSession;
        std::string                             buildPath = "./build/shaders/";
    };

}
// clang-format on
