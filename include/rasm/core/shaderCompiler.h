#pragma once

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

        ShaderCompiler() = default;
        ~ShaderCompiler() = default;

        bool initialize(Target target);
        void shutdown();

        std::vector<uint8_t> compile(const std::string &sourcePath, const std::string &outputPath, const std::string &entryPointName);
        std::vector<uint8_t> compileFromString(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName);

    private:
        slang::TargetDesc getTargetDesc(Target target);

    private:
        Slang::ComPtr<slang::IGlobalSession> globalSession;
        Slang::ComPtr<slang::ISession> activeSession;
        std::string buildPath = "./build/shaders/";
    };

}