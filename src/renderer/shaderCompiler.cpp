#include "renderer/shaderCompiler.h"
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
                                     {"pointsPtr", offsetof(gfx::SceneData, pointsPtr)}});

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
    }

    bool ShaderCompiler::initialize(Target target, const std::string &shaderDir, const std::string &cacheDir)
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

        // Create Slang Global Session
        auto result = createGlobalSession(globalSession.writeRef());

        if (SLANG_FAILED(result))
        {
            spdlog::error("Failed to create Slang global session. Error code: {}", result);
            return false;
        }

        slang::TargetDesc targetDesc = getTargetDesc(target);

        const std::string pathLib = pathToUtf8(libFolder);

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

        // The key starts with every input all shaders share: the compiler, its target and options, and the included files.
        if (cacheDir.empty())
            return true;
        hashNumber(baseKey, SHADER_CACHE_KEY_VERSION);
        hashString(baseKey, globalSession->getBuildTagString());
        // The tag is the release ("2026.2"), which a patched build of the same release keeps; its library file does not.
        const std::filesystem::path slangLibrary = pathFromUtf8(getModulePath(reinterpret_cast<const void *>(&slang_createGlobalSession)));
        hashString(baseKey, pathToUtf8(slangLibrary.filename()));
        hashNumber(baseKey, static_cast<uint64_t>(std::filesystem::file_size(slangLibrary, errorCode)));
        hashNumber(baseKey, static_cast<uint64_t>(std::filesystem::last_write_time(slangLibrary, errorCode).time_since_epoch().count()));
        hashNumber(baseKey, static_cast<uint64_t>(targetDesc.format));
        hashNumber(baseKey, static_cast<uint64_t>(targetDesc.profile));
        hashOptions(baseKey, targetDesc.compilerOptionEntries, targetDesc.compilerOptionEntryCount);
        hashOptions(baseKey, sessionDesc.compilerOptionEntries, sessionDesc.compilerOptionEntryCount);
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

    void ShaderCompiler::shutdown()
    {
        if (stats.cacheHits + stats.compiles > 0)
            spdlog::info("Shaders: {} from the cache, {} compiled in {:.0f} ms.", stats.cacheHits, stats.compiles, stats.compileSeconds * 1000.0);
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

    ShaderCompiler::CompiledShader ShaderCompiler::compileFromString(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName)
    {
        ZoneScoped;

        // The source has the variant's #define lines, so with the base key this is every input of the SPIR-V.
        CacheKey key = baseKey;
        hashString(key, outputPath);
        hashString(key, entryPointName);
        hashString(key, sourceCode);
        const std::filesystem::path entryPath = cacheFolder.empty() ? std::filesystem::path() : cacheFolder / (toHex(key) + ".shader");

        CachedShader shader;
        if (!entryPath.empty() && readCachedShader(entryPath, key, shader))
        {
            ++stats.cacheHits;
        }
        else
        {
            const auto start = std::chrono::steady_clock::now();
            // A rejected entry may have filled some of shader before it failed.
            shader = {};
            CompiledShader compiled;
            bool cacheable = true;
            if (!compileWithSlang(sourceCode, outputPath, entryPointName, compiled, shader.pushConstants, shader.layouts, cacheable))
                return {};
            stats.compileSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            ++stats.compiles;
            shader.code = std::move(compiled.code);
            if (!entryPath.empty() && cacheable)
                writeCachedShader(entryPath, key, shader);
        }

        // Every build checks, since a Release build with a mismatch would read shifted data on the GPU (B23). The
        // layouts are Slang's, from this compile or the one the cache kept, so a changed C++ struct is caught either way.
        if (!validateRasmStructs(shader.pushConstants, shader.layouts))
        {
            spdlog::error("The C++ structs do not match shaders/lib/core.slang; '{}' is not compiled.", outputPath);
            return {};
        }
        structLayouts.merge(shader.layouts);

        CompiledShader result;
        result.code = std::move(shader.code);
        return result;
    }

    bool ShaderCompiler::compileWithSlang(const std::string &sourceCode, const std::string &outputPath, const std::string &entryPointName, CompiledShader &shader,
                                          std::string &pushConstants, StructLayouts &layouts, bool &cacheable)
    {
        ZoneScoped;

        // The session keeps every module and rejects a second one with the same name and different source,
        // so hash the source into the name.
        const auto moduleName = outputPath + "." + std::to_string(std::hash<std::string>{}(sourceCode));
        const std::string modulePath = buildPath + moduleName;

        // Load module
        Slang::ComPtr<slang::IModule> slangModule;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;

            slangModule = activeSession->loadModuleFromSourceString(moduleName.c_str(),          // Module name
                                                                    modulePath.c_str(),          // Module path
                                                                    sourceCode.c_str(),          // Shader source code
                                                                    diagnosticsBlob.writeRef()); // Optional diagnostic container

            diagnoseIfNeeded(diagnosticsBlob);

            if (!slangModule)
            {
                return false;
            }
        }

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
            auto result = activeSession->createCompositeComponentType(componentTypes.data(),
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

    const ShaderCompiler::Stats &ShaderCompiler::getStats() const
    {
        return stats;
    }

    const std::vector<StructField> *ShaderCompiler::findStructLayout(const std::string &name) const
    {
        const auto it = structLayouts.find(name);
        return it != structLayouts.end() ? &it->second.fields : nullptr;
    }

    slang::TargetDesc ShaderCompiler::getTargetDesc(Target target)
    {
        // The returned desc only points at these, so they must outlive this function. Slang counts any texture gather
        // (shadow.slang) as spvImageGatherExtended and warns when the profile lacks it; plain gathers emit no such
        // SPIR-V capability, so naming it changes no code. With spvDemoteToHelperInvocation, discard becomes
        // OpDemoteToHelperInvocation instead of OpKill: the lane stays in its 2x2 quad as a helper, so the derivatives
        // of the samples after a cut-out's discard stay defined (B33).
        static slang::CompilerOptionEntry spirvOptions[] = {{slang::CompilerOptionName::EmitSpirvDirectly, {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}},
                                                            {slang::CompilerOptionName::Capability, {slang::CompilerOptionValueKind::Int, 0, 0, nullptr, nullptr}},
                                                            {slang::CompilerOptionName::Capability, {slang::CompilerOptionValueKind::Int, 0, 0, nullptr, nullptr}}};
        spirvOptions[1].value.intValue0 = globalSession->findCapability("spvImageGatherExtended");
        spirvOptions[2].value.intValue0 = globalSession->findCapability("spvDemoteToHelperInvocation");

        slang::TargetDesc targetDesc = {};

        switch (target)
        {
        case Target::SPIRV:
            targetDesc.format = SLANG_SPIRV;
            targetDesc.profile = globalSession->findProfile("spirv_1_4");
            targetDesc.compilerOptionEntryCount = 3;
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