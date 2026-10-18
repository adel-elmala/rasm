#include "rasm/core/material.h"
#include "rasm/core/engine.h"
#include "rasm/core/utils.h"
#include "rasm/gfx/types.h"

#include "spdlog/spdlog.h"

#include "tiny_gltf.h"

namespace rasm
{

    MaterialHandle Engine::createMaterial(MaterialType type, std::vector<TextureHandle> textures, ShaderHandle shader)
    {
        if (type == MaterialType::SHADER && !shader.isValid())
        {
            spdlog::error("Invalid shader handle provided for custom shader material.");
            return {};
        }

        MaterialHandle handle = handleManager.getNextMaterialHandle();
        if (handle.index >= MAX_MATERIALS)
        {
            spdlog::warn("Exceeded maximum number of materials.");
            return {};
        }

        auto material = Material{.handle = handle, .type = type, .textures = {}, .shader = shader};

        // Uber-material defaults for hand-made materials (loadMaterial fills these from the glTF
        // file instead): white base color so the albedo texture is not multiplied by zero,
        // non-metallic and fully rough surface.
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO)]     = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 1] = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 2] = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::METALLIC)]   = 0.0f;
        material.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)]  = 1.0f;
        if (!textures.empty())
        {
            for (uint32_t i = 0; i < MAX_TEXTURE_SLOTS && i < textures.size(); ++i)
            {
                auto textureHandle = textures[i];
                if (!textureHandle.isValid())
                    continue;
                material.textures[i] = textureHandle;
                addBindlessTexture(bindlessDescriptorSet, textureHandle);
            }
        }

        switch (type)
        {
        case MaterialType::BASIC:
            material.name = "basic";
            material.shader = createShader(readFile("./shaders/common/basic.slang"));
            break;
        case MaterialType::PBR:
            material.name = "pbr";
            material.shader = createShader(readFile("./shaders/common/pbr.slang"));
            break;
        case MaterialType::UNLIT:
            material.name = "unlit";
            material.shader = createShader(readFile("./shaders/common/unlit.slang"));
            break;
        case MaterialType::SHADER:
            material.name = "shader";
            break;

        default:
            spdlog::error("Unknown material template type.");
            break;
        }

        registery.materials.resize(handle.index + 1);
        registery.materials[handle.index] = material;

        return handle;
    }

#if 0 
    std::vector<MaterialHandle> Engine::loadMaterials(MeshHandle meshHandle)
    {
        if (!meshHandle.isValid() || meshHandle.index >= registery.meshes.size())
            return {}; 

        const auto &mesh = registery.meshes[meshHandle.index];
        if (mesh.type != Mesh::MeshType::GLTF)
        {
            spdlog::warn("Mesh is not of type GLTF.");
            return {};
        }

        const auto &GLTFraw = std::get<GLTFRaw>(mesh.data);

        std::vector<MaterialHandle> handles;
        for (int i = 0; i < GLTFraw.subGltfRaws.size(); ++i)
        {
            handles.push_back(loadMaterial(GLTFraw.model, i));
        }
        return handles;
    }
