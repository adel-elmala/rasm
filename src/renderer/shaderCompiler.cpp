#include "renderer/shaderCompiler.h"
#include "core/workerPool.h"
#include "rasm/types.h"
#include "platform/paths.h"
#include "renderer/shaderCache.h"
#include "renderer/uberMaterialTypes.h"

#include "slang.h"
#include "slang-com-helper.h"
#include "slang-com-ptr.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <filesystem>
#include <unordered_map>

namespace rasm
{
    namespace
    {
        struct FieldOffset
        {
            const char *name;
            size_t offset;
        };

        // A new layout of the cache's key or files: every old entry misses.
        constexpr uint32_t SHADER_CACHE_KEY_VERSION = 1;
        constexpr uint32_t SESSION_OPTION_COUNT = 3;
    }

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

        const StructMember *findMember(const StructLayout *layout, const char *name)
        {
            if (!layout)
                return nullptr;
            for (const StructMember &member : layout->members)
                if (member.name == name)
                    return &member;
            return nullptr;
        }

        // The struct a field holds, looking through a pointer or an array.
        const StructLayout *fieldStruct(const StructLayouts &layouts, const StructLayout *structLayout, const char *name)
        {
            const StructMember *member = findMember(structLayout, name);
            if (!member)
                return nullptr;
            const auto found = layouts.find(member->structName);
            return found != layouts.end() ? &found->second : nullptr;
        }

        // A vector's or a matrix's scalar type, through its element types.
        FieldType getFieldType(slang::TypeReflection *type)
        {
            while (type && type->getKind() != slang::TypeReflection::Kind::Scalar && type->getElementType())
                type = type->getElementType();
            switch (type ? type->getScalarType() : slang::TypeReflection::ScalarType::None)
            {
            case slang::TypeReflection::ScalarType::Float32:
                return FieldType::FLOAT;
            case slang::TypeReflection::ScalarType::Int32:
                return FieldType::INT;
            case slang::TypeReflection::ScalarType::UInt32:
                return FieldType::UINT;
            case slang::TypeReflection::ScalarType::UInt64:
                return FieldType::UINT64;
            default:
                return FieldType::OTHER;
            }
        }

        std::string getScalarName(FieldType type)
        {
            switch (type)
            {
            case FieldType::FLOAT:
                return "float";
            case FieldType::INT:
                return "int";
            case FieldType::UINT:
                return "uint";
            case FieldType::UINT64:
                return "uint64_t";
            default:
                return "?";
            }
        }

