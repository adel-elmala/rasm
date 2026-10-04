#include "rasm/core/shaderCompiler.h"
#include "rasm/core/types.h"

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
                 {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}},
                 {slang::CompilerOptionName::GLSLForceScalarLayout,
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

    rasm::Format mapSlangTypeToFormat(slang::TypeReflection *type)
    {
        if (!type)
            return Format::UNKNOWN;

        switch (type->getKind())
        {
        case slang::TypeReflection::Kind::Vector:
        {
            auto elementType = type->getElementType();
            auto elementCount = type->getElementCount();

            if (elementType->getKind() == slang::TypeReflection::Kind::Scalar)
            {
                switch (elementType->getScalarType())
                {
                case slang::TypeReflection::ScalarType::Float32:
                    if (elementCount == 2)
                        return Format::R32G32_SFLOAT;
                    else if (elementCount == 3)
                        return Format::R32G32B32_SFLOAT;
                    else if (elementCount == 4)
                        return Format::R32G32B32A32_SFLOAT;
                    break;
                case slang::TypeReflection::ScalarType::UInt8:
                    if (elementCount == 4)
                        return Format::R8G8B8A8_UNORM; // Assuming UNORM for simplicity
                    break;
                default:
                    break;
                }
            }
            break;
        }
        default:
            break;
        }

        return Format::UNKNOWN;
    }


    uint32_t formatSize(Format format)
    {
        switch (format)
        {
        case Format::R8G8B8A8_UNORM:
        case Format::R8G8B8A8_SRGB:
        case Format::B8G8R8A8_SRGB:
            return 4;
        case Format::R16G16B16A16_SFLOAT:
            return 8;
        case Format::R32G32B32_SFLOAT:
            return 12;
        case Format::R32G32B32A32_SFLOAT:
            return 16;
        case Format::D24_UNORM_S8_UINT:
            return 4; // 3 bytes for depth + 1 byte for stencil
        case Format::R32G32_SFLOAT:
            return 8;
        case Format::U16_UINT:
            return 2;
        case Format::U32_UINT:
            return 4;
        default:
            return 0; // Unknown format
        }
    }

    std::vector<VertexAttributeDescription> slangParamToAttrb(slang::VariableLayoutReflection *param, uint32_t &totalOffset)
    {
        std::vector<VertexAttributeDescription> vertexAttributes;

        auto typeLayout = param->getTypeLayout();
        auto paramType = typeLayout->getType();

        switch (paramType->getKind())
        {
        case slang::TypeReflection::Kind::Struct:
        {
            auto memberCount = typeLayout->getFieldCount();

            for (size_t k = 0; k < memberCount; ++k)
            {
                auto member = typeLayout->getFieldByIndex(static_cast<unsigned int>(k));
                auto attrs = slangParamToAttrb(member, totalOffset);
                vertexAttributes.insert(vertexAttributes.end(), attrs.begin(), attrs.end());
            }
            break;
        }
        case slang::TypeReflection::Kind::Vector:
        case slang::TypeReflection::Kind::Scalar:
        {
            VertexAttributeDescription attrDesc;

            attrDesc.binding = 0; // Assuming a single binding for simplicity
            attrDesc.location = static_cast<uint32_t>(param->getOffset(slang::ParameterCategory::VaryingInput));
            attrDesc.offset = totalOffset;
            attrDesc.format = mapSlangTypeToFormat(paramType);
            attrDesc.size = formatSize(attrDesc.format);
            attrDesc.used = true;

            totalOffset += attrDesc.size;
            vertexAttributes.push_back(attrDesc);
            break;
        }
        default:
        {
            spdlog::warn("Unsupported parameter type for vertex input layout: {}", static_cast<int>(paramType->getKind()));
            break;
        }
        }

        return vertexAttributes;
    }

    std::vector<VertexAttributeDescription> getProgramVertexInputLayout(Slang::ComPtr<slang::IComponentType> program)
    {
        std::vector<VertexAttributeDescription> vertexAttributes;

        auto layout = program->getLayout(0);
        auto nEntrys = layout->getEntryPointCount();

        for (size_t i = 0; i < nEntrys; ++i)
        {
            auto entry = layout->getEntryPointByIndex(i);

            if (entry->getStage() != SLANG_STAGE_VERTEX)
                continue;

            auto nParams = entry->getParameterCount();

            uint32_t totalOffset = 0;
            for (size_t j = 0; j < nParams; ++j)
            {
                auto param = entry->getParameterByIndex(static_cast<unsigned int>(j));

                if (param->getCategory() == slang::ParameterCategory::VaryingInput)
                {
                    auto attrs = slangParamToAttrb(param, totalOffset);
                    vertexAttributes.insert(vertexAttributes.end(), attrs.begin(), attrs.end());
                }
            }
        }
        return vertexAttributes;
    }

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

        auto vertexInputLayout = getProgramVertexInputLayout(linkedProgram);

        std::vector<uint8_t> code(blobSize);
        std::memcpy(code.data(), pBlob, blobSize);

        ShaderCompiler::compiledShader result;
        result.code = std::move(code);
        result.vertexInputLayout = std::move(vertexInputLayout);

        return result;
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