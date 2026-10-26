#include "rasm/material.h"
#include "rasm/engine.h"
#include "core/utils.h"
#include "rhi/vulkan/vulkanTypes.h"

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
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO)]                = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 1]            = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 2]            = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::METALLIC)]              = 0.0f;
        material.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)]             = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)]          = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)]    = 1.0f;
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
            material.shader = createShader(readFile("./shaders/passes/basic.slang"));
            break;
        case MaterialType::PBR:
            material.name = "pbr";
            material.shader = createShader(readFile("./shaders/passes/rasm.slang"));
            break;
        case MaterialType::UNLIT:
            material.name = "unlit";
            material.shader = createShader(readFile("./shaders/passes/unlit.slang"));
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

            // Color textures are sRGB; material data is linear (glTF 2.0).
            // Expand to RGBA8 to avoid unsupported RGB Vulkan formats and mismatched upload sizes.
            auto uploadTexture = [&](int textureIndex, bool srgb) -> TextureHandle
            {
                if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
                    return {};
                int source = model.textures[textureIndex].source;
                if (source < 0 || source >= static_cast<int>(model.images.size()))
                    return {};
                const auto &image = model.images[source];
                if (image.bits != 8 || image.component < 1 || image.component > 4 ||
                    image.width <= 0 || image.height <= 0)
                {
                    spdlog::error("Unsupported glTF image '{}' (expected 8-bit components).", image.uri);
                    return {};
                }
                size_t pixels = static_cast<size_t>(image.width) * image.height;
                if (image.image.size() < pixels * image.component)
                    return {};
                std::vector<unsigned char> rgba(pixels * 4, 255);
                for (size_t i = 0; i < pixels; ++i)
                {
                    const auto *src = image.image.data() + i * image.component;
                    rgba[i * 4] = src[0];
                    rgba[i * 4 + 1] = image.component >= 3 ? src[1] : src[0];
                    rgba[i * 4 + 2] = image.component >= 3 ? src[2] : src[0];
                    if (image.component == 2 || image.component == 4)
                        rgba[i * 4 + 3] = src[image.component - 1];
                }
                ResourceDesc desc{};
                desc.name = image.uri;
                desc.type = ResourceType::TEXTURE;
                desc.texture.width = image.width;
                desc.texture.height = image.height;
                desc.texture.format = srgb ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM;
                desc.texture.usage = TextureUsage::SAMPLED;
                auto texture = createTexture(desc);
                if (!texture.isValid() || !ctx.fillTexture(texture, rgba.data()))
                    return {};
                addBindlessTexture(bindlessDescriptorSet, texture);
                return texture;
            };

            mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)] =
                uploadTexture(material->pbrMetallicRoughness.baseColorTexture.index, true);
            auto mr = uploadTexture(material->pbrMetallicRoughness.metallicRoughnessTexture.index, false);
            mat.textures[static_cast<uint32_t>(PbrSlot::METALLIC)] = mr;
            mat.textures[static_cast<uint32_t>(PbrSlot::ROUGHNESS)] = mr;
            mat.textures[static_cast<uint32_t>(PbrSlot::NORMAL)] = uploadTexture(material->normalTexture.index, false);
            mat.textures[static_cast<uint32_t>(PbrSlot::OCCLUSION)] = uploadTexture(material->occlusionTexture.index, false);
            mat.textures[static_cast<uint32_t>(PbrSlot::EMISSIVE)] = uploadTexture(material->emissiveTexture.index, true);
            mat.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)] = material->normalTexture.scale;
            mat.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)] = material->occlusionTexture.strength;
            for (uint32_t i = 0; i < 3; ++i)
                mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + i] = material->emissiveFactor[i];
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