#endif

    MaterialHandle Engine::loadMaterial(const tinygltf::Model &model, int materialIndex)
    {
        if (materialIndex < 0 || materialIndex >= model.materials.size())
            return {};

        MaterialHandle handle = handleManager.getNextMaterialHandle();
        if (handle.index >= MAX_MATERIALS)
        {
            spdlog::warn("Exceeded maximum number of materials.");
            return {};
        }

        auto mat = Material{.handle = handle, .type = MaterialType::PBR, .textures = {}, .shader = {}};

        const tinygltf::Material *material = &model.materials[materialIndex];

        // glTF lets each texture pick its own TEXCOORD_n set. The vertex data carries
        // MAX_VERTEX_UV_SETS sets; anything beyond that is out of our support for now.
        auto resolveTexCoord = [&](int texCoord, const char *slotName) -> uint32_t
        {
            if (texCoord < 0 || texCoord >= static_cast<int>(MAX_VERTEX_UV_SETS))
            {
                spdlog::warn("Material '{}' references {} texCoord set {} but only {} sets are supported; falling back to TEXCOORD_0.",
                             material->name, slotName, texCoord, MAX_VERTEX_UV_SETS);
                return 0;
            }
            return static_cast<uint32_t>(texCoord);
        };

        mat.texCoords[static_cast<uint32_t>(PbrSlot::ALBEDO)] = resolveTexCoord(material->pbrMetallicRoughness.baseColorTexture.texCoord, "baseColor");
        mat.texCoords[static_cast<uint32_t>(PbrSlot::METALLIC)] = resolveTexCoord(material->pbrMetallicRoughness.metallicRoughnessTexture.texCoord, "metallicRoughness");
        mat.texCoords[static_cast<uint32_t>(PbrSlot::ROUGHNESS)] = mat.texCoords[static_cast<uint32_t>(PbrSlot::METALLIC)];
        mat.texCoords[static_cast<uint32_t>(PbrSlot::NORMAL)] = resolveTexCoord(material->normalTexture.texCoord, "normal");
        mat.texCoords[static_cast<uint32_t>(PbrSlot::OCCLUSION)] = resolveTexCoord(material->occlusionTexture.texCoord, "occlusion");
        mat.texCoords[static_cast<uint32_t>(PbrSlot::EMISSIVE)] = resolveTexCoord(material->emissiveTexture.texCoord, "emissive");

        {
            // glTF defaults: metallicFactor = 1.0, roughnessFactor = 1.0. tinygltf keeps those
            // defaults even when no metallicRoughness texture exists, so always copy them over.
            mat.params[static_cast<uint32_t>(PbrParam::METALLIC)] = material->pbrMetallicRoughness.metallicFactor;
            mat.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)] = material->pbrMetallicRoughness.roughnessFactor;

            // Same for the base color factor: a material may tint with it without any texture.
            mat.params[static_cast<uint32_t>(PbrParam::ALBEDO)] = material->pbrMetallicRoughness.baseColorFactor[0];
            mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 1] = material->pbrMetallicRoughness.baseColorFactor[1];
            mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 2] = material->pbrMetallicRoughness.baseColorFactor[2];

            // Extract Albedo (Base Color) Texture and Factor
            auto baseColorTextureIndex = material->pbrMetallicRoughness.baseColorTexture.index;
            if (baseColorTextureIndex >= 0)
            {
                auto &baseColorTexture = model.textures[baseColorTextureIndex];
                auto &baseColorImage = model.images[baseColorTexture.source];

                auto baseColorTexDesc = ResourceDesc{};
                baseColorTexDesc.name = baseColorImage.uri;
                baseColorTexDesc.type = ResourceType::TEXTURE;
                baseColorTexDesc.texture.width = baseColorImage.width;
                baseColorTexDesc.texture.height = baseColorImage.height;
                baseColorTexDesc.texture.format = baseColorImage.component == 4 ? Format::R8G8B8A8_SRGB : Format::R8G8B8_UNORM;
                baseColorTexDesc.texture.usage = TextureUsage::SAMPLED;

                TextureHandle baseColorTextureHandle = createTexture(baseColorTexDesc);
                addBindlessTexture(bindlessDescriptorSet, baseColorTextureHandle);
                ctx.fillTexture(baseColorTextureHandle, baseColorImage.image.data()); // TODO: make sure first that the texture was not filled before
                mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)] = baseColorTextureHandle;
            }

            // Extract Metallic-Roughness Texture (factors are already copied above)
            auto metallicRoughnessTextureIndex = material->pbrMetallicRoughness.metallicRoughnessTexture.index;
            if (metallicRoughnessTextureIndex >= 0)
            {
                auto &metallicRoughnessTexture = model.textures[metallicRoughnessTextureIndex];
                auto &metallicRoughnessImage = model.images[metallicRoughnessTexture.source];

                auto metallicRoughnessTexDesc = ResourceDesc{};
                metallicRoughnessTexDesc.name = metallicRoughnessImage.uri;
                metallicRoughnessTexDesc.type = ResourceType::TEXTURE;
                metallicRoughnessTexDesc.texture.width = metallicRoughnessImage.width;
                metallicRoughnessTexDesc.texture.height = metallicRoughnessImage.height;
                metallicRoughnessTexDesc.texture.format = metallicRoughnessImage.component == 4 ? Format::R8G8B8A8_SRGB : Format::R8G8B8_UNORM;
                metallicRoughnessTexDesc.texture.usage = TextureUsage::SAMPLED;

                TextureHandle metallicRoughnessTextureHandle = createTexture(metallicRoughnessTexDesc);
                addBindlessTexture(bindlessDescriptorSet, metallicRoughnessTextureHandle);
                ctx.fillTexture(metallicRoughnessTextureHandle, metallicRoughnessImage.image.data()); // TODO: make sure first that the texture was not filled before
                mat.textures[static_cast<uint32_t>(PbrSlot::METALLIC)] = metallicRoughnessTextureHandle;
                mat.textures[static_cast<uint32_t>(PbrSlot::ROUGHNESS)] = metallicRoughnessTextureHandle;
            }
            // Extract other material properties (Normal, Occlusion, Emissive) if available
            auto normalTextureIndex = material->normalTexture.index;
            if (normalTextureIndex >= 0)
            {
                auto &normalTexture = model.textures[normalTextureIndex];
                auto &normalImage = model.images[normalTexture.source];

                ResourceDesc normalTexDesc{};
                normalTexDesc.name = normalImage.uri;
                normalTexDesc.type = ResourceType::TEXTURE;
                normalTexDesc.texture.width = normalImage.width;
                normalTexDesc.texture.height = normalImage.height;
                normalTexDesc.texture.format = normalImage.component == 4 ? Format::R8G8B8A8_SRGB : Format::R8G8B8_UNORM;
                normalTexDesc.texture.usage = TextureUsage::SAMPLED;

                TextureHandle normalTextureHandle = createTexture(normalTexDesc);
                addBindlessTexture(bindlessDescriptorSet, normalTextureHandle);
                ctx.fillTexture(normalTextureHandle, normalImage.image.data()); // TODO: make sure first that the texture was not filled before
                mat.textures[static_cast<uint32_t>(PbrSlot::NORMAL)] = normalTextureHandle;

                mat.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)] = material->normalTexture.scale;
            }

            auto occlusionTextureIndex = material->occlusionTexture.index;
            if (occlusionTextureIndex >= 0)
            {
                auto &occlusionTexture = model.textures[occlusionTextureIndex];
                auto &occlusionImage = model.images[occlusionTexture.source];

                ResourceDesc occlusionTexDesc{};
                occlusionTexDesc.name = occlusionImage.uri;
                occlusionTexDesc.type = ResourceType::TEXTURE;
                occlusionTexDesc.texture.width = occlusionImage.width;
                occlusionTexDesc.texture.height = occlusionImage.height;
                occlusionTexDesc.texture.format = occlusionImage.component == 4 ? Format::R8G8B8A8_SRGB : Format::R8G8B8_UNORM;
                occlusionTexDesc.texture.usage = TextureUsage::SAMPLED;

                TextureHandle occlusionTextureHandle = createTexture(occlusionTexDesc);
                addBindlessTexture(bindlessDescriptorSet, occlusionTextureHandle);
                ctx.fillTexture(occlusionTextureHandle, occlusionImage.image.data()); // TODO: make sure first that the texture was not filled before
                mat.textures[static_cast<uint32_t>(PbrSlot::OCCLUSION)] = occlusionTextureHandle;

                mat.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)] = material->occlusionTexture.strength;
            }

            // Emissive factor also exists without an emissive texture (glTF lets them be independent).
            mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR)] = material->emissiveFactor[0];
            mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + 1] = material->emissiveFactor[1];
            mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + 2] = material->emissiveFactor[2];

            auto emissiveTextureIndex = material->emissiveTexture.index;
            if (emissiveTextureIndex >= 0)
            {
                auto &emissiveTexture = model.textures[emissiveTextureIndex];
                auto &emissiveImage = model.images[emissiveTexture.source];

                ResourceDesc emissiveTexDesc{};
                emissiveTexDesc.name = emissiveImage.uri;
                emissiveTexDesc.type = ResourceType::TEXTURE;
                emissiveTexDesc.texture.width = emissiveImage.width;
                emissiveTexDesc.texture.height = emissiveImage.height;
                emissiveTexDesc.texture.format = emissiveImage.component == 4 ? Format::R8G8B8A8_SRGB : Format::R8G8B8_UNORM;
                emissiveTexDesc.texture.usage = TextureUsage::SAMPLED;

                TextureHandle emissiveTextureHandle = createTexture(emissiveTexDesc);
                addBindlessTexture(bindlessDescriptorSet, emissiveTextureHandle);
                ctx.fillTexture(emissiveTextureHandle, emissiveImage.image.data()); // TODO: make sure first that the texture was not filled before
                mat.textures[static_cast<uint32_t>(PbrSlot::EMISSIVE)] = emissiveTextureHandle;
            }
        }

        registery.materials.resize(handle.index + 1);
        registery.materials[handle.index] = mat;
        return handle;
    }

    ShaderHandle Engine::createShader(const std::string &shaderSource)
    {
        ShaderHandle handle = handleManager.getNextShaderHandle();
        if (handle.index >= MAX_SHADERS)
        {
            spdlog::warn("Exceeded maximum number of shaders.");
            return {};
        }

        registery.shaders.resize(handle.index + 1);
        registery.shaders[handle.index] = shaderSource;

        return handle;
    }

    const Material &Engine::getMaterial(const MaterialHandle &handle)
    {
        if (handle.index >= registery.materials.size())
        {
            spdlog::error("Invalid material handle.");
            throw std::runtime_error("Invalid material handle.");
        }
        return registery.materials[handle.index];
    }

    gfx::Material Engine::convertToGfxMaterial(const Material &mat)
    {
        gfx::Material gmat{};

        // base color (albedo)
        gmat.pbr.baseColorFactor[0] = mat.params[static_cast<uint32_t>(PbrParam::ALBEDO)];
        gmat.pbr.baseColorFactor[1] = mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 1];
        gmat.pbr.baseColorFactor[2] = mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 2];

        // glTF texCoord set per texture; the shader selects the matching interpolated UV set.
        auto texCoordOf = [](uint32_t texCoord) -> uint32_t { return texCoord < MAX_VERTEX_UV_SETS ? texCoord : 0u; };

        gmat.pbr.baseColorTextureIndex = getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)]);
        gmat.pbr.baseColorTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::ALBEDO)]);
        gmat.pbr.metallicRoughnessTextureIndex = getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::METALLIC)]);
        gmat.pbr.metallicRoughtnessTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::METALLIC)]);

        gmat.pbr.metallicFactor = mat.params[static_cast<uint32_t>(PbrParam::METALLIC)];
        gmat.pbr.roughnessFactor = mat.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)];

        // normal map
        gmat.normal.normalTextureIndex = getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::NORMAL)]);
        gmat.normal.normalTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::NORMAL)]);
        gmat.normal.scale = mat.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)];

        // occlusion map
        gmat.occlusion.occlusionTextureIndex = getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::OCCLUSION)]);
        gmat.occlusion.occlusionTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::OCCLUSION)]);
        gmat.occlusion.strength = mat.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)];

        // emissive map
        gmat.emissive.emissiveTextureIndex = getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::EMISSIVE)]);
        gmat.emissive.emissiveTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::EMISSIVE)]);
        gmat.emissive.emissiveFactor[0] = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR)];
        gmat.emissive.emissiveFactor[1] = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + 1];
        gmat.emissive.emissiveFactor[2] = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + 2];

        return gmat;
    }

}
