#include "rasm/material.h"
#include "rasm/engine.h"
#include "assets/ktxImage.h"
#include "assets/meshImport.h"

#include "spdlog/spdlog.h"

#include "tiny_gltf.h"

namespace rasm
{
    namespace
    {
        // Maps a glTF sampler onto the shared set. wrapS picks the address mode for both axes, and magFilter
        // picks nearest or linear for every filter.
        SamplerType toSamplerType(const tinygltf::Model &model, int samplerIndex)
        {
            if (samplerIndex < 0 || samplerIndex >= static_cast<int>(model.samplers.size()))
                return SamplerType::LINEAR_REPEAT;

            const auto &sampler = model.samplers[samplerIndex];
            const bool nearest = sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST;
            switch (sampler.wrapS)
            {
            case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
                return nearest ? SamplerType::NEAREST_CLAMP : SamplerType::LINEAR_CLAMP;
            case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
                return nearest ? SamplerType::NEAREST_MIRRORED_REPEAT : SamplerType::LINEAR_MIRRORED_REPEAT;
            default:
                return nearest ? SamplerType::NEAREST_REPEAT : SamplerType::LINEAR_REPEAT;
            }
        }

        float getNumber(const tinygltf::Value &object, const char *name, float fallback)
        {
            const tinygltf::Value &value = object.Get(name);
            return value.IsNumber() ? static_cast<float>(value.GetNumberAsDouble()) : fallback;
        }

        glm::vec2 getVec2(const tinygltf::Value &object, const char *name, glm::vec2 fallback)
        {
            const tinygltf::Value &value = object.Get(name);
            if (!value.IsArray() || value.ArrayLen() < 2 || !value.Get(0).IsNumber() || !value.Get(1).IsNumber())
                return fallback;
            return glm::vec2(static_cast<float>(value.Get(0).GetNumberAsDouble()), static_cast<float>(value.Get(1).GetNumberAsDouble()));
        }

        // KHR_texture_transform on a texture reference. Its texCoord, when present, replaces the reference's.
        TextureTransform getTextureTransform(const tinygltf::ExtensionMap &extensions, int &texCoord)
        {
            auto it = extensions.find("KHR_texture_transform");
            if (it == extensions.end() || !it->second.IsObject())
                return {};
            const tinygltf::Value &transform = it->second;
            const tinygltf::Value &transformTexCoord = transform.Get("texCoord");
            if (transformTexCoord.IsInt())
                texCoord = transformTexCoord.GetNumberAsInt();
            return {.offset = getVec2(transform, "offset", glm::vec2(0.0f)), .rotation = getNumber(transform, "rotation", 0.0f), .scale = getVec2(transform, "scale", glm::vec2(1.0f))};
        }

        AlphaMode toAlphaMode(const std::string &alphaMode)
        {
            if (alphaMode == "MASK")
                return AlphaMode::MASK;
            if (alphaMode == "BLEND")
                return AlphaMode::BLEND;
            return AlphaMode::NONE;
        }
    }

    MaterialHandle Engine::createMaterial(MaterialType type, std::vector<TextureHandle> textures, bool doubleSided)
    {
        const HandleSlot slot = registry.materialHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::error("The material table is full: {} materials are alive. EngineConfig::maxMaterials ({}) sets its size.", registry.materialHandles.liveCount(), config.maxMaterials);
            return {};
        }
        const MaterialHandle handle{slot.index, slot.generation};

        auto material = Material{.handle = handle, .type = type, .textures = {}, .doubleSided = doubleSided};

