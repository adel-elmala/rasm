#pragma once

#include "rasm/core/types.h"
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

        struct compiledShader
        {
            std::vector<uint8_t> code;
            std::vector<VertexAttributeDescription> vertexInputLayout;
        };

        ShaderCompiler() = default;
        ~ShaderCompiler() = default;

        bool initialize(Target target);
        void shutdown();

        compiledShader compile(const std::string &sourcePath, const std::string &outputPath, const std::string &entryPointName);
        compiledShader compileFromString(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName);

    private:
        slang::TargetDesc getTargetDesc(Target target);

    private:
        Slang::ComPtr<slang::IGlobalSession> globalSession;
        Slang::ComPtr<slang::ISession> activeSession;
        std::string buildPath = "./build/shaders/";
    };

}