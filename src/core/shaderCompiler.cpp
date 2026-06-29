#include "rasm/core/shaderCompiler.h"

#include "slang.h"
#include "slang-com-helper.h"
#include "slang-com-ptr.h"

#include "spdlog/spdlog.h"

#include <array>
#include <fstream>
#include <string>

namespace rasm
{

    void diagnoseIfNeeded(slang::IBlob *diagnosticsBlob)
    {
        if (diagnosticsBlob != nullptr)
        {
            spdlog::error("Shader compilation diagnostics: {}", (const char *)diagnosticsBlob->getBufferPointer());
        }
    }

    std::string loadFileToString(const std::string &filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            spdlog::error("Failed to open file: {}", filePath);
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    bool ShaderCompiler::initialize(Target target)
    {
        // Create Slang Global Session
        auto result = createGlobalSession(globalSession.writeRef());

        if (SLANG_FAILED(result))
        {
            spdlog::error("Failed to create Slang global session. Error code: {}", result);
            return false;
        }

        slang::TargetDesc targetDesc = getTargetDesc(target);

        slang::SessionDesc sessionDesc = {};
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;

        slang::CompilerOptionEntry options[] =
            {
                {slang::CompilerOptionName::VulkanUseEntryPointName,
                 {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}},
                {slang::CompilerOptionName::MatrixLayoutColumn,
                 {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}}
            };

        sessionDesc.compilerOptionEntries = options;
        sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(sizeof(options) / sizeof(options[0]));

        auto sessionResult = globalSession->createSession(sessionDesc, activeSession.writeRef());

        if (SLANG_FAILED(sessionResult))
        {
            spdlog::error("Failed to create Slang session. Error code: {}", sessionResult);
            return false;
        }

        return true;
    }

    void ShaderCompiler::shutdown() {}

    std::vector<uint8_t> ShaderCompiler::compile(const std::string &sourcePath, const std::string &outputPath, const std::string &entryPointName)
    {
        auto sourceCode = loadFileToString(sourcePath);
        if (sourceCode.empty())
        {
            spdlog::error("Failed to load shader source code from file: {}", sourcePath);
            return {};
        }

        return compileFromString(sourceCode, outputPath, entryPointName);
    }

    std::vector<uint8_t> ShaderCompiler::compileFromString(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName)
    {
        // Load module
        Slang::ComPtr<slang::IModule> slangModule;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;

            slangModule = activeSession->loadModuleFromSourceString(outputPath.c_str(),               // Module name
                                                                    (buildPath + outputPath).c_str(), // Module path
                                                                    sourceCode.c_str(),               // Shader source code
                                                                    diagnosticsBlob.writeRef());      // Optional diagnostic container

            diagnoseIfNeeded(diagnosticsBlob);

            if (!slangModule)
            {
                return {};
            }
        }

        // Query Entry Points
        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        {
            // Slang::ComPtr<slang::IBlob> diagnosticsBlob;
            slangModule->findEntryPointByName(entryPointName.c_str(), entryPoint.writeRef());
            if (!entryPoint)
            {
                spdlog::error("Error getting entry point: {}", entryPointName);
                return {};
            }
        }

        // Compose Modules + Entry Points
        std::array<slang::IComponentType *, 2> componentTypes = {slangModule,
                                                                 entryPoint};

        Slang::ComPtr<slang::IComponentType> composedProgram;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;
            auto result = activeSession->createCompositeComponentType(componentTypes.data(),
                                                                      componentTypes.size(),
                                                                      composedProgram.writeRef(),
                                                                      diagnosticsBlob.writeRef());

            diagnoseIfNeeded(diagnosticsBlob);
            if (SLANG_FAILED(result))
            {
                spdlog::error("Error creating composite component type");
                return {};
            }
        }

        // Link
        Slang::ComPtr<slang::IComponentType> linkedProgram;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;
            SlangResult result = composedProgram->link(linkedProgram.writeRef(),
                                                       diagnosticsBlob.writeRef());

            diagnoseIfNeeded(diagnosticsBlob);
            if (SLANG_FAILED(result))
            {
                spdlog::error("Error linking component type");
                return {};
            }
        }

        // Get Target Code
        Slang::ComPtr<slang::IBlob> compiledBlob;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;

            SlangResult result = linkedProgram->getEntryPointCode(0,
                                                                  0,
                                                                  compiledBlob.writeRef(),
                                                                  diagnosticsBlob.writeRef());
            diagnoseIfNeeded(diagnosticsBlob);
            if (SLANG_FAILED(result))
            {
                spdlog::error("Error getting entry point code");
                return {};
            }
        }

        auto pBlob = compiledBlob->getBufferPointer();
        auto blobSize = compiledBlob->getBufferSize();

        std::vector<uint8_t> code(blobSize);
        std::memcpy(code.data(), pBlob, blobSize);

        return code;
    }

    slang::TargetDesc ShaderCompiler::getTargetDesc(Target target)
    {
        slang::TargetDesc targetDesc = {};

        switch (target)
        {
        case Target::SPIRV:
            targetDesc.format = SLANG_SPIRV;
            targetDesc.profile = globalSession->findProfile("spirv_1_4");
            targetDesc.compilerOptionEntryCount = 1;
            targetDesc.compilerOptionEntries = new slang::CompilerOptionEntry[1]{
                {slang::CompilerOptionName::EmitSpirvDirectly,
                 {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}}};
            break;
        case Target::MSL:
            targetDesc.format = SLANG_METAL;
            targetDesc.profile = globalSession->findProfile("metal_2_3");
            break;
        case Target::HLSL:
            targetDesc.format = SLANG_HLSL;
            targetDesc.profile = globalSession->findProfile("sm_6_0");
            break;
        case Target::GLSL:
            targetDesc.format = SLANG_GLSL;
            targetDesc.profile = globalSession->findProfile("glsl_4_5");
            break;
        default:
            spdlog::error("Unsupported shader target.");
            break;
        }
        return targetDesc;
    }

}