        void flattenFields(slang::TypeLayoutReflection *layout, const std::string &name, uint32_t offset, std::vector<StructField> &fields)
        {
            using Kind = slang::TypeReflection::Kind;
            const char *typeName = layout->getName();
            StructField field = {.name = name, .typeName = typeName ? typeName : "", .offset = offset, .size = static_cast<uint32_t>(layout->getSize())};
            switch (layout->getKind())
            {
            case Kind::Struct:
                for (unsigned int i = 0; i < layout->getFieldCount(); ++i)
                {
                    auto member = layout->getFieldByIndex(i);
                    flattenFields(member->getTypeLayout(), name.empty() ? member->getName() : name + "." + member->getName(),
                                  offset + static_cast<uint32_t>(member->getOffset(slang::ParameterCategory::Uniform)), fields);
                }
                return;
            case Kind::Array:
                for (size_t i = 0; i < layout->getElementCount(); ++i)
                    flattenFields(layout->getElementTypeLayout(), name + "[" + std::to_string(i) + "]",
                                  offset + static_cast<uint32_t>(i * layout->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM)), fields);
                return;
            case Kind::Pointer:
                field.type = FieldType::POINTER;
                field.typeName = "pointer";
                break;
            case Kind::Enum:
                field.type = FieldType::UINT;
                break;
            case Kind::Scalar:
                field.type = getFieldType(layout->getType());
                break;
            case Kind::Vector:
                field.type = getFieldType(layout->getType());
                field.count = static_cast<uint32_t>(layout->getElementCount());
                field.columns = field.count;
                field.typeName = getScalarName(field.type) + std::to_string(field.count);
                break;
            case Kind::Matrix:
                field.type = getFieldType(layout->getType());
                field.count = layout->getRowCount() * layout->getColumnCount();
                field.columns = layout->getColumnCount();
                field.typeName = getScalarName(field.type) + std::to_string(layout->getRowCount()) + "x" + std::to_string(layout->getColumnCount());
                break;
            default:
                break;
            }
            fields.push_back(field);
        }

        // A struct's name with its namespaces and generic arguments, so two structs a program reaches never share one.
        // A struct at global scope, like every RASM struct, keeps its plain name.
        std::string getStructName(slang::TypeLayoutReflection *layout)
        {
            Slang::ComPtr<slang::IBlob> fullName;
            if (layout->getType() && SLANG_SUCCEEDED(layout->getType()->getFullName(fullName.writeRef())) && fullName && fullName->getBufferSize() > 0)
                return std::string(static_cast<const char *>(fullName->getBufferPointer()), strnlen(static_cast<const char *>(fullName->getBufferPointer()), fullName->getBufferSize()));
            const char *name = layout->getName();
            return name ? name : "";
        }

        // The struct a member holds, directly or through a pointer or an array, like the checks look it up.
        std::string memberStructName(slang::TypeLayoutReflection *type)
        {
            using Kind = slang::TypeReflection::Kind;
            slang::TypeLayoutReflection *held = type->getKind() == Kind::Struct ? type : type->getElementTypeLayout();
            return held && held->getKind() == Kind::Struct ? getStructName(held) : "";
        }

        // Every struct the layout reaches, through fields, arrays and pointers, by name. A struct seen before is
        // skipped, which also ends pointer cycles.
        void collectStructLayouts(slang::TypeLayoutReflection *layout, StructLayouts &layouts)
        {
            using Kind = slang::TypeReflection::Kind;
            if (!layout)
                return;
            if (layout->getKind() == Kind::Pointer || layout->getKind() == Kind::Array)
            {
                collectStructLayouts(layout->getElementTypeLayout(), layouts);
                return;
            }
            if (layout->getKind() != Kind::Struct)
                return;
            const std::string name = getStructName(layout);
            if (name.empty() || layouts.count(name) > 0)
                return;
            StructLayout &structLayout = layouts[name];
            structLayout.size = static_cast<uint32_t>(layout->getSize());
            for (unsigned int i = 0; i < layout->getFieldCount(); ++i)
            {
                auto member = layout->getFieldByIndex(i);
                structLayout.members.push_back({.name = member->getName(),
                                                .offset = static_cast<uint32_t>(member->getOffset(slang::ParameterCategory::Uniform)),
                                                .structName = memberStructName(member->getTypeLayout())});
            }
            flattenFields(layout, "", 0, structLayout.fields);
            for (unsigned int i = 0; i < layout->getFieldCount(); ++i)
                collectStructLayouts(layout->getFieldByIndex(i)->getTypeLayout(), layouts);
        }

        bool validateStruct(const StructLayout *structLayout, const char *name, size_t expectedSize, const std::vector<FieldOffset> &expectedFields)
        {
            if (!structLayout)
            {
                spdlog::error("Struct {} not found in shader program.", name);
                return false;
            }

            bool valid = true;

            if (structLayout->size != expectedSize)
            {
                spdlog::error("Size mismatch for {}: C++ {}, shader {}", name, expectedSize, structLayout->size);
                valid = false;
            }

            for (const auto &expected : expectedFields)
            {
                const StructMember *member = findMember(structLayout, expected.name);
                if (!member)
                {
                    spdlog::error("Field {}.{} not found in shader program.", name, expected.name);
                    valid = false;
                    continue;
                }

                if (member->offset != expected.offset)
                {
                    spdlog::error("Offset mismatch for {}.{}: C++ {}, shader {}", name, expected.name, expected.offset, member->offset);
                    valid = false;
                }
            }

            return valid;
        }

        void hashOptions(CacheKey &key, const slang::CompilerOptionEntry *options, uint32_t count)
        {
            hashNumber(key, count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const slang::CompilerOptionEntry &option = options[i];
                hashNumber(key, static_cast<uint64_t>(option.name));
                hashNumber(key, static_cast<uint64_t>(option.value.kind));
                hashNumber(key, static_cast<uint64_t>(static_cast<int64_t>(option.value.intValue0)));
                hashNumber(key, static_cast<uint64_t>(static_cast<int64_t>(option.value.intValue1)));
                hashString(key, option.value.stringValue0 ? option.value.stringValue0 : "");
                hashString(key, option.value.stringValue1 ? option.value.stringValue1 : "");
            }
        }

        // Every file of the folder, by its path in the folder and its bytes, in path order. The shaders include these
        // files, so an edit of any of them gives every shader a new key.
        bool hashFolder(CacheKey &key, const std::filesystem::path &folder)
        {
            std::error_code error;
            std::vector<std::filesystem::path> files;
            // increment with an error code: a file removed while the folder is read must not throw.
            for (auto it = std::filesystem::recursive_directory_iterator(folder, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
            {
                std::error_code fileError;
                if (it->is_regular_file(fileError))
                    files.push_back(it->path());
            }
            if (error)
                return false;
            std::sort(files.begin(), files.end());
            for (const auto &file : files)
            {
                std::ifstream stream(file, std::ios::binary);
                if (!stream)
                    return false;
                const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                hashString(key, pathToUtf8(file.lexically_relative(folder)));
                hashString(key, bytes);
            }
            return true;
        }

        bool isInside(const std::filesystem::path &path, const std::filesystem::path &folder)
        {
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::weakly_canonical(path, error).lexically_relative(folder);
            return !error && !relative.empty() && *relative.begin() != "..";
        }

        bool validateInstanceTransform(const StructLayout *transform)
        {
            return validateStruct(transform, "InstanceTransform", sizeof(gfx::InstanceTransform),
                                  {{"world", offsetof(gfx::InstanceTransform, world)},
                                   {"translationLow", offsetof(gfx::InstanceTransform, translationLow)},
                                   {"normalMatrix", offsetof(gfx::InstanceTransform, normalMatrix)},
                                   {"previousWorld", offsetof(gfx::InstanceTransform, previousWorld)},
                                   {"boundsCenter", offsetof(gfx::InstanceTransform, boundsCenter)},
                                   {"boundsRadius", offsetof(gfx::InstanceTransform, boundsRadius)},
                                   {"skinnedVertex", offsetof(gfx::InstanceTransform, skinnedVertex)},
                                   {"skinnedMotion", offsetof(gfx::InstanceTransform, skinnedMotion)}});
        }

        bool validateGeometryData(const StructLayout *geometry)
        {
            return validateStruct(geometry, "GeometryData", sizeof(gfx::GeometryData),
                                  {{"vertexOffset", offsetof(gfx::GeometryData, vertexOffset)},
                                   {"boundsCenter", offsetof(gfx::GeometryData, boundsCenter)},
                                   {"boundsRadius", offsetof(gfx::GeometryData, boundsRadius)},
                                   {"lodCount", offsetof(gfx::GeometryData, lodCount)},
                                   {"lods", offsetof(gfx::GeometryData, lods)},
                                   {"colorOffset", offsetof(gfx::GeometryData, colorOffset)},
                                   {"skinOffset", offsetof(gfx::GeometryData, skinOffset)},
                                   {"morphOffset", offsetof(gfx::GeometryData, morphOffset)},
                                   {"morphTargetCount", offsetof(gfx::GeometryData, morphTargetCount)}});
        }

        // The Animate and Skin passes: pc -> frame -> the table, the mesh data in the pool, the transforms and the
        // geometry pool.
        bool validateAnimationStructs(const StructLayouts &layouts, const StructLayout *pushConstants)
        {
            auto frame = fieldStruct(layouts, pushConstants, "frame");
            auto entity = fieldStruct(layouts, frame, "entities");
            auto mesh = fieldStruct(layouts, entity, "mesh");
            bool valid = validateStruct(pushConstants, "AnimatePushConstants", sizeof(gfx::AnimatePushConstants),
                                        {{"frame", offsetof(gfx::AnimatePushConstants, frame)},
                                         {"zero", offsetof(gfx::AnimatePushConstants, zero)},
                                         {"padding", offsetof(gfx::AnimatePushConstants, padding)}});
            valid &= validateStruct(frame, "AnimationFrame", sizeof(gfx::AnimationFrame),
                                    {{"entities", offsetof(gfx::AnimationFrame, entities)},
                                     {"transforms", offsetof(gfx::AnimationFrame, transforms)},
                                     {"previousTransforms", offsetof(gfx::AnimationFrame, previousTransforms)},
                                     {"pose", offsetof(gfx::AnimationFrame, pose)},
                                     {"clock", offsetof(gfx::AnimationFrame, clock)},
                                     {"sync", offsetof(gfx::AnimationFrame, sync)},
                                     {"entityCount", offsetof(gfx::AnimationFrame, entityCount)},
                                     {"previousSync", offsetof(gfx::AnimationFrame, previousSync)},
                                     {"joints", offsetof(gfx::AnimationFrame, joints)},
                                     {"vertices", offsetof(gfx::AnimationFrame, vertices)},
                                     {"skins", offsetof(gfx::AnimationFrame, skins)},
                                     {"geometry", offsetof(gfx::AnimationFrame, geometry)},
                                     {"skinnedVertices", offsetof(gfx::AnimationFrame, skinnedVertices)},
                                     {"weights", offsetof(gfx::AnimationFrame, weights)},
                                     {"weightOverrides", offsetof(gfx::AnimationFrame, weightOverrides)},
                                     {"morphs", offsetof(gfx::AnimationFrame, morphs)}});
            valid &= validateStruct(entity, "AnimatedEntity", sizeof(gfx::AnimatedEntity),
                                    {{"entity", offsetof(gfx::AnimatedEntity, entity)},
                                     {"translationLow", offsetof(gfx::AnimatedEntity, translationLow)},
                                     {"rowPadding", offsetof(gfx::AnimatedEntity, rowPadding)},
                                     {"mesh", offsetof(gfx::AnimatedEntity, mesh)},
                                     {"phase", offsetof(gfx::AnimatedEntity, phase)},
                                     {"speed", offsetof(gfx::AnimatedEntity, speed)},
                                     {"duration", offsetof(gfx::AnimatedEntity, duration)},
                                     {"clip", offsetof(gfx::AnimatedEntity, clip)},
                                     {"loop", offsetof(gfx::AnimatedEntity, loop)},
                                     {"firstTransform", offsetof(gfx::AnimatedEntity, firstTransform)},
                                     {"firstNode", offsetof(gfx::AnimatedEntity, firstNode)},
                                     {"resetSync", offsetof(gfx::AnimatedEntity, resetSync)},
                                     {"firstJoint", offsetof(gfx::AnimatedEntity, firstJoint)},
                                     {"firstSkinnedVertex", offsetof(gfx::AnimatedEntity, firstSkinnedVertex)},
                                     {"firstWeight", offsetof(gfx::AnimatedEntity, firstWeight)},
                                     {"padding", offsetof(gfx::AnimatedEntity, padding)}});
            valid &= validateStruct(mesh, "AnimationMeshData", sizeof(gfx::AnimationMeshData),
                                    {{"nodes", offsetof(gfx::AnimationMeshData, nodes)},
                                     {"samplers", offsetof(gfx::AnimationMeshData, samplers)},
                                     {"channels", offsetof(gfx::AnimationMeshData, channels)},
                                     {"subMeshes", offsetof(gfx::AnimationMeshData, subMeshes)},
                                     {"keys", offsetof(gfx::AnimationMeshData, keys)},
                                     {"joints", offsetof(gfx::AnimationMeshData, joints)},
                                     {"jointSpheres", offsetof(gfx::AnimationMeshData, jointSpheres)},
                                     {"weights", offsetof(gfx::AnimationMeshData, weights)},
                                     {"morphRadii", offsetof(gfx::AnimationMeshData, morphRadii)},
                                     {"nodeCount", offsetof(gfx::AnimationMeshData, nodeCount)},
                                     {"subMeshCount", offsetof(gfx::AnimationMeshData, subMeshCount)},
                                     {"jointCount", offsetof(gfx::AnimationMeshData, jointCount)},
                                     {"skinnedVertexCount", offsetof(gfx::AnimationMeshData, skinnedVertexCount)},
                                     {"weightCount", offsetof(gfx::AnimationMeshData, weightCount)},
                                     {"padding", offsetof(gfx::AnimationMeshData, padding)}});
            valid &= validateStruct(fieldStruct(layouts, mesh, "subMeshes"), "AnimationSubMeshData", sizeof(gfx::AnimationSubMeshData),
                                    {{"node", offsetof(gfx::AnimationSubMeshData, node)},
                                     {"jointCount", offsetof(gfx::AnimationSubMeshData, jointCount)},
                                     {"firstJoint", offsetof(gfx::AnimationSubMeshData, firstJoint)},
                                     {"firstSphere", offsetof(gfx::AnimationSubMeshData, firstSphere)},
                                     {"geometryIndex", offsetof(gfx::AnimationSubMeshData, geometryIndex)},
                                     {"vertexCount", offsetof(gfx::AnimationSubMeshData, vertexCount)},
                                     {"firstSkinnedVertex", offsetof(gfx::AnimationSubMeshData, firstSkinnedVertex)},
                                     {"firstWeight", offsetof(gfx::AnimationSubMeshData, firstWeight)},
                                     {"firstMorphRadius", offsetof(gfx::AnimationSubMeshData, firstMorphRadius)}});
            valid &= validateStruct(fieldStruct(layouts, mesh, "joints"), "AnimationJointData", sizeof(gfx::AnimationJointData),
                                    {{"inverseBind", offsetof(gfx::AnimationJointData, inverseBind)},
                                     {"node", offsetof(gfx::AnimationJointData, node)}});
            valid &= validateStruct(fieldStruct(layouts, frame, "skins"), "SkinVertex", sizeof(gfx::SkinVertex),
                                    {{"joints", offsetof(gfx::SkinVertex, joints)},
                                     {"weights", offsetof(gfx::SkinVertex, weights)}});
            valid &= validateStruct(fieldStruct(layouts, frame, "morphs"), "MorphDelta", sizeof(gfx::MorphDelta),
                                    {{"position", offsetof(gfx::MorphDelta, position)},
                                     {"normalTangent", offsetof(gfx::MorphDelta, normalTangent)}});
            valid &= validateGeometryData(fieldStruct(layouts, frame, "geometry"));
            valid &= validateStruct(fieldStruct(layouts, mesh, "nodes"), "AnimationNodeData", sizeof(gfx::AnimationNodeData),
                                    {{"rest", offsetof(gfx::AnimationNodeData, rest)},
                                     {"translation", offsetof(gfx::AnimationNodeData, translation)},
                                     {"parent", offsetof(gfx::AnimationNodeData, parent)},
                                     {"rotation", offsetof(gfx::AnimationNodeData, rotation)},
                                     {"scale", offsetof(gfx::AnimationNodeData, scale)},
                                     {"padding", offsetof(gfx::AnimationNodeData, padding)}});
            valid &= validateStruct(fieldStruct(layouts, mesh, "samplers"), "AnimationSamplerData", sizeof(gfx::AnimationSamplerData),
                                    {{"firstTime", offsetof(gfx::AnimationSamplerData, firstTime)},
                                     {"firstValue", offsetof(gfx::AnimationSamplerData, firstValue)},
                                     {"keyCount", offsetof(gfx::AnimationSamplerData, keyCount)},
                                     {"interpolation", offsetof(gfx::AnimationSamplerData, interpolation)}});
            valid &= validateInstanceTransform(fieldStruct(layouts, frame, "transforms"));
            return valid;
        }

        // pushConstants names the struct of the program's pc, and layouts has every struct pc reaches, as Slang laid them
        // out (collectStructLayouts), from this compile or from the cache.
        bool validateRasmStructs(const std::string &pcName, const StructLayouts &layouts)
        {
            const auto found = layouts.find(pcName);
            const StructLayout *pushConstants = found != layouts.end() ? &found->second : nullptr;

            // The tone-map pass, the MSAA resolve, TAA, the bloom passes, the environment bake, the UI pass and the Inspect
            // pass have their own push constants.
            if (pcName == "ToneMapPushConstants")
            {
                return validateStruct(pushConstants, "ToneMapPushConstants", sizeof(gfx::ToneMapPushConstants),
                                      {{"source", offsetof(gfx::ToneMapPushConstants, source)},
                                       {"exposure", offsetof(gfx::ToneMapPushConstants, exposure)},
                                       {"bloom", offsetof(gfx::ToneMapPushConstants, bloom)},
                                       {"bloomStrength", offsetof(gfx::ToneMapPushConstants, bloomStrength)}});
            }
            if (pcName == "ResolvePushConstants")
            {
                return validateStruct(pushConstants, "ResolvePushConstants", sizeof(gfx::ResolvePushConstants),
                                      {{"source", offsetof(gfx::ResolvePushConstants, source)},
                                       {"target", offsetof(gfx::ResolvePushConstants, target)},
                                       {"samples", offsetof(gfx::ResolvePushConstants, samples)},
                                       {"exposure", offsetof(gfx::ResolvePushConstants, exposure)}});
            }
            if (pcName == "TaaPushConstants")
            {
                return validateStruct(pushConstants, "TaaPushConstants", sizeof(gfx::TaaPushConstants),
                                      {{"colorAndMotion", offsetof(gfx::TaaPushConstants, colorAndMotion)},
                                       {"depthAndHistory", offsetof(gfx::TaaPushConstants, depthAndHistory)},
                                       {"targetAndReset", offsetof(gfx::TaaPushConstants, targetAndReset)},
                                       {"exposure", offsetof(gfx::TaaPushConstants, exposure)}});
            }
            if (pcName == "BloomPushConstants")
            {
                return validateStruct(pushConstants, "BloomPushConstants", sizeof(gfx::BloomPushConstants),
                                      {{"source", offsetof(gfx::BloomPushConstants, source)},
                                       {"target", offsetof(gfx::BloomPushConstants, target)},
                                       {"base", offsetof(gfx::BloomPushConstants, base)},
                                       {"scale", offsetof(gfx::BloomPushConstants, scale)}});
            }
            if (pcName == "UiPushConstants")
            {
                return validateStruct(pushConstants, "UiPushConstants", sizeof(gfx::UiPushConstants),
                                      {{"vertices", offsetof(gfx::UiPushConstants, vertices)},
                                       {"texture", offsetof(gfx::UiPushConstants, texture)},
                                       {"displaySize", offsetof(gfx::UiPushConstants, displaySize)}});
            }
            if (pcName == "InspectPushConstants")
            {
                bool valid = validateStruct(pushConstants, "InspectPushConstants", sizeof(gfx::InspectPushConstants),
                                            {{"params", offsetof(gfx::InspectPushConstants, params)},
                                             {"padding", offsetof(gfx::InspectPushConstants, padding)}});
                return validateStruct(fieldStruct(layouts, pushConstants, "params"), "InspectParams", sizeof(gfx::InspectParams),
                                      {{"scene", offsetof(gfx::InspectParams, scene)},
                                       {"mode", offsetof(gfx::InspectParams, mode)},
                                       {"source", offsetof(gfx::InspectParams, source)},
                                       {"target", offsetof(gfx::InspectParams, target)},
                                       {"flags", offsetof(gfx::InspectParams, flags)},
                                       {"channel", offsetof(gfx::InspectParams, channel)},
                                       {"face", offsetof(gfx::InspectParams, face)},
                                       {"level", offsetof(gfx::InspectParams, level)},
                                       {"sample", offsetof(gfx::InspectParams, sample)},
                                       {"sourceWidth", offsetof(gfx::InspectParams, sourceWidth)},
                                       {"sourceHeight", offsetof(gfx::InspectParams, sourceHeight)},
                                       {"width", offsetof(gfx::InspectParams, width)},
                                       {"height", offsetof(gfx::InspectParams, height)},
                                       {"exposure", offsetof(gfx::InspectParams, exposure)},
                                       {"depthRange", offsetof(gfx::InspectParams, depthRange)}}) &&
                       valid;
            }
            if (pcName == "ExportPushConstants")
            {
                bool valid = validateStruct(pushConstants, "ExportPushConstants", sizeof(gfx::ExportPushConstants),
                                            {{"params", offsetof(gfx::ExportPushConstants, params)},
                                             {"padding", offsetof(gfx::ExportPushConstants, padding)}});
                return validateStruct(fieldStruct(layouts, pushConstants, "params"), "ExportParams", sizeof(gfx::ExportParams),
                                      {{"accumPtr", offsetof(gfx::ExportParams, accumPtr)},
                                       {"outputPtr", offsetof(gfx::ExportParams, outputPtr)},
                                       {"source", offsetof(gfx::ExportParams, source)},
                                       {"bloom", offsetof(gfx::ExportParams, bloom)},
                                       {"exposure", offsetof(gfx::ExportParams, exposure)},
                                       {"bloomStrength", offsetof(gfx::ExportParams, bloomStrength)},
                                       {"offsetX", offsetof(gfx::ExportParams, offsetX)},
                                       {"offsetY", offsetof(gfx::ExportParams, offsetY)},
                                       {"width", offsetof(gfx::ExportParams, width)},
                                       {"height", offsetof(gfx::ExportParams, height)},
                                       {"weight", offsetof(gfx::ExportParams, weight)},
                                       {"flags", offsetof(gfx::ExportParams, flags)},
                                       {"output", offsetof(gfx::ExportParams, output)},
                                       {"padding", offsetof(gfx::ExportParams, padding)}}) &&
                       valid;
            }
            if (pcName == "EnvironmentPushConstants")
            {
                return validateStruct(pushConstants, "EnvironmentPushConstants", sizeof(gfx::EnvironmentPushConstants),
                                      {{"source", offsetof(gfx::EnvironmentPushConstants, source)},
                                       {"target", offsetof(gfx::EnvironmentPushConstants, target)},
                                       {"roughness", offsetof(gfx::EnvironmentPushConstants, roughness)},
                                       {"size", offsetof(gfx::EnvironmentPushConstants, size)}});
            }

            if (pcName == "AnimatePushConstants")
            {
                return validateAnimationStructs(layouts, pushConstants);
            }

            // Custom pipelines bring their own push constants and do not share core.slang.
            const StructMember *sceneMember = findMember(pushConstants, "scene");
            if (!sceneMember || sceneMember->structName != "SceneData")
            {
                return true;
            }
            auto scene = fieldStruct(layouts, pushConstants, "scene");

            auto materialData = fieldStruct(layouts, scene, "materialsPtr");
            auto material = fieldStruct(layouts, materialData, "material");

            bool valid = true;

            valid &= validateStruct(pushConstants, "PushConstants", sizeof(gfx::PushConstants),
                                    {{"scene", offsetof(gfx::PushConstants, scene)},
                                     {"firstDraw", offsetof(gfx::PushConstants, firstDraw)},
                                     {"argument", offsetof(gfx::PushConstants, argument)}});

            valid &= validateStruct(scene, "SceneData", sizeof(gfx::SceneData),
                                    {{"projection", offsetof(gfx::SceneData, projection)},
                                     {"view", offsetof(gfx::SceneData, view)},
                                     {"originHigh", offsetof(gfx::SceneData, originHigh)},
                                     {"shadowedLightCount", offsetof(gfx::SceneData, shadowedLightCount)},
                                     {"vertsPtr", offsetof(gfx::SceneData, vertsPtr)},
                                     {"drawInfoPtr", offsetof(gfx::SceneData, drawInfoPtr)},
                                     {"instancesPtr", offsetof(gfx::SceneData, instancesPtr)},
                                     {"materialsPtr", offsetof(gfx::SceneData, materialsPtr)},
                                     {"geometryPtr", offsetof(gfx::SceneData, geometryPtr)},
                                     {"allDrawsPtr", offsetof(gfx::SceneData, allDrawsPtr)},
                                     {"drawCountsPtr", offsetof(gfx::SceneData, drawCountsPtr)},
                                     {"cullSlotsPtr", offsetof(gfx::SceneData, cullSlotsPtr)},
                                     {"groupCountsPtr", offsetof(gfx::SceneData, groupCountsPtr)},
                                     {"cullStatsPtr", offsetof(gfx::SceneData, cullStatsPtr)},
                                     {"visibilityPtr", offsetof(gfx::SceneData, visibilityPtr)},
                                     {"hizPtr", offsetof(gfx::SceneData, hizPtr)},
                                     {"colorsPtr", offsetof(gfx::SceneData, colorsPtr)},
                                     {"sortKeysPtr", offsetof(gfx::SceneData, sortKeysPtr)},
                                     {"sortDrawsPtr", offsetof(gfx::SceneData, sortDrawsPtr)},
                                     {"lightsPtr", offsetof(gfx::SceneData, lightsPtr)},
                                     {"sceneLightsPtr", offsetof(gfx::SceneData, sceneLightsPtr)},
                                     {"clustersPtr", offsetof(gfx::SceneData, clustersPtr)},
                                     {"frustumPlanes", offsetof(gfx::SceneData, frustumPlanes)},
                                     {"drawCount", offsetof(gfx::SceneData, drawCount)},
                                     {"runCount", offsetof(gfx::SceneData, runCount)},
                                     {"runFirst", offsetof(gfx::SceneData, runFirst)},
                                     {"blendRun", offsetof(gfx::SceneData, blendRun)},
                                     {"pointRun", offsetof(gfx::SceneData, pointRun)},
                                     {"sortCapacity", offsetof(gfx::SceneData, sortCapacity)},
                                     {"hizLevelCount", offsetof(gfx::SceneData, hizLevelCount)},
                                     {"depthTexture", offsetof(gfx::SceneData, depthTexture)},
                                     {"viewportWidth", offsetof(gfx::SceneData, viewportWidth)},
                                     {"viewportHeight", offsetof(gfx::SceneData, viewportHeight)},
                                     {"lodErrorPixels", offsetof(gfx::SceneData, lodErrorPixels)},
                                     {"forcedLod", offsetof(gfx::SceneData, forcedLod)},
                                     {"shadowMaps", offsetof(gfx::SceneData, shadowMaps)},
                                     {"lodPixelsPerUnit", offsetof(gfx::SceneData, lodPixelsPerUnit)},
                                     {"lodDepthPlane", offsetof(gfx::SceneData, lodDepthPlane)},
                                     {"environmentSpecular", offsetof(gfx::SceneData, environmentSpecular)},
                                     {"environmentSpecularMips", offsetof(gfx::SceneData, environmentSpecularMips)},
                                     {"environmentIrradiance", offsetof(gfx::SceneData, environmentIrradiance)},
                                     {"environmentSky", offsetof(gfx::SceneData, environmentSky)},
                                     {"brdfLut", offsetof(gfx::SceneData, brdfLut)},
                                     {"environmentIntensity", offsetof(gfx::SceneData, environmentIntensity)},
                                     {"environmentRotation", offsetof(gfx::SceneData, environmentRotation)},
                                     {"shadowedLights", offsetof(gfx::SceneData, shadowedLights)},
                                     {"globalLightCount", offsetof(gfx::SceneData, globalLightCount)},
                                     {"localLightCount", offsetof(gfx::SceneData, localLightCount)},
                                     {"nearZ", offsetof(gfx::SceneData, nearZ)},
                                     {"depthSamples", offsetof(gfx::SceneData, depthSamples)},
                                     {"previousViewProjection", offsetof(gfx::SceneData, previousViewProjection)},
                                     {"jitter", offsetof(gfx::SceneData, jitter)},
                                     {"textureLodBias", offsetof(gfx::SceneData, textureLodBias)},
                                     {"debugView", offsetof(gfx::SceneData, debugView)},
                                     {"animatedTransformsPtr", offsetof(gfx::SceneData, animatedTransformsPtr)},
                                     {"skinnedVertsPtr", offsetof(gfx::SceneData, skinnedVertsPtr)},
                                     {"previousSkinnedVertsPtr", offsetof(gfx::SceneData, previousSkinnedVertsPtr)},
                                     {"meshletsPtr", offsetof(gfx::SceneData, meshletsPtr)},
                                     {"meshletDrawsPtr", offsetof(gfx::SceneData, meshletDrawsPtr)},
                                     {"meshletChunksPtr", offsetof(gfx::SceneData, meshletChunksPtr)},
                                     {"chunkMasksPtr", offsetof(gfx::SceneData, chunkMasksPtr)},
                                     {"chunkSumsPtr", offsetof(gfx::SceneData, chunkSumsPtr)},
                                     {"meshletArgsPtr", offsetof(gfx::SceneData, meshletArgsPtr)},
                                     {"meshletDrawCountsPtr", offsetof(gfx::SceneData, meshletDrawCountsPtr)},
                                     {"groupMeshletsPtr", offsetof(gfx::SceneData, groupMeshletsPtr)},
                                     {"meshletVisibilityPtr", offsetof(gfx::SceneData, meshletVisibilityPtr)},
                                     {"cullViewPoint", offsetof(gfx::SceneData, cullViewPoint)},
                                     {"runChunkFirst", offsetof(gfx::SceneData, runChunkFirst)},
                                     {"runMeshletFirst", offsetof(gfx::SceneData, runMeshletFirst)},
                                     {"runMeshletBudget", offsetof(gfx::SceneData, runMeshletBudget)},
                                     {"meshletRuns", offsetof(gfx::SceneData, meshletRuns)},
                                     {"coneRuns", offsetof(gfx::SceneData, coneRuns)},
                                     {"viewFlags", offsetof(gfx::SceneData, viewFlags)},
                                     {"padding", offsetof(gfx::SceneData, padding)},
                                     {"meshletDataPtr", offsetof(gfx::SceneData, meshletDataPtr)},
                                     {"originLow", offsetof(gfx::SceneData, originLow)},
                                     {"zero", offsetof(gfx::SceneData, zero)},
                                     {"environmentOrientation", offsetof(gfx::SceneData, environmentOrientation)},
                                     {"labelsPtr", offsetof(gfx::SceneData, labelsPtr)},
                                     {"grid", offsetof(gfx::SceneData, grid)},
                                     {"labelProbesPtr", offsetof(gfx::SceneData, labelProbesPtr)},
                                     {"labelFadesPtr", offsetof(gfx::SceneData, labelFadesPtr)},
                                     {"labelFadeStep", offsetof(gfx::SceneData, labelFadeStep)},
                                     {"labelSlotCount", offsetof(gfx::SceneData, labelSlotCount)},
                                     {"pointsPtr", offsetof(gfx::SceneData, pointsPtr)},
                                     {"pointRasterPtr", offsetof(gfx::SceneData, pointRasterPtr)},
                                     {"pointBatchesPtr", offsetof(gfx::SceneData, pointBatchesPtr)},
                                     {"pointPixelsPtr", offsetof(gfx::SceneData, pointPixelsPtr)},
                                     {"pointBatchCapacity", offsetof(gfx::SceneData, pointBatchCapacity)},
                                     {"pointRasterPadding", offsetof(gfx::SceneData, pointRasterPadding)},
                                     {"glyphCullPtr", offsetof(gfx::SceneData, glyphCullPtr)},
                                     {"glyphChunksPtr", offsetof(gfx::SceneData, glyphChunksPtr)},
                                     {"glyphBatchesPtr", offsetof(gfx::SceneData, glyphBatchesPtr)},
                                     {"glyphCommandsPtr", offsetof(gfx::SceneData, glyphCommandsPtr)},
                                     {"glyphSlotsPtr", offsetof(gfx::SceneData, glyphSlotsPtr)},
                                     {"glyphListPtr", offsetof(gfx::SceneData, glyphListPtr)},
                                     {"glyphChunkCapacity", offsetof(gfx::SceneData, glyphChunkCapacity)},
                                     {"glyphBatchCapacity", offsetof(gfx::SceneData, glyphBatchCapacity)},
                                     {"weightedRun", offsetof(gfx::SceneData, weightedRun)},
                                     {"additiveRun", offsetof(gfx::SceneData, additiveRun)},
                                     {"weightedRangePtr", offsetof(gfx::SceneData, weightedRangePtr)},
                                     {"weightedDepths", offsetof(gfx::SceneData, weightedDepths)},
                                     {"weightedAccum", offsetof(gfx::SceneData, weightedAccum)},
                                     {"weightedCoverage", offsetof(gfx::SceneData, weightedCoverage)},
                                     {"weightedSamples", offsetof(gfx::SceneData, weightedSamples)},
                                     {"pixelScale", offsetof(gfx::SceneData, pixelScale)}});

            valid &= validateStruct(fieldStruct(layouts, scene, "pointRasterPtr"), "PointRasterHeader", sizeof(gfx::PointRasterHeader),
                                    {{"rasterGroups", offsetof(gfx::PointRasterHeader, rasterGroups)},
                                     {"rasterY", offsetof(gfx::PointRasterHeader, rasterY)},
                                     {"rasterZ", offsetof(gfx::PointRasterHeader, rasterZ)},
                                     {"clearGroups", offsetof(gfx::PointRasterHeader, clearGroups)},
                                     {"clearY", offsetof(gfx::PointRasterHeader, clearY)},
                                     {"clearZ", offsetof(gfx::PointRasterHeader, clearZ)},
                                     {"rectMinX", offsetof(gfx::PointRasterHeader, rectMinX)},
                                     {"rectMinY", offsetof(gfx::PointRasterHeader, rectMinY)},
                                     {"rectMaxX", offsetof(gfx::PointRasterHeader, rectMaxX)},
                                     {"rectMaxY", offsetof(gfx::PointRasterHeader, rectMaxY)},
                                     {"padding", offsetof(gfx::PointRasterHeader, padding)}});
            valid &= validateStruct(fieldStruct(layouts, scene, "glyphCullPtr"), "GlyphCullHeader", sizeof(gfx::GlyphCullHeader),
                                    {{"groups", offsetof(gfx::GlyphCullHeader, groups)},
                                     {"groupsY", offsetof(gfx::GlyphCullHeader, groupsY)},
                                     {"groupsZ", offsetof(gfx::GlyphCullHeader, groupsZ)},
                                     {"chunkCount", offsetof(gfx::GlyphCullHeader, chunkCount)},
                                     {"commandCount", offsetof(gfx::GlyphCullHeader, commandCount)},
                                     {"weightedCommandCount", offsetof(gfx::GlyphCullHeader, weightedCommandCount)},
                                     {"padding", offsetof(gfx::GlyphCullHeader, padding)}});
            valid &= validateStruct(fieldStruct(layouts, scene, "glyphChunksPtr"), "GlyphChunkSlot", sizeof(gfx::GlyphChunkSlot),
                                    {{"row", offsetof(gfx::GlyphChunkSlot, row)},
                                     {"firstBatch", offsetof(gfx::GlyphChunkSlot, firstBatch)},
                                     {"firstCommand", offsetof(gfx::GlyphChunkSlot, firstCommand)},
                                     {"lodCount", offsetof(gfx::GlyphChunkSlot, lodCount)},
                                     {"counts", offsetof(gfx::GlyphChunkSlot, counts)}});

            valid &= validateStruct(fieldStruct(layouts, scene, "meshletsPtr"), "GeometryMeshlet", sizeof(gfx::GeometryMeshlet),
                                    {{"center", offsetof(gfx::GeometryMeshlet, center)},
                                     {"radius", offsetof(gfx::GeometryMeshlet, radius)},
                                     {"coneAxis", offsetof(gfx::GeometryMeshlet, coneAxis)},
                                     {"coneCutoff", offsetof(gfx::GeometryMeshlet, coneCutoff)},
                                     {"firstIndex", offsetof(gfx::GeometryMeshlet, firstIndex)},
                                     {"indexCount", offsetof(gfx::GeometryMeshlet, indexCount)},
                                     {"firstData", offsetof(gfx::GeometryMeshlet, firstData)},
                                     {"vertexCount", offsetof(gfx::GeometryMeshlet, vertexCount)}});

            valid &= validateInstanceTransform(fieldStruct(layouts, scene, "animatedTransformsPtr"));

            valid &= validateStruct(fieldStruct(layouts, scene, "shadowedLights"), "ShadowedLight", sizeof(gfx::ShadowedLight),
                                    {{"light", offsetof(gfx::ShadowedLight, light)},
                                     {"firstMap", offsetof(gfx::ShadowedLight, firstMap)},
                                     {"mapCount", offsetof(gfx::ShadowedLight, mapCount)}});

            valid &= validateStruct(fieldStruct(layouts, scene, "lightsPtr"), "Light", sizeof(gfx::Light),
                                    {{"position", offsetof(gfx::Light, position)},
                                     {"intensity", offsetof(gfx::Light, intensity)},
                                     {"color", offsetof(gfx::Light, color)},
                                     {"range", offsetof(gfx::Light, range)},
                                     {"direction", offsetof(gfx::Light, direction)},
                                     {"type", offsetof(gfx::Light, type)},
                                     {"spotScale", offsetof(gfx::Light, spotScale)},
                                     {"spotOffset", offsetof(gfx::Light, spotOffset)},
                                     {"positionLow", offsetof(gfx::Light, positionLow)}});

            valid &= validateStruct(fieldStruct(layouts, scene, "shadowMaps"), "ShadowMap", sizeof(gfx::ShadowMap),
                                    {{"matrix", offsetof(gfx::ShadowMap, matrix)},
                                     {"texture", offsetof(gfx::ShadowMap, texture)},
                                     {"split", offsetof(gfx::ShadowMap, split)}});

            valid &= validateStruct(fieldStruct(layouts, scene, "grid"), "GridData", sizeof(gfx::GridData),
                                    {{"axisX", offsetof(gfx::GridData, axisX)},
                                     {"height", offsetof(gfx::GridData, height)},
                                     {"axisZ", offsetof(gfx::GridData, axisZ)},
                                     {"cellSize", offsetof(gfx::GridData, cellSize)},
                                     {"normal", offsetof(gfx::GridData, normal)},
                                     {"subdivisions", offsetof(gfx::GridData, subdivisions)},
                                     {"residueX", offsetof(gfx::GridData, residueX)},
                                     {"residueZ", offsetof(gfx::GridData, residueZ)},
                                     {"fractionX", offsetof(gfx::GridData, fractionX)},
                                     {"fractionZ", offsetof(gfx::GridData, fractionZ)},
                                     {"originCells", offsetof(gfx::GridData, originCells)},
                                     {"extent", offsetof(gfx::GridData, extent)},
                                     {"minorColor", offsetof(gfx::GridData, minorColor)},
                                     {"majorColor", offsetof(gfx::GridData, majorColor)},
                                     {"xAxisColor", offsetof(gfx::GridData, xAxisColor)},
                                     {"zAxisColor", offsetof(gfx::GridData, zAxisColor)},
                                     {"lineWidth", offsetof(gfx::GridData, lineWidth)},
                                     {"majorLineWidth", offsetof(gfx::GridData, majorLineWidth)},
                                     {"fadeDistance", offsetof(gfx::GridData, fadeDistance)},
                                     {"levelCount", offsetof(gfx::GridData, levelCount)},
                                     {"period", offsetof(gfx::GridData, period)},
                                     {"zero", offsetof(gfx::GridData, zero)}});

            valid &= validateStruct(fieldStruct(layouts, scene, "vertsPtr"), "VertexFull", sizeof(gfx::VertexFull),
                                    {{"pos", offsetof(gfx::VertexFull, pos)},
                                     {"normal", offsetof(gfx::VertexFull, normal)},
                                     {"uv", offsetof(gfx::VertexFull, uv)},
                                     {"tangent", offsetof(gfx::VertexFull, tangent)}});

            valid &= validateStruct(fieldStruct(layouts, scene, "drawInfoPtr"), "DrawInfo", sizeof(gfx::DrawInfo),
                                    {{"indexCount", offsetof(gfx::DrawInfo, indexCount)},
                                     {"instanceCount", offsetof(gfx::DrawInfo, instanceCount)},
                                     {"firstIndex", offsetof(gfx::DrawInfo, firstIndex)},
                                     {"vertexOffset", offsetof(gfx::DrawInfo, vertexOffset)},
                                     {"firstInstance", offsetof(gfx::DrawInfo, firstInstance)}});

            valid &= validateStruct(fieldStruct(layouts, scene, "instancesPtr"), "InstanceData", sizeof(gfx::InstanceData),
                                    {{"world", offsetof(gfx::InstanceData, world)},
                                     {"translationLow", offsetof(gfx::InstanceData, translationLow)},
                                     {"normalMatrix", offsetof(gfx::InstanceData, normalMatrix)},
                                     {"geometryIndex", offsetof(gfx::InstanceData, geometryIndex)},
                                     {"materialIndex", offsetof(gfx::InstanceData, materialIndex)},
                                     {"previousWorld", offsetof(gfx::InstanceData, previousWorld)},
                                     {"animationRow", offsetof(gfx::InstanceData, animationRow)},
                                     {"meshletVisibility", offsetof(gfx::InstanceData, meshletVisibility)}});

            valid &= validateGeometryData(fieldStruct(layouts, scene, "geometryPtr"));

            valid &= validateStruct(fieldStruct(layouts, fieldStruct(layouts, scene, "geometryPtr"), "lods"), "GeometryLod", sizeof(gfx::GeometryLod),
                                    {{"firstIndex", offsetof(gfx::GeometryLod, firstIndex)},
                                     {"indexCount", offsetof(gfx::GeometryLod, indexCount)},
                                     {"error", offsetof(gfx::GeometryLod, error)},
                                     {"firstMeshlet", offsetof(gfx::GeometryLod, firstMeshlet)},
                                     {"meshletCount", offsetof(gfx::GeometryLod, meshletCount)}});

            valid &= validateStruct(materialData, "MaterialData", sizeof(gfx::MaterialData),
                                    {{"material", offsetof(gfx::MaterialData, material)},
                                     {"materialId", offsetof(gfx::MaterialData, materialId)},
                                     {"alphaMode", offsetof(gfx::MaterialData, alphaMode)},
                                     {"alphaCutoff", offsetof(gfx::MaterialData, alphaCutoff)},
                                     {"flags", offsetof(gfx::MaterialData, flags)},
                                     {"textureTransforms", offsetof(gfx::MaterialData, textureTransforms)},
                                     {"terrain", offsetof(gfx::MaterialData, terrain)},
                                     {"vector", offsetof(gfx::MaterialData, vector)},
                                     {"points", offsetof(gfx::MaterialData, points)}});

            valid &= validateStruct(fieldStruct(layouts, materialData, "points"), "PointData", sizeof(gfx::PointData),
                                    {{"color", offsetof(gfx::PointData, color)},
                                     {"size", offsetof(gfx::PointData, size)},
                                     {"flags", offsetof(gfx::PointData, flags)},
                                     {"colormap", offsetof(gfx::PointData, colormap)},
                                     {"rangeMin", offsetof(gfx::PointData, rangeMin)},
                                     {"rangeMax", offsetof(gfx::PointData, rangeMax)},
                                     {"density", offsetof(gfx::PointData, density)}});

            valid &= validateStruct(fieldStruct(layouts, materialData, "vector"), "VectorData", sizeof(gfx::VectorData),
                                    {{"color", offsetof(gfx::VectorData, color)},
                                     {"outlineColor", offsetof(gfx::VectorData, outlineColor)},
                                     {"width", offsetof(gfx::VectorData, width)},
                                     {"outlineWidth", offsetof(gfx::VectorData, outlineWidth)},
                                     {"dashLength", offsetof(gfx::VectorData, dashLength)},
                                     {"gapLength", offsetof(gfx::VectorData, gapLength)},
                                     {"flags", offsetof(gfx::VectorData, flags)},
                                     {"drawOrder", offsetof(gfx::VectorData, drawOrder)},
                                     {"font", offsetof(gfx::VectorData, font)},
                                     {"distanceRange", offsetof(gfx::VectorData, distanceRange)}});

            valid &= validateStruct(fieldStruct(layouts, materialData, "terrain"), "TerrainData", sizeof(gfx::TerrainData),
                                    {{"heights", offsetof(gfx::TerrainData, heights)},
                                     {"heightKind", offsetof(gfx::TerrainData, heightKind)},
                                     {"heightScale", offsetof(gfx::TerrainData, heightScale)},
                                     {"heightOffset", offsetof(gfx::TerrainData, heightOffset)},
                                     {"minHeight", offsetof(gfx::TerrainData, minHeight)},
                                     {"skirt", offsetof(gfx::TerrainData, skirt)},
                                     {"sphereRadius", offsetof(gfx::TerrainData, sphereRadius)}});

            valid &= validateStruct(material, "Material", sizeof(gfx::Material),
                                    {{"metallicRoughness", offsetof(gfx::Material, pbr)},
                                     {"normal", offsetof(gfx::Material, normal)},
                                     {"occlusion", offsetof(gfx::Material, occlusion)},
                                     {"emissive", offsetof(gfx::Material, emissive)}});

            valid &= validateStruct(fieldStruct(layouts, materialData, "textureTransforms"), "TextureTransform", sizeof(gfx::TextureTransform),
                                    {{"matrix", offsetof(gfx::TextureTransform, matrix)},
                                     {"offset", offsetof(gfx::TextureTransform, offset)}});

            valid &= validateStruct(fieldStruct(layouts, material, "metallicRoughness"), "MetallicRoughnessMaterial", sizeof(gfx::MetallicRoughnessMaterial),
                                    {{"baseColorFactor", offsetof(gfx::MetallicRoughnessMaterial, baseColorFactor)},
                                     {"baseColorTextureIndex", offsetof(gfx::MetallicRoughnessMaterial, baseColorTextureIndex)},
                                     {"baseColorTextureCoordinate", offsetof(gfx::MetallicRoughnessMaterial, baseColorTextureCoordinate)},
                                     {"metallicRoughnessTextureIndex", offsetof(gfx::MetallicRoughnessMaterial, metallicRoughnessTextureIndex)},
                                     {"metallicRoughnessTextureCoordinate", offsetof(gfx::MetallicRoughnessMaterial, metallicRoughnessTextureCoordinate)},
                                     {"metallicFactor", offsetof(gfx::MetallicRoughnessMaterial, metallicFactor)},
                                     {"roughnessFactor", offsetof(gfx::MetallicRoughnessMaterial, roughnessFactor)}});

            valid &= validateStruct(fieldStruct(layouts, material, "normal"), "NormalMaterial", sizeof(gfx::NormalMaterial),
                                    {{"normalTextureIndex", offsetof(gfx::NormalMaterial, normalTextureIndex)},
                                     {"normalTextureCoordinate", offsetof(gfx::NormalMaterial, normalTextureCoordinate)},
                                     {"scale", offsetof(gfx::NormalMaterial, scale)}});

            valid &= validateStruct(fieldStruct(layouts, material, "occlusion"), "OcclusionMaterial", sizeof(gfx::OcclusionMaterial),
                                    {{"occlusionTextureIndex", offsetof(gfx::OcclusionMaterial, occlusionTextureIndex)},
                                     {"occlusionTextureCoordinate", offsetof(gfx::OcclusionMaterial, occlusionTextureCoordinate)},
                                     {"strength", offsetof(gfx::OcclusionMaterial, strength)}});

            valid &= validateStruct(fieldStruct(layouts, material, "emissive"), "EmissiveMaterial", sizeof(gfx::EmissiveMaterial),
                                    {{"emissiveTextureIndex", offsetof(gfx::EmissiveMaterial, emissiveTextureIndex)},
                                     {"emissiveTextureCoordinate", offsetof(gfx::EmissiveMaterial, emissiveTextureCoordinate)},
                                     {"emissiveFactor", offsetof(gfx::EmissiveMaterial, emissiveFactor)}});

            return valid;
        }

        // The returned desc points into options, which must outlive its use. Slang counts any texture gather
        // (shadow.slang) as spvImageGatherExtended and warns when the profile lacks it; plain gathers emit no such
        // SPIR-V capability, so naming it changes no code. With spvDemoteToHelperInvocation, discard becomes
        // OpDemoteToHelperInvocation instead of OpKill: the lane stays in its 2x2 quad as a helper, so the derivatives
        // of the samples after a cut-out's discard stay defined (B33). A NonUniformResourceIndex sample (pointRaster.slang's
        // resolve, which reads many materials' colormaps in one draw) makes Slang ask for the other capabilities below
        // and warn without them; they only allow what a shader uses, so the other shaders' code is the same.
        slang::TargetDesc getTargetDesc(slang::IGlobalSession *globalSession, ShaderCompiler::Target target, std::vector<slang::CompilerOptionEntry> &options)
        {
            static const char *const spirvCapabilities[] = {"spvImageGatherExtended", "spvDemoteToHelperInvocation", "spvShaderNonUniformEXT", "SPV_KHR_non_semantic_info", "SPV_GOOGLE_user_type",
                                                            "spvDerivativeControl", "spvImageQuery", "spvSparseResidency", "spvMinLod", "spvFragmentFullyCoveredEXT"};
            slang::TargetDesc targetDesc = {};
            options.clear();
            switch (target)
            {
            case ShaderCompiler::Target::SPIRV:
                options.push_back({slang::CompilerOptionName::EmitSpirvDirectly, {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}});
                for (const char *capability : spirvCapabilities)
                    options.push_back({slang::CompilerOptionName::Capability, {slang::CompilerOptionValueKind::Int, static_cast<int32_t>(globalSession->findCapability(capability)), 0, nullptr, nullptr}});
                targetDesc.format = SLANG_SPIRV;
                targetDesc.profile = globalSession->findProfile("spirv_1_4");
                targetDesc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());
                targetDesc.compilerOptionEntries = options.data();
                break;
            case ShaderCompiler::Target::MSL:
                targetDesc.format = SLANG_METAL;
                targetDesc.profile = globalSession->findProfile("metal_2_3");
                break;
            case ShaderCompiler::Target::HLSL:
                targetDesc.format = SLANG_HLSL;
                targetDesc.profile = globalSession->findProfile("sm_6_0");
                break;
            case ShaderCompiler::Target::GLSL:
                targetDesc.format = SLANG_GLSL;
                targetDesc.profile = globalSession->findProfile("glsl_4_5");
                break;
            case ShaderCompiler::Target::WGSL:
                targetDesc.format = SLANG_WGSL;
                break;
            default:
                spdlog::error("Unsupported shader target.");
                break;
            }
            return targetDesc;
        }

        void removeFromQueue(std::deque<std::string> &queue, const std::string &name)
        {
            const auto it = std::find(queue.begin(), queue.end(), name);
            if (it != queue.end())
                queue.erase(it);
        }

        void getSessionOptions(slang::CompilerOptionEntry (&options)[SESSION_OPTION_COUNT])
        {
            options[0] = {slang::CompilerOptionName::VulkanUseEntryPointName, {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}};
            options[1] = {slang::CompilerOptionName::MatrixLayoutColumn, {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}};
            options[2] = {slang::CompilerOptionName::GLSLForceScalarLayout, {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}};
        }
    }

    ShaderCompiler::ShaderCompiler() = default;

    ShaderCompiler::~ShaderCompiler() = default;

    // workerThreads compile the prefetched shaders; 0 leaves every compile to compileFromString's thread.
    bool ShaderCompiler::initialize(Target shaderTarget, const std::string &shaderDir, const std::string &cacheDir, uint32_t workerThreads)
    {
        const std::filesystem::path libFolder = pathFromUtf8(shaderDir) / "lib";
        const std::filesystem::path uberShader = pathFromUtf8(shaderDir) / "passes/rasm.slang";
        std::error_code errorCode;
        if (!std::filesystem::is_directory(libFolder, errorCode) || !std::filesystem::is_regular_file(uberShader, errorCode))
        {
            spdlog::error("Shaders not found in {}. Copy them there, or set RendererConfig::shaderDir.", shaderDir);
            return false;
        }
        spdlog::info("Loading shaders from {}", shaderDir);
        libDir = std::filesystem::weakly_canonical(libFolder, errorCode);
        target = shaderTarget;
        searchPath = pathToUtf8(libFolder);
        if (!createSlangContext(mainContext))
            return false;
        if (workerThreads > 0)
            workers = std::make_unique<WorkerPool>(workerThreads, UINT64_MAX, "RASM shaders");

        std::vector<slang::CompilerOptionEntry> targetOptions;
        const slang::TargetDesc targetDesc = getTargetDesc(mainContext.globalSession, target, targetOptions);
        slang::CompilerOptionEntry sessionOptions[SESSION_OPTION_COUNT] = {};
        getSessionOptions(sessionOptions);

        // The key starts with every input all shaders share: the compiler, its target and options, and the included files.
        if (cacheDir.empty())
            return true;
        hashNumber(baseKey, SHADER_CACHE_KEY_VERSION);
        hashString(baseKey, mainContext.globalSession->getBuildTagString());
        // The tag is the release ("2026.2"), which a patched build of the same release keeps; its library file does not.
        const std::filesystem::path slangLibrary = pathFromUtf8(getModulePath(reinterpret_cast<const void *>(&slang_createGlobalSession)));
        hashString(baseKey, pathToUtf8(slangLibrary.filename()));
        hashNumber(baseKey, static_cast<uint64_t>(std::filesystem::file_size(slangLibrary, errorCode)));
        hashNumber(baseKey, static_cast<uint64_t>(std::filesystem::last_write_time(slangLibrary, errorCode).time_since_epoch().count()));
        hashNumber(baseKey, static_cast<uint64_t>(targetDesc.format));
        hashNumber(baseKey, static_cast<uint64_t>(targetDesc.profile));
        hashOptions(baseKey, targetDesc.compilerOptionEntries, targetDesc.compilerOptionEntryCount);
        hashOptions(baseKey, sessionOptions, SESSION_OPTION_COUNT);
        const std::filesystem::path folder = pathFromUtf8(cacheDir) / "shaders";
        std::filesystem::create_directories(folder, errorCode);
        if (!hashFolder(baseKey, libDir) || errorCode)
        {
            spdlog::warn("Shaders are compiled without the cache: cannot read {} or make {}.", pathToUtf8(libDir), pathToUtf8(folder));
            return true;
        }
        cacheFolder = folder;
        pruneCacheFolder(cacheFolder, MAX_SHADER_CACHE_BYTES);
        return true;
    }

    // The queued prefetches are dropped and the running ones finish, so nothing compiles once the renderer goes.
    void ShaderCompiler::shutdown()
    {
        if (workers)
            workers->shutdown();
        {
            std::lock_guard lock(mutex);
            stopped = true;
        }
        prefetchDone.notify_all();
        const Stats done = getStats();
        if (done.cacheHits + done.compiles > 0)
            spdlog::info("Shaders: {} from the cache, {} compiled in {:.0f} ms ({} on the main thread).", done.cacheHits, done.compiles, done.compileSeconds * 1000.0, done.mainCompiles);
    }

    // Slang's global session is not thread-safe, and the objects made from one are used by one thread at a time
    // (slang.h, IGlobalSession), so each thread that compiles has a context of its own.
    bool ShaderCompiler::createSlangContext(SlangContext &context)
    {
        ZoneScoped;

        // A context whose compile failed keeps its global session and takes a new session: the old one keeps the
        // files it read, so a fix on disk, of a lib/ file too, would not be seen.
        context.modules.clear();
        context.session = nullptr;
        if (!context.globalSession)
        {
            const SlangResult result = createGlobalSession(context.globalSession.writeRef());
            if (SLANG_FAILED(result))
            {
                spdlog::error("Failed to create Slang global session. Error code: {}", result);
                return false;
            }
        }

        std::vector<slang::CompilerOptionEntry> targetOptions;
        slang::TargetDesc targetDesc = getTargetDesc(context.globalSession, target, targetOptions);
        const char *searchPaths[] = {searchPath.c_str()};
        slang::CompilerOptionEntry options[SESSION_OPTION_COUNT] = {};
        getSessionOptions(options);

        slang::SessionDesc sessionDesc = {};
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        sessionDesc.searchPaths = searchPaths;
        sessionDesc.searchPathCount = static_cast<uint32_t>(sizeof(searchPaths) / sizeof(searchPaths[0]));
        sessionDesc.compilerOptionEntries = options;
        sessionDesc.compilerOptionEntryCount = SESSION_OPTION_COUNT;

        const SlangResult sessionResult = context.globalSession->createSession(sessionDesc, context.session.writeRef());
        if (SLANG_FAILED(sessionResult))
        {
            spdlog::error("Failed to create Slang session. Error code: {}", sessionResult);
            return false;
        }
        return true;
    }

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

    // The source has the variant's #define lines, so with the base key this is every input of the SPIR-V. Without a
    // cache folder the base key is empty, and the key still tells the shaders apart.
    CacheKey ShaderCompiler::getKey(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName) const
    {
        CacheKey key = baseKey;
        hashString(key, outputPath);
        hashString(key, entryPointName);
        hashString(key, sourceCode);
        return key;
    }

    bool ShaderCompiler::readCachedEntry(const CacheKey &key, CachedShader &shader)
    {
        if (cacheFolder.empty() || !readCachedShader(cacheFolder / (toHex(key) + ".shader"), key, shader))
            return false;
        std::lock_guard lock(mutex);
        ++stats.cacheHits;
        return true;
    }

    bool ShaderCompiler::compileEntry(SlangContext &context, const CacheKey &key, const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName,
                                      CachedShader &shader, bool onWorker)
    {
        const auto start = std::chrono::steady_clock::now();
        // A context whose new session could not be made after a failure tries again.
        if (!context.session && !createSlangContext(context))
            return false;
        // A rejected cache entry may have filled some of shader before it failed.
        shader = {};
        CompiledShader compiled;
        bool cacheable = true;
        if (!compileWithSlang(context, sourceCode, outputPath, entryPointName, compiled, shader.pushConstants, shader.layouts, cacheable))
        {
            createSlangContext(context);
            return false;
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        shader.code = std::move(compiled.code);
        if (!cacheFolder.empty() && cacheable)
            writeCachedShader(cacheFolder / (toHex(key) + ".shader"), key, shader);
        {
            std::lock_guard lock(mutex);
            stats.compileSeconds += seconds;
            ++stats.compiles;
            stats.mainCompiles += onWorker ? 0 : 1;
        }
        // A warm-up that missed a pipeline's shader shows here (and in shutdown's count); an application's own
        // pipelines and the overlay's and the UI's are compiled here on purpose.
        if (!onWorker)
            spdlog::debug("'{}' compiled on the main thread in {:.0f} ms.", outputPath, seconds * 1000.0);
        return true;
    }

    // Every build checks, since a Release build with a mismatch would read shifted data on the GPU (B23). The layouts
    // are Slang's, from this compile or the one the cache kept, so a changed C++ struct is caught either way.
    ShaderCompiler::CompiledShader ShaderCompiler::useShader(const CachedShader &shader, const std::string &outputPath)
    {
        if (!validateRasmStructs(shader.pushConstants, shader.layouts))
        {
            spdlog::error("The C++ structs do not match shaders/lib/core.slang; '{}' is not compiled.", outputPath);
            return {};
        }
        for (const auto &[name, layout] : shader.layouts)
            structLayouts.try_emplace(name, layout);
        return {.code = shader.code};
    }

    // A prefetched shader is taken as its worker left it, waited for while a worker compiles it, or compiled here when no
    // worker has started it. Without wait, a shader no worker has finished is moved first in the queue and comes back
    // pending, so the caller draws without it this frame; a cache hit is read here, as it costs less than a frame.
    ShaderCompiler::CompiledShader ShaderCompiler::compileFromString(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName, bool wait)
    {
        ZoneScoped;

        const CacheKey key = getKey(sourceCode, outputPath, entryPointName);
        std::unique_lock lock(mutex);
        const auto found = prefetches.find(toHex(key));
        if (found == prefetches.end())
        {
            lock.unlock();
            CachedShader shader;
            if (readCachedEntry(key, shader))
                return useShader(shader, outputPath);
            if (!wait && workers)
            {
                const Request request = {.source = sourceCode, .outputPath = outputPath, .entryPoint = entryPointName};
                prefetch(&request, 1, true);
                return {.pending = true};
            }
            if (!compileEntry(mainContext, key, sourceCode, outputPath, entryPointName, shader, false))
                return {};
            return useShader(shader, outputPath);
        }

        // An entry is never erased, so the reference outlives the unlocks. A failed one is queued again, first, so a
        // shader fixed on disk is picked up while the application runs; with wait, this thread then compiles it.
        Prefetch &entry = found->second;
        if (entry.state == PrefetchState::FAILED)
        {
            entry.state = PrefetchState::QUEUED;
            ++openPrefetches;
            queue.push_front(found->first);
            workers->push({.run = [this]()
                           { runPrefetch(); }});
        }
        if (!wait && (entry.state == PrefetchState::QUEUED || entry.state == PrefetchState::RUNNING))
        {
            if (entry.state == PrefetchState::QUEUED)
            {
                removeFromQueue(queue, found->first);
                queue.push_front(found->first);
            }
            return {.pending = true};
        }
        // Taken off the queue, so no worker compiles it a second time.
        if (entry.state == PrefetchState::QUEUED)
        {
            entry.state = PrefetchState::RUNNING;
            removeFromQueue(queue, found->first);
            lock.unlock();
            CachedShader shader;
            const bool compiled = readCachedEntry(entry.key, shader) ||
                                  compileEntry(mainContext, entry.key, entry.request.source, entry.request.outputPath, entry.request.entryPoint, shader, false);
            lock.lock();
            entry.shader = std::move(shader);
            endPrefetch(lock, entry, compiled, false);
        }
        else if (entry.state == PrefetchState::RUNNING)
        {
            ZoneNamedN(waitZone, "waitForShader", true);
            prefetchDone.wait(lock, [&entry]()
                              { return entry.state != PrefetchState::RUNNING; });
        }
        if (entry.state == PrefetchState::FAILED)
            return {};
        // A DONE entry does not change any more.
        lock.unlock();
        return useShader(entry.shader, outputPath);
    }

    // Queues requests for the workers; first puts them before the ones queued already, in their order. A shader asked
    // for before is not queued again, but moved forward with first while no worker has it.
    void ShaderCompiler::prefetch(const Request *requests, uint32_t count, bool first)
    {
        ZoneScoped;

        if (!workers)
            return;
        std::vector<std::string> names;
        uint32_t added = 0;
        {
            std::lock_guard lock(mutex);
            for (uint32_t i = 0; i < count; ++i)
            {
                const CacheKey key = getKey(requests[i].source, requests[i].outputPath, requests[i].entryPoint);
                const auto [found, inserted] = prefetches.try_emplace(toHex(key));
                if (inserted)
                {
                    found->second.key = key;
                    hashString(found->second.sourceKey, requests[i].source);
                    found->second.request = requests[i];
                    ++openPrefetches;
                    ++added;
                    names.push_back(found->first);
                }
                else if (first && found->second.state == PrefetchState::QUEUED && std::find(names.begin(), names.end(), found->first) == names.end())
                {
                    removeFromQueue(queue, found->first);
                    names.push_back(found->first);
                }
            }
            queue.insert(first ? queue.begin() : queue.end(), names.begin(), names.end());
        }
        for (uint32_t i = 0; i < added; ++i)
            workers->push({.run = [this]()
                           { runPrefetch(); }});
    }

    // Returns once every queued shader is compiled or read; at once without workers, or after shutdown dropped some.
    void ShaderCompiler::waitForPrefetches()
    {
        ZoneScoped;

        std::unique_lock lock(mutex);
        prefetchDone.wait(lock, [this]()
                          { return openPrefetches == 0 || stopped; });
    }

    // One job per queued request, but a job takes whichever is first (the main thread may have moved or taken some),
    // with the other queued shaders of its source: they reuse the module it loads.
    void ShaderCompiler::runPrefetch()
    {
        ZoneScoped;

        std::unique_lock lock(mutex);
        std::vector<Prefetch *> group;
        while (!queue.empty() && group.empty())
        {
            Prefetch &front = prefetches.at(queue.front());
            queue.pop_front();
            if (front.state == PrefetchState::QUEUED)
            {
                front.state = PrefetchState::RUNNING;
                group.push_back(&front);
            }
        }
        if (group.empty())
            return;
        for (const std::string &name : queue)
        {
            Prefetch &other = prefetches.at(name);
            if (other.state == PrefetchState::QUEUED && other.sourceKey == group[0]->sourceKey)
            {
                other.state = PrefetchState::RUNNING;
                group.push_back(&other);
            }
        }
        lock.unlock();

        // A context is made only for a miss, so a run whose cache has every shader makes none.
        std::unique_ptr<SlangContext> context;
        for (size_t i = 0; i < group.size(); ++i)
        {
            Prefetch &entry = *group[i];
            CachedShader shader;
            bool compiled = readCachedEntry(entry.key, shader);
            if (!compiled && !context)
            {
                lock.lock();
                if (!idleContexts.empty())
                {
                    context = std::move(idleContexts.back());
                    idleContexts.pop_back();
                }
                lock.unlock();
                if (!context)
                {
                    context = std::make_unique<SlangContext>();
                    if (!createSlangContext(*context))
                        context.reset();
                    lock.lock();
                    stats.workerSessions += context ? 1 : 0;
                    lock.unlock();
                }
            }
            if (!compiled && context)
                compiled = compileEntry(*context, entry.key, entry.request.source, entry.request.outputPath, entry.request.entryPoint, shader, true);
            lock.lock();
            // Back before the last entry ends, so the end of the last open prefetch frees it with the others.
            if (i + 1 == group.size() && context)
            {
                idleContexts.push_back(std::move(context));
                context = nullptr;
            }
            entry.shader = std::move(shader);
            endPrefetch(lock, entry, compiled, true);
            lock.unlock();
        }
    }

    // Called with the lock held, which it leaves held. The last open prefetch frees the workers' Slang contexts, whose
    // sessions keep every module they loaded, on a worker: a session takes milliseconds to go.
    void ShaderCompiler::endPrefetch(std::unique_lock<std::mutex> &lock, Prefetch &entry, bool compiled, bool onWorker)
    {
        entry.state = compiled ? PrefetchState::DONE : PrefetchState::FAILED;
        if (compiled)
            entry.request.source = std::string();
        --openPrefetches;
        std::vector<std::unique_ptr<SlangContext>> freed;
        const bool release = openPrefetches == 0 && !idleContexts.empty();
        if (release && onWorker)
        {
            freed.swap(idleContexts);
            stats.workerSessions -= static_cast<uint32_t>(freed.size());
        }
        lock.unlock();
        prefetchDone.notify_all();
        if (release && !onWorker)
            workers->push({.run = [this]()
                           { releaseIdleContexts(); }});
        freed.clear();
        lock.lock();
    }

    void ShaderCompiler::releaseIdleContexts()
    {
        std::vector<std::unique_ptr<SlangContext>> freed;
        std::lock_guard lock(mutex);
        if (openPrefetches == 0)
        {
            freed.swap(idleContexts);
            stats.workerSessions -= static_cast<uint32_t>(freed.size());
        }
    }

    bool ShaderCompiler::compileWithSlang(SlangContext &context, const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName, CompiledShader &shader,
                                          std::string &pushConstants, StructLayouts &layouts, bool &cacheable)
    {
        ZoneScoped;

        // A module is loaded once per context and source: the stages of a pipeline and the entry points of one file
        // share it, and only its parse and check, the larger part of a compile, are skipped. The session keeps every
        // module and rejects a second one with the same name and different source, so the name is the source's hash;
        // the path names the first shader that loaded it, for the diagnostics.
        CacheKey sourceKey;
        hashString(sourceKey, sourceCode);
        const std::string moduleName = toHex(sourceKey);
        auto [loaded, inserted] = context.modules.try_emplace(moduleName);
        if (inserted)
        {
            ZoneNamedN(loadZone, "loadModule", true);
            loaded->second.path = buildPath + outputPath + "." + moduleName;
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;
            loaded->second.module = context.session->loadModuleFromSourceString(moduleName.c_str(), loaded->second.path.c_str(), sourceCode.c_str(), diagnosticsBlob.writeRef());
            diagnoseIfNeeded(diagnosticsBlob);
        }
        slang::IModule *slangModule = loaded->second.module;
        if (!slangModule)
            return false;
        const std::string &modulePath = loaded->second.path;

        // The key holds the files of lib/ only, so a module that read any other file is not cached.
        for (SlangInt32 i = 0; i < slangModule->getDependencyFileCount(); ++i)
        {
            const char *dependency = slangModule->getDependencyFilePath(i);
            if (dependency && modulePath != dependency && !isInside(pathFromUtf8(dependency), libDir))
            {
                spdlog::info("'{}' includes {}, outside {}, so it is not cached.", outputPath, dependency, pathToUtf8(libDir));
                cacheable = false;
            }
        }

        // Query Entry Points
        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        {
            slangModule->findEntryPointByName(entryPointName.c_str(), entryPoint.writeRef());
            if (!entryPoint)
            {
                spdlog::error("Error getting entry point: {}", entryPointName);
                return false;
            }
        }

        // Compose Modules + Entry Points
        std::array<slang::IComponentType *, 2> componentTypes = {slangModule,
                                                                 entryPoint};

        Slang::ComPtr<slang::IComponentType> composedProgram;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;
            auto result = context.session->createCompositeComponentType(componentTypes.data(),
                                                                        componentTypes.size(),
                                                                        composedProgram.writeRef(),
                                                                        diagnosticsBlob.writeRef());

            diagnoseIfNeeded(diagnosticsBlob);
            if (SLANG_FAILED(result))
            {
                spdlog::error("Error creating composite component type");
                return false;
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
                return false;
            }
        }

        // Walks pc through its pointers because only that reports the layout the shaders read: ProgramLayout::getTypeLayout
        // has no LayoutRules value for data behind a pointer, so it adds std140/std430 padding.
        slang::ProgramLayout *programLayout = linkedProgram->getLayout();
        for (unsigned int i = 0; i < programLayout->getParameterCount(); ++i)
        {
            auto parameter = programLayout->getParameterByIndex(i);
            if (std::strcmp(parameter->getName(), "pc") != 0)
                continue;
            slang::TypeLayoutReflection *pcLayout = parameter->getTypeLayout()->getElementTypeLayout();
            pushConstants = pcLayout ? getStructName(pcLayout) : "";
            collectStructLayouts(pcLayout, layouts);
        }

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
                return false;
            }
        }

        const auto *code = static_cast<const uint8_t *>(compiledBlob->getBufferPointer());
        shader.code.assign(code, code + compiledBlob->getBufferSize());
        return true;
    }

    ShaderCompiler::Stats ShaderCompiler::getStats() const
    {
        std::lock_guard lock(mutex);
        return stats;
    }

    const std::vector<StructField> *ShaderCompiler::findStructLayout(const std::string &name) const
    {
        const auto it = structLayouts.find(name);
        return it != structLayouts.end() ? &it->second.fields : nullptr;
    }
}