        // Uber-material defaults for hand-made materials (loadMaterial fills these from the glTF
        // file instead): white base color so the albedo texture is not multiplied by zero,
        // non-metallic and fully rough surface.
        // clang-format off
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO)]                = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 1]            = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 2]            = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 3]            = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::METALLIC)]              = 0.0f;
        material.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)]             = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)]          = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)]    = 1.0f;
        material.params[static_cast<uint32_t>(PbrParam::EMISSIVE_STRENGTH)]     = 1.0f;
        // clang-format on
        if (!textures.empty())
        {
            for (uint32_t i = 0; i < MAX_TEXTURE_SLOTS && i < textures.size(); ++i)
            {
                auto textureHandle = textures[i];
                if (!textureHandle.isValid())
                    continue;
                material.textures[i] = textureHandle;
                if (getBindlessTextureIndex(textureHandle) == UINT32_MAX)
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

        registry.materials.resize(registry.materialHandles.size());
        registry.materials[handle.index] = material;
        writeMaterialRow(handle);

        return handle;
    }

    // Entities made with it, and the sub-meshes of the mesh whose import made it, let go of it; their rows are rewritten
    // with the entity's material or the default one, so the row can take the next material at once (each frame slot has
    // its own copy of the table). Its textures stay: they are the application's, or its mesh's.
    void Engine::destroyMaterial(MaterialHandle material)
    {
        if (!isLive(material))
        {
            spdlog::error("destroyMaterial: material handle {{{}, {}}} is invalid or was destroyed.", material.index, material.generation);
            return;
        }
        if (material == defaultMaterial)
        {
            spdlog::error("destroyMaterial: the default material can't be destroyed.");
            return;
        }
        detachMaterial(material);
        registry.materials[material.index] = {};
        if (material.index < registry.materialUsers.size())
            registry.materialUsers[material.index] = 0;
        if (material.index < registry.materialOwners.size())
            registry.materialOwners[material.index] = {};
        registry.materialHandles.release(material.index, material.generation);
    }

    // Nothing is searched when nothing uses the material, the usual case when a mesh and its entities go first.
    void Engine::detachMaterial(MaterialHandle material)
    {
        const uint32_t users = material.index < registry.materialUsers.size() ? registry.materialUsers[material.index] : 0;
        const MeshHandle owner = material.index < registry.materialOwners.size() ? registry.materialOwners[material.index] : MeshHandle{};
        bool ownerUsed = false;
        if (isLive(owner))
        {
            for (SubMesh &subMesh : registry.meshes[owner.index].subMeshes)
            {
                if (subMesh.material == material)
                    subMesh.material = {};
            }
            ownerUsed = registry.meshes[owner.index].users > 0;
        }
        if (users == 0 && !ownerUsed)
            return;
        for (size_t i = 1; i < registry.entities.size(); ++i)
        {
            Entity &entity = registry.entities[i];
            if (!isLive(entity.handle))
                continue;
            const bool user = entity.material == material;
            if (user)
                entity.material = {};
            if (user || (ownerUsed && entity.mesh == owner))
                writeInstanceRows(entity.handle, false);
        }
        refreshEntitiesAfterMaterialChange();
    }

    void Engine::updateMaterial(MaterialHandle material, const Material &values)
    {
        if (!isLive(material))
        {
            spdlog::error("Invalid material handle for updating material.");
            return;
        }

        bool texturesLoading = false;
        for (auto texture : values.textures)
        {
            if (texture.isValid() && getBindlessTextureIndex(texture) == UINT32_MAX)
                addBindlessTexture(bindlessDescriptorSet, texture);
            texturesLoading = texturesLoading || (texture.isValid() && !isLoaded(texture));
        }

        Material &materialRef = registry.materials[material.index];
        const bool runChanged = materialRef.doubleSided != values.doubleSided || materialRef.alphaMode != values.alphaMode;
        materialRef = values;
        materialRef.handle = material;
        writeMaterialRow(material);

        // doubleSided and alphaMode pick the draw run of the material's instances, and an entity with a texture that
        // still loads is not drawn. All are rare, so every entity is checked instead of tracking who uses the material.
        if (runChanged || texturesLoading)
            refreshEntitiesAfterMaterialChange();
    }

    MaterialHandle Engine::loadMaterial(const tinygltf::Model &model, int materialIndex, GltfImportCache &cache)
    {
        if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
            return {};

        // Primitives share materials and materials share images; create each of them once per model.
        if (auto it = cache.materials.find(materialIndex); it != cache.materials.end())
            return it->second;

        const HandleSlot slot = registry.materialHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::error("The material table is full: {} materials are alive. EngineConfig::maxMaterials ({}) sets its size.", registry.materialHandles.liveCount(), config.maxMaterials);
            return {};
        }
        const MaterialHandle handle{slot.index, slot.generation};

        auto mat = Material{.handle = handle, .type = MaterialType::PBR, .textures = {}};

        const tinygltf::Material *material = &model.materials[materialIndex];

        if (material->extensions.count("KHR_materials_unlit") != 0)
            mat.type = MaterialType::UNLIT;
        mat.doubleSided = material->doubleSided;
        mat.alphaMode = toAlphaMode(material->alphaMode);
        mat.alphaCutoff = static_cast<float>(material->alphaCutoff);

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

        // KHR_texture_transform may name another texCoord set, so the transform is read first.
        auto readTexture = [&](PbrSlot slot, int texCoord, const tinygltf::ExtensionMap &extensions, const char *slotName)
        {
            const auto index = static_cast<uint32_t>(slot);
            mat.textureTransforms[index] = getTextureTransform(extensions, texCoord);
            mat.texCoords[index] = resolveTexCoord(texCoord, slotName);
        };
        readTexture(PbrSlot::ALBEDO, material->pbrMetallicRoughness.baseColorTexture.texCoord, material->pbrMetallicRoughness.baseColorTexture.extensions, "baseColor");
        readTexture(PbrSlot::METALLIC, material->pbrMetallicRoughness.metallicRoughnessTexture.texCoord, material->pbrMetallicRoughness.metallicRoughnessTexture.extensions, "metallicRoughness");
        mat.textureTransforms[static_cast<uint32_t>(PbrSlot::ROUGHNESS)] = mat.textureTransforms[static_cast<uint32_t>(PbrSlot::METALLIC)];
        mat.texCoords[static_cast<uint32_t>(PbrSlot::ROUGHNESS)] = mat.texCoords[static_cast<uint32_t>(PbrSlot::METALLIC)];
        readTexture(PbrSlot::NORMAL, material->normalTexture.texCoord, material->normalTexture.extensions, "normal");
        readTexture(PbrSlot::OCCLUSION, material->occlusionTexture.texCoord, material->occlusionTexture.extensions, "occlusion");
        readTexture(PbrSlot::EMISSIVE, material->emissiveTexture.texCoord, material->emissiveTexture.extensions, "emissive");

        {
            // glTF defaults: metallicFactor = 1.0, roughnessFactor = 1.0. tinygltf keeps those
            // defaults even when no metallicRoughness texture exists, so always copy them over.
            mat.params[static_cast<uint32_t>(PbrParam::METALLIC)] = static_cast<float>(material->pbrMetallicRoughness.metallicFactor);
            mat.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)] = static_cast<float>(material->pbrMetallicRoughness.roughnessFactor);

            // Same for the base color factor: a material may tint with it without any texture.
            mat.params[static_cast<uint32_t>(PbrParam::ALBEDO)] = static_cast<float>(material->pbrMetallicRoughness.baseColorFactor[0]);
            mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 1] = static_cast<float>(material->pbrMetallicRoughness.baseColorFactor[1]);
            mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 2] = static_cast<float>(material->pbrMetallicRoughness.baseColorFactor[2]);
            mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + 3] = static_cast<float>(material->pbrMetallicRoughness.baseColorFactor[3]);

            // Color textures are sRGB; material data is linear (glTF 2.0).
            // Expand to RGBA8 to avoid unsupported RGB Vulkan formats and mismatched upload sizes.
            auto uploadTexture = [&](int textureIndex, KtxSlot slot) -> TextureHandle
            {
                if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
                    return {};
                const SamplerType sampler = toSamplerType(model, model.textures[textureIndex].sampler);
                const bool srgb = slot == KtxSlot::COLOR;

                // KHR_texture_basisu first, transcoded on the worker for this kind of slot. Without it, or when its
                // image failed, the texture's own source (a PNG or JPEG fallback) is used.
                const int basisuSource = getBasisuSource(model, textureIndex);
                const TextureRaw *ktx = nullptr;
                if (basisuSource >= 0 && cache.raw)
                {
                    const auto format = cache.raw->ktxFormats.find(getKtxSlotKey(basisuSource, slot));
                    const auto image = format != cache.raw->ktxFormats.end() ? cache.raw->ktxImages.find(getKtxImageKey(basisuSource, format->second)) : cache.raw->ktxImages.end();
                    ktx = image != cache.raw->ktxImages.end() ? &image->second : nullptr;
                }
                const int source = ktx ? basisuSource : model.textures[textureIndex].source;
                if (source < 0 || source >= static_cast<int>(model.images.size()))
                    return {};

                // Upload each (image, format, sampler) once per model. A texture handle has one bindless slot, so the
                // rare image used with two samplers is uploaded twice.
                const Format format = ktx ? ktx->format : srgb ? Format::R8G8B8A8_SRGB
                                                               : Format::R8G8B8A8_UNORM;
                const uint64_t cacheKey = (static_cast<uint64_t>(source) << 16) | (static_cast<uint64_t>(sampler) << 8) | static_cast<uint64_t>(format);
                if (auto it = cache.imageTextures.find(cacheKey); it != cache.imageTextures.end())
                    return it->second;

                const auto &image = model.images[source];
                const std::string name = image.uri.empty() ? "glTF image " + std::to_string(source) : image.uri;
                if (ktx)
                {
                    const TextureRaw &raw = *ktx;
                    auto texture = createTexture({.name = name,
                                                  .width = raw.width,
                                                  .height = raw.height,
                                                  .format = raw.format,
                                                  .usage = TextureUsage::SAMPLED,
                                                  .mipLevels = static_cast<uint32_t>(raw.levelSizes.size())});
                    if (!texture.isValid())
                        return {};
                    if (!fillTextureLevels(texture, raw))
                    {
                        ctx.destroyTexture(texture);
                        return {};
                    }
                    addBindlessTexture(bindlessDescriptorSet, texture, sampler);
                    cache.imageTextures[cacheKey] = texture;
                    return texture;
                }

                if (image.bits != 8 || image.component < 1 || image.component > 4 ||
                    image.width <= 0 || image.height <= 0)
                {
                    spdlog::error("Unsupported glTF image '{}' (expected 8-bit components).", image.uri);
                    return {};
                }
                size_t pixels = static_cast<size_t>(image.width) * image.height;
                if (image.image.size() < pixels * image.component)
                    return {};
                // tinygltf decodes images to RGBA on the worker thread, so the copy below is only for
                // an image that kept fewer channels.
                const unsigned char *rgbaData = image.image.data();
                std::vector<unsigned char> rgba;
                if (image.component != 4)
                {
                    rgba.assign(pixels * 4, 255);
                    for (size_t i = 0; i < pixels; ++i)
                    {
                        const auto *src = image.image.data() + i * image.component;
                        rgba[i * 4] = src[0];
                        rgba[i * 4 + 1] = image.component >= 3 ? src[1] : src[0];
                        rgba[i * 4 + 2] = image.component >= 3 ? src[2] : src[0];
                        if (image.component == 2)
                            rgba[i * 4 + 3] = src[1];
                    }
                    rgbaData = rgba.data();
                }
                auto texture = createTexture({.name = name,
                                              .width = static_cast<uint32_t>(image.width),
                                              .height = static_cast<uint32_t>(image.height),
                                              .format = format,
                                              .usage = TextureUsage::SAMPLED});
                if (!texture.isValid())
                    return {};
                if (!ctx.fillTexture(texture, rgbaData))
                {
                    ctx.destroyTexture(texture);
                    return {};
                }
                addBindlessTexture(bindlessDescriptorSet, texture, sampler);
                cache.imageTextures[cacheKey] = texture;
                return texture;
            };

            mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)] =
                uploadTexture(material->pbrMetallicRoughness.baseColorTexture.index, KtxSlot::COLOR);
            // Unlit shading reads only the base color; the PBR textures are a fallback for viewers without the extension.
            if (mat.type != MaterialType::UNLIT)
            {
                auto mr = uploadTexture(material->pbrMetallicRoughness.metallicRoughnessTexture.index, KtxSlot::DATA);
                mat.textures[static_cast<uint32_t>(PbrSlot::METALLIC)] = mr;
                mat.textures[static_cast<uint32_t>(PbrSlot::ROUGHNESS)] = mr;
                mat.textures[static_cast<uint32_t>(PbrSlot::NORMAL)] = uploadTexture(material->normalTexture.index, KtxSlot::NORMAL);
                mat.textures[static_cast<uint32_t>(PbrSlot::OCCLUSION)] = uploadTexture(material->occlusionTexture.index, KtxSlot::OCCLUSION);
                mat.textures[static_cast<uint32_t>(PbrSlot::EMISSIVE)] = uploadTexture(material->emissiveTexture.index, KtxSlot::COLOR);
            }
            mat.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)] = static_cast<float>(material->normalTexture.scale);
            mat.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)] = static_cast<float>(material->occlusionTexture.strength);
            for (uint32_t i = 0; i < 3; ++i)
                mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + i] = static_cast<float>(material->emissiveFactor[i]);
            float emissiveStrength = 1.0f;
            if (auto it = material->extensions.find("KHR_materials_emissive_strength"); it != material->extensions.end())
                emissiveStrength = getNumber(it->second, "emissiveStrength", 1.0f);
            mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_STRENGTH)] = emissiveStrength;
        }

        registry.materials.resize(registry.materialHandles.size());
        registry.materials[handle.index] = mat;
        writeMaterialRow(handle);
        cache.materials[materialIndex] = handle;
        return handle;
    }

    const Material &Engine::getMaterial(const MaterialHandle &handle)
    {
        if (isLive(handle))
            return registry.materials[handle.index];

        if (handle.isValid())
            spdlog::error("Invalid material handle {}; using the default material.", handle.index);
        if (defaultMaterial.isValid())
            return registry.materials[defaultMaterial.index];

        // Only reachable when engine construction failed before creating the default material.
        static const Material noMaterial{};
        spdlog::error("No default material; the engine did not initialize.");
        return noMaterial;
    }

}
