#include "rasm/material.h"
#include "rasm/renderer.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    MaterialHandle Renderer::createMaterial(MaterialType type, std::vector<TextureHandle> textures, bool doubleSided)
    {
        const HandleSlot slot = registry.materialHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::error("The material table is full: {} materials are alive. RendererConfig::maxMaterials ({}) sets its size.", registry.materialHandles.liveCount(), config.maxMaterials);
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
        case MaterialType::TERRAIN:
            material.name = "terrain";
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
    void Renderer::destroyMaterial(MaterialHandle material)
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
    void Renderer::detachMaterial(MaterialHandle material)
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

    void Renderer::updateMaterial(MaterialHandle material, const Material &values)
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
        const TextureHandle heights = values.terrain.heights;
        if (heights.isValid() && getBindlessTextureIndex(heights) == UINT32_MAX)
            addBindlessTexture(bindlessDescriptorSet, heights, SamplerType::NEAREST_CLAMP);
        texturesLoading = texturesLoading || (heights.isValid() && !isLoaded(heights));

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

    const Material &Renderer::getMaterial(const MaterialHandle &handle)
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
