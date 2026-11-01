#include "rasm/material.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"

#include "tiny_gltf.h"

namespace rasm
{

    MaterialHandle Engine::createMaterial(MaterialType type, std::vector<TextureHandle> textures)
    {
        MaterialHandle handle = handleManager.getNextMaterialHandle();
        if (handle.index >= MAX_MATERIALS)
        {
            spdlog::warn("Exceeded maximum number of materials.");
            return {};
        }

        auto material = Material{.handle = handle, .type = type, .textures = {}};

        // Uber-material defaults for hand-made materials (loadMaterial fills these from the glTF
        // file instead): white base color so the albedo texture is not multiplied by zero,
        // non-metallic and fully rough surface.
        // clang-format off
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO)]                = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 1]            = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 2]            = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::METALLIC)]              = 0.0f;
        material.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)]             = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)]          = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)]    = 1.0f;
        // clang-format on
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
        case MaterialType::PBR:
            material.name = "pbr";
            break;
        case MaterialType::UNLIT:
            material.name = "unlit";
            break;

        default:
            spdlog::error("Unknown material template type.");
            break;
        }

        registery.materials.resize(handle.index + 1);
        registery.materials[handle.index] = material;

        return handle;
    }

    MaterialHandle Engine::loadMaterial(const tinygltf::Model &model, int materialIndex, GltfImportCache &cache)
    {
        if (materialIndex < 0 || materialIndex >= model.materials.size())
            return {};

        // Primitives share materials and materials share images; create each of them once per model.
        if (auto it = cache.materials.find(materialIndex); it != cache.materials.end())
            return it->second;

        MaterialHandle handle = handleManager.getNextMaterialHandle();
        if (handle.index >= MAX_MATERIALS)
        {
            spdlog::warn("Exceeded maximum number of materials.");
            return {};
        }

        auto mat = Material{.handle = handle, .type = MaterialType::PBR, .textures = {}};

        const tinygltf::Material *material = &model.materials[materialIndex];

        if (material->extensions.count("KHR_materials_unlit") != 0)
            mat.type = MaterialType::UNLIT;

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

                // Upload each (image, color space) once per model.
                const uint64_t cacheKey = (static_cast<uint64_t>(source) << 1) | (srgb ? 1u : 0u);
                if (auto it = cache.imageTextures.find(cacheKey); it != cache.imageTextures.end())
                    return it->second;

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
                desc.name = image.uri.empty() ? "glTF image " + std::to_string(source) : image.uri;
                desc.type = ResourceType::TEXTURE;
                desc.texture.width = image.width;
                desc.texture.height = image.height;
                desc.texture.format = srgb ? Format::R8G8B8A8_SRGB : Format::R8G8B8A8_UNORM;
                desc.texture.usage = TextureUsage::SAMPLED;
                auto texture = createTexture(desc);
                if (!texture.isValid() || !ctx.fillTexture(texture, rgba.data()))
                    return {};
                addBindlessTexture(bindlessDescriptorSet, texture);
                cache.imageTextures[cacheKey] = texture;
                return texture;
            };

            mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)] =
                uploadTexture(material->pbrMetallicRoughness.baseColorTexture.index, true);
            // Unlit shading reads only the base color; the PBR textures are a fallback for viewers without the extension.
            if (mat.type != MaterialType::UNLIT)
            {
                auto mr = uploadTexture(material->pbrMetallicRoughness.metallicRoughnessTexture.index, false);
                mat.textures[static_cast<uint32_t>(PbrSlot::METALLIC)] = mr;
                mat.textures[static_cast<uint32_t>(PbrSlot::ROUGHNESS)] = mr;
                mat.textures[static_cast<uint32_t>(PbrSlot::NORMAL)] = uploadTexture(material->normalTexture.index, false);
                mat.textures[static_cast<uint32_t>(PbrSlot::OCCLUSION)] = uploadTexture(material->occlusionTexture.index, false);
                mat.textures[static_cast<uint32_t>(PbrSlot::EMISSIVE)] = uploadTexture(material->emissiveTexture.index, true);
            }
            mat.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)] = material->normalTexture.scale;
            mat.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)] = material->occlusionTexture.strength;
            for (uint32_t i = 0; i < 3; ++i)
                mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + i] = material->emissiveFactor[i];
        }

        registery.materials.resize(handle.index + 1);
        registery.materials[handle.index] = mat;
        cache.materials[materialIndex] = handle;
        return handle;
    }

    const Material &Engine::getMaterial(const MaterialHandle &handle)
    {
        if (handle.isValid() && handle.index < registery.materials.size())
            return registery.materials[handle.index];

        if (handle.isValid())
            spdlog::error("Invalid material handle {}; using the default material.", handle.index);
        if (defaultMaterial.isValid())
            return registery.materials[defaultMaterial.index];

        // Only reachable when engine construction failed before creating the default material.
        static const Material noMaterial{};
        spdlog::error("No default material; the engine did not initialize.");
        return noMaterial;
    }

}
