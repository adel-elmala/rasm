#include "renderer/shaderCompiler.h"
#include "rasm/types.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "slang.h"
#include "slang-com-helper.h"
#include "slang-com-ptr.h"

#include "spdlog/spdlog.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>
#include <filesystem>

namespace rasm
{
    namespace
    {
        struct FieldOffset
        {
            const char *name;
            size_t offset;
        };
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

        bool validateInstanceTransform(slang::TypeLayoutReflection *transform)
        {
            return validateStruct(transform, "InstanceTransform", sizeof(gfx::InstanceTransform),
                                  {{"worldMatrix", offsetof(gfx::InstanceTransform, worldMatrix)},
                                   {"normalMatrix", offsetof(gfx::InstanceTransform, normalMatrix)},
                                   {"previousWorld", offsetof(gfx::InstanceTransform, previousWorld)},
                                   {"boundsCenter", offsetof(gfx::InstanceTransform, boundsCenter)},
                                   {"boundsRadius", offsetof(gfx::InstanceTransform, boundsRadius)},
                                   {"skinnedVertex", offsetof(gfx::InstanceTransform, skinnedVertex)},
                                   {"skinnedMotion", offsetof(gfx::InstanceTransform, skinnedMotion)}});
        }

        bool validateGeometryData(slang::TypeLayoutReflection *geometry)
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
        bool validateAnimationStructs(slang::TypeLayoutReflection *pushConstants)
        {
            auto frame = fieldStruct(pushConstants, "frame");
            auto entity = fieldStruct(frame, "entities");
            auto mesh = fieldStruct(entity, "mesh");
            bool valid = validateStruct(pushConstants, "AnimatePushConstants", sizeof(gfx::AnimatePushConstants),
                                        {{"frame", offsetof(gfx::AnimatePushConstants, frame)},
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
                                    {{"entityMatrix", offsetof(gfx::AnimatedEntity, entityMatrix)},
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
            valid &= validateStruct(fieldStruct(mesh, "subMeshes"), "AnimationSubMeshData", sizeof(gfx::AnimationSubMeshData),
                                    {{"node", offsetof(gfx::AnimationSubMeshData, node)},
                                     {"jointCount", offsetof(gfx::AnimationSubMeshData, jointCount)},
                                     {"firstJoint", offsetof(gfx::AnimationSubMeshData, firstJoint)},
                                     {"firstSphere", offsetof(gfx::AnimationSubMeshData, firstSphere)},
                                     {"geometryIndex", offsetof(gfx::AnimationSubMeshData, geometryIndex)},
                                     {"vertexCount", offsetof(gfx::AnimationSubMeshData, vertexCount)},
                                     {"firstSkinnedVertex", offsetof(gfx::AnimationSubMeshData, firstSkinnedVertex)},
                                     {"firstWeight", offsetof(gfx::AnimationSubMeshData, firstWeight)},
                                     {"firstMorphRadius", offsetof(gfx::AnimationSubMeshData, firstMorphRadius)}});
            valid &= validateStruct(fieldStruct(mesh, "joints"), "AnimationJointData", sizeof(gfx::AnimationJointData),
                                    {{"inverseBind", offsetof(gfx::AnimationJointData, inverseBind)},
                                     {"node", offsetof(gfx::AnimationJointData, node)}});
            valid &= validateStruct(fieldStruct(frame, "skins"), "SkinVertex", sizeof(gfx::SkinVertex),
                                    {{"joints", offsetof(gfx::SkinVertex, joints)},
                                     {"weights", offsetof(gfx::SkinVertex, weights)}});
            valid &= validateStruct(fieldStruct(frame, "morphs"), "MorphDelta", sizeof(gfx::MorphDelta),
                                    {{"position", offsetof(gfx::MorphDelta, position)},
                                     {"normalTangent", offsetof(gfx::MorphDelta, normalTangent)}});
            valid &= validateGeometryData(fieldStruct(frame, "geometry"));
            valid &= validateStruct(fieldStruct(mesh, "nodes"), "AnimationNodeData", sizeof(gfx::AnimationNodeData),
                                    {{"rest", offsetof(gfx::AnimationNodeData, rest)},
                                     {"translation", offsetof(gfx::AnimationNodeData, translation)},
                                     {"parent", offsetof(gfx::AnimationNodeData, parent)},
                                     {"rotation", offsetof(gfx::AnimationNodeData, rotation)},
                                     {"scale", offsetof(gfx::AnimationNodeData, scale)},
                                     {"padding", offsetof(gfx::AnimationNodeData, padding)}});
            valid &= validateStruct(fieldStruct(mesh, "samplers"), "AnimationSamplerData", sizeof(gfx::AnimationSamplerData),
                                    {{"firstTime", offsetof(gfx::AnimationSamplerData, firstTime)},
                                     {"firstValue", offsetof(gfx::AnimationSamplerData, firstValue)},
                                     {"keyCount", offsetof(gfx::AnimationSamplerData, keyCount)},
                                     {"interpolation", offsetof(gfx::AnimationSamplerData, interpolation)}});
            valid &= validateInstanceTransform(fieldStruct(frame, "transforms"));
            return valid;
        }

        // Walks pc -> scene -> pointers because only that reports the layout the shaders read. ProgramLayout::getTypeLayout
        // has no LayoutRules value for data behind a pointer, so it adds std140/std430 padding.
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

            // The tone-map pass, the MSAA resolve, TAA, the bloom passes, the environment bake and the UI pass have their own push
            // constants.
            if (pushConstants && pushConstants->getName() && std::strcmp(pushConstants->getName(), "ToneMapPushConstants") == 0)
            {
                return validateStruct(pushConstants, "ToneMapPushConstants", sizeof(gfx::ToneMapPushConstants),
                                      {{"source", offsetof(gfx::ToneMapPushConstants, source)},
                                       {"exposure", offsetof(gfx::ToneMapPushConstants, exposure)},
                                       {"bloom", offsetof(gfx::ToneMapPushConstants, bloom)},
                                       {"bloomStrength", offsetof(gfx::ToneMapPushConstants, bloomStrength)}});
            }
            if (pushConstants && pushConstants->getName() && std::strcmp(pushConstants->getName(), "ResolvePushConstants") == 0)
            {
                return validateStruct(pushConstants, "ResolvePushConstants", sizeof(gfx::ResolvePushConstants),
                                      {{"source", offsetof(gfx::ResolvePushConstants, source)},
                                       {"target", offsetof(gfx::ResolvePushConstants, target)},
                                       {"samples", offsetof(gfx::ResolvePushConstants, samples)},
                                       {"exposure", offsetof(gfx::ResolvePushConstants, exposure)}});
            }
            if (pushConstants && pushConstants->getName() && std::strcmp(pushConstants->getName(), "TaaPushConstants") == 0)
            {
                return validateStruct(pushConstants, "TaaPushConstants", sizeof(gfx::TaaPushConstants),
                                      {{"colorAndMotion", offsetof(gfx::TaaPushConstants, colorAndMotion)},
                                       {"depthAndHistory", offsetof(gfx::TaaPushConstants, depthAndHistory)},
                                       {"targetAndReset", offsetof(gfx::TaaPushConstants, targetAndReset)},
                                       {"exposure", offsetof(gfx::TaaPushConstants, exposure)}});
            }
            if (pushConstants && pushConstants->getName() && std::strcmp(pushConstants->getName(), "BloomPushConstants") == 0)
            {
                return validateStruct(pushConstants, "BloomPushConstants", sizeof(gfx::BloomPushConstants),
                                      {{"source", offsetof(gfx::BloomPushConstants, source)},
                                       {"target", offsetof(gfx::BloomPushConstants, target)},
                                       {"base", offsetof(gfx::BloomPushConstants, base)},
                                       {"scale", offsetof(gfx::BloomPushConstants, scale)}});
            }
            if (pushConstants && pushConstants->getName() && std::strcmp(pushConstants->getName(), "UiPushConstants") == 0)
            {
                return validateStruct(pushConstants, "UiPushConstants", sizeof(gfx::UiPushConstants),
                                      {{"vertices", offsetof(gfx::UiPushConstants, vertices)},
                                       {"texture", offsetof(gfx::UiPushConstants, texture)},
                                       {"displaySize", offsetof(gfx::UiPushConstants, displaySize)}});
            }
            if (pushConstants && pushConstants->getName() && std::strcmp(pushConstants->getName(), "EnvironmentPushConstants") == 0)
            {
                return validateStruct(pushConstants, "EnvironmentPushConstants", sizeof(gfx::EnvironmentPushConstants),
                                      {{"source", offsetof(gfx::EnvironmentPushConstants, source)},
                                       {"target", offsetof(gfx::EnvironmentPushConstants, target)},
                                       {"roughness", offsetof(gfx::EnvironmentPushConstants, roughness)},
                                       {"size", offsetof(gfx::EnvironmentPushConstants, size)}});
            }

            if (pushConstants && pushConstants->getName() && std::strcmp(pushConstants->getName(), "AnimatePushConstants") == 0)
            {
                return validateAnimationStructs(pushConstants);
            }

            // Custom pipelines bring their own push constants and do not share core.slang.
            auto scene = fieldStruct(pushConstants, "scene");
            if (!scene || std::strcmp(scene->getName(), "SceneData") != 0)
            {
                return true;
            }

            auto materialData = fieldStruct(scene, "materialsPtr");
            auto material = fieldStruct(materialData, "material");

            bool valid = true;

            valid &= validateStruct(pushConstants, "PushConstants", sizeof(gfx::PushConstants),
                                    {{"scene", offsetof(gfx::PushConstants, scene)},
                                     {"firstDraw", offsetof(gfx::PushConstants, firstDraw)},
                                     {"argument", offsetof(gfx::PushConstants, argument)}});

            valid &= validateStruct(scene, "SceneData", sizeof(gfx::SceneData),
                                    {{"projection", offsetof(gfx::SceneData, projection)},
                                     {"view", offsetof(gfx::SceneData, view)},
                                     {"cameraWorldPosition", offsetof(gfx::SceneData, cameraWorldPosition)},
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
                                     {"padding", offsetof(gfx::SceneData, padding)},
                                     {"animatedTransformsPtr", offsetof(gfx::SceneData, animatedTransformsPtr)},
                                     {"skinnedVertsPtr", offsetof(gfx::SceneData, skinnedVertsPtr)},
                                     {"previousSkinnedVertsPtr", offsetof(gfx::SceneData, previousSkinnedVertsPtr)}});

            valid &= validateInstanceTransform(fieldStruct(scene, "animatedTransformsPtr"));

            valid &= validateStruct(fieldStruct(scene, "shadowedLights"), "ShadowedLight", sizeof(gfx::ShadowedLight),
                                    {{"light", offsetof(gfx::ShadowedLight, light)},
                                     {"firstMap", offsetof(gfx::ShadowedLight, firstMap)},
                                     {"mapCount", offsetof(gfx::ShadowedLight, mapCount)}});

            valid &= validateStruct(fieldStruct(scene, "lightsPtr"), "Light", sizeof(gfx::Light),
                                    {{"position", offsetof(gfx::Light, position)},
                                     {"intensity", offsetof(gfx::Light, intensity)},
                                     {"color", offsetof(gfx::Light, color)},
                                     {"range", offsetof(gfx::Light, range)},
                                     {"direction", offsetof(gfx::Light, direction)},
                                     {"type", offsetof(gfx::Light, type)},
                                     {"spotScale", offsetof(gfx::Light, spotScale)},
                                     {"spotOffset", offsetof(gfx::Light, spotOffset)}});

            valid &= validateStruct(fieldStruct(scene, "shadowMaps"), "ShadowMap", sizeof(gfx::ShadowMap),
                                    {{"matrix", offsetof(gfx::ShadowMap, matrix)},
                                     {"texture", offsetof(gfx::ShadowMap, texture)},
                                     {"split", offsetof(gfx::ShadowMap, split)}});

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

            valid &= validateStruct(fieldStruct(scene, "instancesPtr"), "InstanceData", sizeof(gfx::InstanceData),
                                    {{"worldMatrix", offsetof(gfx::InstanceData, worldMatrix)},
                                     {"normalMatrix", offsetof(gfx::InstanceData, normalMatrix)},
                                     {"geometryIndex", offsetof(gfx::InstanceData, geometryIndex)},
                                     {"materialIndex", offsetof(gfx::InstanceData, materialIndex)},
                                     {"previousWorld", offsetof(gfx::InstanceData, previousWorld)},
                                     {"animationRow", offsetof(gfx::InstanceData, animationRow)}});

            valid &= validateGeometryData(fieldStruct(scene, "geometryPtr"));

            valid &= validateStruct(fieldStruct(fieldStruct(scene, "geometryPtr"), "lods"), "GeometryLod", sizeof(gfx::GeometryLod),
                                    {{"firstIndex", offsetof(gfx::GeometryLod, firstIndex)},
                                     {"indexCount", offsetof(gfx::GeometryLod, indexCount)},
                                     {"error", offsetof(gfx::GeometryLod, error)}});

            valid &= validateStruct(materialData, "MaterialData", sizeof(gfx::MaterialData),
                                    {{"material", offsetof(gfx::MaterialData, material)},
                                     {"materialId", offsetof(gfx::MaterialData, materialId)},
                                     {"alphaMode", offsetof(gfx::MaterialData, alphaMode)},
                                     {"alphaCutoff", offsetof(gfx::MaterialData, alphaCutoff)},
                                     {"flags", offsetof(gfx::MaterialData, flags)},
                                     {"textureTransforms", offsetof(gfx::MaterialData, textureTransforms)}});

            valid &= validateStruct(material, "Material", sizeof(gfx::Material),
                                    {{"metallicRoughness", offsetof(gfx::Material, pbr)},
                                     {"normal", offsetof(gfx::Material, normal)},
                                     {"occlusion", offsetof(gfx::Material, occlusion)},
                                     {"emissive", offsetof(gfx::Material, emissive)}});

            valid &= validateStruct(fieldStruct(materialData, "textureTransforms"), "TextureTransform", sizeof(gfx::TextureTransform),
                                    {{"matrix", offsetof(gfx::TextureTransform, matrix)},
                                     {"offset", offsetof(gfx::TextureTransform, offset)}});

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

        // Every build checks, since a Release build with a mismatch would read shifted data on the GPU (B23).
        // validateRasmStructs logged each mismatch.
        if (!validateRasmStructs(linkedProgram->getLayout()))
        {
            spdlog::error("The C++ structs do not match shaders/lib/core.slang; '{}' is not compiled.", outputPath);
            return {};
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
        // The returned desc only points at these, so they must outlive this function. Slang counts any texture gather
        // (shadow.slang) as spvImageGatherExtended and warns when the profile lacks it; plain gathers emit no such
        // SPIR-V capability, so naming it changes no code.
        static slang::CompilerOptionEntry spirvOptions[] = {{slang::CompilerOptionName::EmitSpirvDirectly, {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}},
                                                            {slang::CompilerOptionName::Capability, {slang::CompilerOptionValueKind::Int, 0, 0, nullptr, nullptr}}};
        spirvOptions[1].value.intValue0 = globalSession->findCapability("spvImageGatherExtended");

        slang::TargetDesc targetDesc = {};

        switch (target)
        {
        case Target::SPIRV:
            targetDesc.format = SLANG_SPIRV;
            targetDesc.profile = globalSession->findProfile("spirv_1_4");
            targetDesc.compilerOptionEntryCount = 2;
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