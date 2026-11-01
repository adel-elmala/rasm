#include "renderer/shaderCompiler.h"
#include "rasm/types.h"
#include "renderer/uberMaterialTypes.h"

#include "slang.h"
#include "slang-com-helper.h"
#include "slang-com-ptr.h"

#include "spdlog/spdlog.h"

#include <array>
#include <fstream>
#include <string>
#include <filesystem>

namespace rasm
{
    namespace
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

        struct StructInfo
        {
            std::string name;
            std::vector<StructInfo> members;
            size_t size = 0;
            int32_t alignment = 0;
            size_t offset = 0;
        };

        std::optional<StructInfo> getStructInfo(Slang::ComPtr<slang::IComponentType> program, const std::string &name)
        {
            auto programLayout = program->getLayout();

            auto structReflection = programLayout->findTypeByName(name.c_str());
            if (!structReflection)
            {
                spdlog::error("Struct {} not found in shader program.", name);
                return std::nullopt;
            }

            auto structLayout = programLayout->getTypeLayout(structReflection, slang::LayoutRules::DefaultStructuredBuffer);

            auto structInfo = StructInfo{};
            structInfo.name = name;

            // Use getStride() if this struct is going to be used in arrays/buffers
            // to account for trailing layout/alignment padding.
            structInfo.size = structLayout->getSize();

            if (structLayout && structLayout->getKind() == slang::TypeReflection::Kind::Struct)
            {
                auto nFields = structLayout->getFieldCount();
                for (size_t i = 0; i < nFields; ++i)
                {
                    auto field = structLayout->getFieldByIndex(static_cast<unsigned int>(i));
                    auto fieldName = field->getName();

                    // Explicitly query uniform byte offset
                    auto fieldOffset = field->getOffset(slang::ParameterCategory::Uniform);

                    auto fieldType = field->getTypeLayout();
                    auto fieldAlignment = fieldType->getAlignment();
                    auto fieldSize = fieldType->getSize();

                    structInfo.members.push_back(StructInfo{
                        .name = fieldName ? fieldName : "",
                        .size = fieldSize,
                        .alignment = fieldAlignment,
                        .offset = fieldOffset});
                }
            }

            return structInfo;
        }

        void printStructInfo(const StructInfo &structInfo)
        {
            spdlog::info("Struct: {} (size: {}, alignment: {})", structInfo.name, structInfo.size, structInfo.alignment);
            for (const auto &member : structInfo.members)
            {
                spdlog::info("  Member: {} (size: {}, alignment: {}, offset: {})", member.name, member.size, member.alignment, member.offset);
            }
        }

        void validateRasmStructs(Slang::ComPtr<slang::IComponentType> program)
        {
            auto validate = [&program](std::string structName, size_t expectedSize)
            {
                auto si = getStructInfo(program, structName);
                if (!si)
                {
                    spdlog::error("Failed to get struct info for {}", structName);
                    return;
                }

                if (si->size != expectedSize)
                {
                    spdlog::error("Size mismatch for {}: expected {}, got {}", structName, expectedSize, si->size);
                    printStructInfo(*si);
                }
            };

            validate("VertexFull", sizeof(gfx::VertexFull));
            validate("VertexPosOnly", sizeof(gfx::VertexPosOnly));
            validate("DrawInfo", sizeof(gfx::DrawInfo));
            validate("Modeldata", sizeof(gfx::Modeldata));
            validate("SceneData", sizeof(gfx::SceneData));
            validate("Light", sizeof(gfx::Light));
            validate("MetallicRoughnessMaterial", sizeof(gfx::MetallicRoughnessMaterial));
            validate("NormalMaterial", sizeof(gfx::NormalMaterial));
            validate("OcclusionMaterial", sizeof(gfx::OcclusionMaterial));
            validate("EmissiveMaterial", sizeof(gfx::EmissiveMaterial));
            validate("Material", sizeof(gfx::Material));
        }
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

        std::filesystem::path cwd = std::filesystem::current_path();
        spdlog::info("Current Working Directory at runtime is: {}", cwd.string());

        // Adjust the text "../../" depending on how deep your build folder is from your project root
        std::string pathLib = (cwd / "shaders/lib").string();

        const char *searchPaths[] = {pathLib.c_str()};

        slang::SessionDesc sessionDesc = {};
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        sessionDesc.searchPaths = searchPaths;
        sessionDesc.searchPathCount = static_cast<uint32_t>(sizeof(searchPaths) / sizeof(searchPaths[0]));

        slang::CompilerOptionEntry options[] =
            {
                {slang::CompilerOptionName::VulkanUseEntryPointName,
                 {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}},
                {slang::CompilerOptionName::MatrixLayoutColumn,
                 {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}},
                {slang::CompilerOptionName::GLSLForceScalarLayout,
                 {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}}};

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

    ShaderCompiler::compiledShader ShaderCompiler::compile(const std::string &sourcePath, const std::string &outputPath, const std::string &entryPointName)
    {
        auto sourceCode = loadFileToString(sourcePath);
        if (sourceCode.empty())
        {
            spdlog::error("Failed to load shader source code from file: {}", sourcePath);
            return {};
        }

        return compileFromString(sourceCode, outputPath, entryPointName);
    }

    ShaderCompiler::compiledShader ShaderCompiler::compileFromString(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName)
    {
        // The session keeps every module and rejects a second one with the same name and different source,
        // so hash the source into the name.
        const auto moduleName = outputPath + "." + std::to_string(std::hash<std::string>{}(sourceCode));

        // Load module
        Slang::ComPtr<slang::IModule> slangModule;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;

            slangModule = activeSession->loadModuleFromSourceString(moduleName.c_str(),               // Module name
                                                                    (buildPath + moduleName).c_str(), // Module path
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

        constexpr SlangInt targetIndex = 0;

        // Get Target Code
        Slang::ComPtr<slang::IBlob> compiledBlob;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;

            SlangResult result = linkedProgram->getEntryPointCode(0,
                                                                  targetIndex,
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

        ShaderCompiler::compiledShader result;
        result.code = std::move(code);

        return result;
    }

    slang::TargetDesc ShaderCompiler::getTargetDesc(Target target)
    {
        // The returned desc only points at these, so they must outlive this function.
        static const slang::CompilerOptionEntry spirvOptions[] = {{slang::CompilerOptionName::EmitSpirvDirectly,
                                                                   {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}}};

        slang::TargetDesc targetDesc = {};

        switch (target)
        {
        case Target::SPIRV:
            targetDesc.format = SLANG_SPIRV;
            targetDesc.profile = globalSession->findProfile("spirv_1_4");
            targetDesc.compilerOptionEntryCount = 1;
            targetDesc.compilerOptionEntries = spirvOptions;
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