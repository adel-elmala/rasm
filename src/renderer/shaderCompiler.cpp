#include "renderer/shaderCompiler.h"
#include "rasm/types.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "slang.h"
#include "slang-com-helper.h"
#include "slang-com-ptr.h"

#include "spdlog/spdlog.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
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

        struct FieldOffset
        {
            const char *name;
            size_t offset;
        };

        slang::VariableLayoutReflection *findField(slang::TypeLayoutReflection *structLayout, const char *name)
        {
            for (unsigned int i = 0; i < structLayout->getFieldCount(); ++i)
            {
                auto field = structLayout->getFieldByIndex(i);
                if (std::strcmp(field->getName(), name) == 0)
                {
                    return field;
                }
            }

            return nullptr;
        }

        // The struct a field holds, looking through a pointer or an array.
        slang::TypeLayoutReflection *fieldStruct(slang::TypeLayoutReflection *structLayout, const char *name)
        {
            auto field = structLayout ? findField(structLayout, name) : nullptr;
            if (!field)
            {
                return nullptr;
            }

            auto fieldType = field->getTypeLayout();
            return fieldType->getKind() == slang::TypeReflection::Kind::Struct ? fieldType : fieldType->getElementTypeLayout();
        }

        bool validateStruct(slang::TypeLayoutReflection *structLayout, const char *name, size_t expectedSize, const std::vector<FieldOffset> &expectedFields)
        {
            if (!structLayout)
            {
                spdlog::error("Struct {} not found in shader program.", name);
                return false;
            }

            bool valid = true;

            if (structLayout->getSize() != expectedSize)
            {
                spdlog::error("Size mismatch for {}: C++ {}, shader {}", name, expectedSize, structLayout->getSize());
                valid = false;
            }

            for (const auto &expected : expectedFields)
            {
                auto field = findField(structLayout, expected.name);
                if (!field)
                {
                    spdlog::error("Field {}.{} not found in shader program.", name, expected.name);
                    valid = false;
                    continue;
                }

                auto offset = field->getOffset(slang::ParameterCategory::Uniform);
                if (offset != expected.offset)
                {
                    spdlog::error("Offset mismatch for {}.{}: C++ {}, shader {}", name, expected.name, expected.offset, offset);
                    valid = false;
                }
            }

            return valid;
        }

        // Walks pc -> scene -> pointers because only that reports the layout the shaders read. ProgramLayout::getTypeLayout
        // has no LayoutRules value for data behind a pointer, so it adds std140/std430 padding (Modeldata is 240 there, not 188).
        bool validateRasmStructs(slang::ProgramLayout *programLayout)
        {
            slang::TypeLayoutReflection *pushConstants = nullptr;
            for (unsigned int i = 0; i < programLayout->getParameterCount(); ++i)
            {
                auto parameter = programLayout->getParameterByIndex(i);
                if (std::strcmp(parameter->getName(), "pc") == 0)
                {
                    pushConstants = parameter->getTypeLayout()->getElementTypeLayout();
                }
            }

            // Custom pipelines bring their own push constants and do not share core.slang.
            auto scene = fieldStruct(pushConstants, "scene");
            if (!scene || std::strcmp(scene->getName(), "SceneData") != 0)
            {
                return true;
            }

            auto modeldata = fieldStruct(scene, "modeldataPtr");
            auto material = fieldStruct(modeldata, "material");

            bool valid = true;

            valid &= validateStruct(pushConstants, "PushConstants", sizeof(gfx::PushConstants),
                                    {{"scene", offsetof(gfx::PushConstants, scene)},
                                     {"firstDraw", offsetof(gfx::PushConstants, firstDraw)},
                                     {"padding", offsetof(gfx::PushConstants, padding)}});

            valid &= validateStruct(scene, "SceneData", sizeof(gfx::SceneData),
                                    {{"projection", offsetof(gfx::SceneData, projection)},
                                     {"view", offsetof(gfx::SceneData, view)},
                                     {"cameraWorldPosition", offsetof(gfx::SceneData, cameraWorldPosition)},
                                     {"numLights", offsetof(gfx::SceneData, numLights)},
                                     {"lights", offsetof(gfx::SceneData, lights)},
                                     {"vertsPtr", offsetof(gfx::SceneData, vertsPtr)},
                                     {"drawInfoPtr", offsetof(gfx::SceneData, drawInfoPtr)},
                                     {"modeldataPtr", offsetof(gfx::SceneData, modeldataPtr)}});

            valid &= validateStruct(fieldStruct(scene, "lights"), "Light", sizeof(gfx::Light),
                                    {{"position", offsetof(gfx::Light, position)},
                                     {"intensity", offsetof(gfx::Light, intensity)},
                                     {"color", offsetof(gfx::Light, color)},
                                     {"range", offsetof(gfx::Light, range)},
                                     {"direction", offsetof(gfx::Light, direction)},
                                     {"type", offsetof(gfx::Light, type)}});

            valid &= validateStruct(fieldStruct(scene, "vertsPtr"), "VertexFull", sizeof(gfx::VertexFull),
                                    {{"pos", offsetof(gfx::VertexFull, pos)},
                                     {"normal", offsetof(gfx::VertexFull, normal)},
                                     {"uv", offsetof(gfx::VertexFull, uv)},
                                     {"tangent", offsetof(gfx::VertexFull, tangent)}});

            valid &= validateStruct(fieldStruct(scene, "drawInfoPtr"), "DrawInfo", sizeof(gfx::DrawInfo),
                                    {{"indexCount", offsetof(gfx::DrawInfo, indexCount)},
                                     {"instanceCount", offsetof(gfx::DrawInfo, instanceCount)},
                                     {"firstIndex", offsetof(gfx::DrawInfo, firstIndex)},
                                     {"vertexOffset", offsetof(gfx::DrawInfo, vertexOffset)},
                                     {"firstInstance", offsetof(gfx::DrawInfo, firstInstance)}});

            valid &= validateStruct(modeldata, "Modeldata", sizeof(gfx::Modeldata),
                                    {{"worldMatrix", offsetof(gfx::Modeldata, worldMatrix)},
                                     {"normalMatrix", offsetof(gfx::Modeldata, normalMatrix)},
                                     {"material", offsetof(gfx::Modeldata, material)},
                                     {"vertFormat", offsetof(gfx::Modeldata, vertFormat)},
                                     {"materialId", offsetof(gfx::Modeldata, materialId)}});

            valid &= validateStruct(material, "Material", sizeof(gfx::Material),
                                    {{"metallicRoughness", offsetof(gfx::Material, pbr)},
                                     {"normal", offsetof(gfx::Material, normal)},
                                     {"occlusion", offsetof(gfx::Material, occlusion)},
                                     {"emissive", offsetof(gfx::Material, emissive)}});

            valid &= validateStruct(fieldStruct(material, "metallicRoughness"), "MetallicRoughnessMaterial", sizeof(gfx::MetallicRoughnessMaterial),
                                    {{"baseColorFactor", offsetof(gfx::MetallicRoughnessMaterial, baseColorFactor)},
                                     {"baseColorTextureIndex", offsetof(gfx::MetallicRoughnessMaterial, baseColorTextureIndex)},
                                     {"baseColorTextureCoordinate", offsetof(gfx::MetallicRoughnessMaterial, baseColorTextureCoordinate)},
                                     {"metallicRoughnessTextureIndex", offsetof(gfx::MetallicRoughnessMaterial, metallicRoughnessTextureIndex)},
                                     {"metallicRoughnessTextureCoordinate", offsetof(gfx::MetallicRoughnessMaterial, metallicRoughnessTextureCoordinate)},
                                     {"metallicFactor", offsetof(gfx::MetallicRoughnessMaterial, metallicFactor)},
                                     {"roughnessFactor", offsetof(gfx::MetallicRoughnessMaterial, roughnessFactor)}});

            valid &= validateStruct(fieldStruct(material, "normal"), "NormalMaterial", sizeof(gfx::NormalMaterial),
                                    {{"normalTextureIndex", offsetof(gfx::NormalMaterial, normalTextureIndex)},
                                     {"normalTextureCoordinate", offsetof(gfx::NormalMaterial, normalTextureCoordinate)},
                                     {"scale", offsetof(gfx::NormalMaterial, scale)}});

            valid &= validateStruct(fieldStruct(material, "occlusion"), "OcclusionMaterial", sizeof(gfx::OcclusionMaterial),
                                    {{"occlusionTextureIndex", offsetof(gfx::OcclusionMaterial, occlusionTextureIndex)},
                                     {"occlusionTextureCoordinate", offsetof(gfx::OcclusionMaterial, occlusionTextureCoordinate)},
                                     {"strength", offsetof(gfx::OcclusionMaterial, strength)}});

            valid &= validateStruct(fieldStruct(material, "emissive"), "EmissiveMaterial", sizeof(gfx::EmissiveMaterial),
                                    {{"emissiveTextureIndex", offsetof(gfx::EmissiveMaterial, emissiveTextureIndex)},
                                     {"emissiveTextureCoordinate", offsetof(gfx::EmissiveMaterial, emissiveTextureCoordinate)},
                                     {"emissiveFactor", offsetof(gfx::EmissiveMaterial, emissiveFactor)}});

            return valid;
        }
    }

    bool ShaderCompiler::initialize(Target target, const std::string &shaderDir)
    {
        const std::filesystem::path libDir = pathFromUtf8(shaderDir) / "lib";
        const std::filesystem::path uberShader = pathFromUtf8(shaderDir) / "passes/rasm.slang";
        std::error_code errorCode;
        if (!std::filesystem::is_directory(libDir, errorCode) || !std::filesystem::is_regular_file(uberShader, errorCode))
        {
            spdlog::error("Shaders not found in {}. Copy them there, or set EngineConfig::shaderDir.", shaderDir);
            return false;
        }
        spdlog::info("Loading shaders from {}", shaderDir);

        // Create Slang Global Session
        auto result = createGlobalSession(globalSession.writeRef());

        if (SLANG_FAILED(result))
        {
            spdlog::error("Failed to create Slang global session. Error code: {}", result);
            return false;
        }

        slang::TargetDesc targetDesc = getTargetDesc(target);

        const std::string pathLib = pathToUtf8(libDir);

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

    ShaderCompiler::CompiledShader ShaderCompiler::compile(const std::string &sourcePath, const std::string &outputPath, const std::string &entryPointName)
    {
        auto sourceCode = loadFileToString(sourcePath);
        if (sourceCode.empty())
        {
            spdlog::error("Failed to load shader source code from file: {}", sourcePath);
            return {};
        }

        return compileFromString(sourceCode, outputPath, entryPointName);
    }

    ShaderCompiler::CompiledShader ShaderCompiler::compileFromString(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName)
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

        assert(validateRasmStructs(linkedProgram->getLayout()) && "C++ structs must match shaders/lib/core.slang");

        constexpr SlangInt TARGET_INDEX = 0;

        // Get Target Code
        Slang::ComPtr<slang::IBlob> compiledBlob;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;

            SlangResult result = linkedProgram->getEntryPointCode(0,
                                                                  TARGET_INDEX,
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

        ShaderCompiler::CompiledShader result;
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
        case Target::WGSL:
            targetDesc.format = SLANG_WGSL;
            break;
        default:
            spdlog::error("Unsupported shader target.");
            break;
        }
        return targetDesc;
    }

}