#include "rasm/mesh.h"
#include "rasm/renderer.h"
#include "assets/meshBuild.h"
#include "assets/meshData.h"
#include "assets/meshLod.h"
#include "assets/meshlets.h"
#include "renderer/animation.h"
#include "renderer/geometryPool.h"
#include "renderer/loadHooks.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rasm
{
    namespace
    {
        uint32_t toUnorm8(float value)
        {
            return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        float encodeSrgb(float linear)
        {
            linear = std::clamp(linear, 0.0f, 1.0f);
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        }
    }

    // RGB is stored sRGB-encoded, so 8 bits keep the dark shades that linear values lose; the vertex shader decodes it
    // back to linear before interpolation. Alpha stays linear. R is the lowest byte.
    uint32_t packVertexColor(const glm::vec4 &linearColor)
    {
        return toUnorm8(encodeSrgb(linearColor.r)) | toUnorm8(encodeSrgb(linearColor.g)) << 8 | toUnorm8(encodeSrgb(linearColor.b)) << 16 | toUnorm8(linearColor.a) << 24;
    }

    MeshBuild::MeshBuild() = default;
    MeshBuild::~MeshBuild() = default;
    MeshBuild::MeshBuild(MeshBuild &&other) noexcept = default;
    MeshBuild &MeshBuild::operator=(MeshBuild &&other) noexcept = default;

    bool MeshBuild::isValid() const
    {
        return data != nullptr;
    }

    MeshBuild buildMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<uint32_t> colors, const MeshBuildOptions &options)
    {
        MeshBuild build;
        if (!colors.empty() && colors.size() != vertices.size())
        {
            spdlog::error("buildMesh: {} colors for {} vertices; give one per vertex, or none.", colors.size(), vertices.size());
            return build;
        }
        // The importers check their indices; the application's are checked here, before the LOD builder reads them.
        if (indices.size() % 3 != 0)
        {
            spdlog::error("buildMesh: {} indices is not a whole number of triangles.", indices.size());
            return build;
        }
        for (uint32_t index : indices)
        {
            if (index >= vertices.size())
            {
                spdlog::error("buildMesh: index {} is out of range for {} vertices.", index, vertices.size());
                return build;
            }
        }
        build.data = std::make_unique<MeshBuild::Data>();
        ObjRaw &raw = build.data->raw;
        raw = ObjRaw{.vertices = std::move(vertices), .indices = std::move(indices), .colors = std::move(colors)};
        if (options.lods)
            buildMeshLods(raw);
        if (options.meshlets)
            buildMeshlets(raw);
        return build;
    }

    MeshHandle Renderer::createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<uint32_t> colors)
    {
        MeshBuild build = buildMesh(std::move(vertices), std::move(indices), std::move(colors), {.lods = config.meshLods, .meshlets = true});
        if (!build.isValid())
            return {};
        return createMesh(std::move(build));
    }

    MeshHandle Renderer::createMesh(MeshBuild &&build)
    {
        if (!build.isValid())
        {
            spdlog::error("createMesh: the mesh build is not valid (buildMesh refused its data, or a createMesh took it).");
            return {};
        }
        const std::unique_ptr<MeshBuild::Data> data = std::move(build.data);
        const ObjRaw &raw = data->raw;

        // The handle is taken once the geometry is in the pool, so a mesh that does not fit uses no index.
        Mesh mesh{.state = MeshState::READY, .subMeshes = {SubMesh{.transform = glm::mat4(1.0f)}}};
        if (!geometryPool->check(raw))
            return {};
        if (!geometryPool->allocate(raw, mesh.subMeshes[0].geometry))
        {
            geometryPool->logFull(1);
            return {};
        }
        if (!geometryPool->write(ctx, raw, mesh.subMeshes[0].geometry))
        {
            geometryPool->free(mesh.subMeshes[0].geometry);
            return {};
        }

        mesh.labels = std::move(data->labels);
        const HandleSlot slot = registry.meshHandles.allocate();
        const MeshHandle handle{slot.index, slot.generation};
        registry.meshes.resize(registry.meshHandles.size());
        registry.meshes[handle.index] = std::move(mesh);

        return handle;
    }

    // A glTF's textures are filled after its mesh is ready, a few per frame (P2), so it counts as loaded once they are.
    bool Renderer::isLoaded(MeshHandle mesh) const
    {
        if (!isLive(mesh) || registry.meshes[mesh.index].state != MeshState::READY)
            return false;
        for (const TextureHandle &texture : registry.meshes[mesh.index].ownedTextures)
        {
            if (loadingTextures.contains(texture))
                return false;
        }
        return true;
    }

    // Entities that use the mesh keep living and draw nothing. Its geometry ranges, table rows and animation pool range
    // wait for the frames in flight, which may still draw it; its materials and textures go with it.
    void Renderer::destroyMesh(MeshHandle mesh)
    {
        if (!isLive(mesh))
        {
            spdlog::error("destroyMesh: mesh handle {{{}, {}}} is invalid or was destroyed.", mesh.index, mesh.generation);
            return;
        }
        for (const MeshHandle &grid : terrainMeshes)
        {
            if (grid == mesh)
            {
                spdlog::error("destroyMesh: mesh {{{}, {}}} is a terrain grid (getTerrainMesh), which every terrain tile shares; the renderer keeps it.", mesh.index, mesh.generation);
                return;
            }
        }
        // The loader drops a load still in its pool, and the path leaves its cache.
        if (loads)
            loads->forget(mesh);
        Mesh &meshRef = registry.meshes[mesh.index];
        // The usual order destroys the entities first, and then nothing is searched.
        for (size_t i = 1; meshRef.users > 0 && i < registry.entities.size(); ++i)
        {
            Entity &entity = registry.entities[i];
            if (!(entity.mesh == mesh) || !isLive(entity.handle))
                continue;
            releaseEntityRows(entity.handle);
            entity.mesh = {};
            --meshRef.users;
        }

        std::vector<MeshGeometry> geometries;
        for (const SubMesh &subMesh : meshRef.subMeshes)
        {
            if (subMesh.geometry.allocated)
                geometries.push_back(subMesh.geometry);
        }
        if (!geometries.empty())
            ctx.releaseAfterGpu([this, geometries]()
                                {
                                    for (const MeshGeometry &geometry : geometries)
                                        geometryPool->free(geometry); });
        releaseMeshAnimation(mesh);

        const std::vector<MaterialHandle> materials = std::move(meshRef.ownedMaterials);
        const std::vector<TextureHandle> textures = std::move(meshRef.ownedTextures);
        registry.meshes[mesh.index] = Mesh{};
        registry.meshHandles.release(mesh.index, mesh.generation);

        for (MaterialHandle material : materials)
        {
            if (isLive(material))
                destroyMaterial(material);
        }
        for (TextureHandle texture : textures)
        {
            if (ctx.isAlive(texture))
                destroyTexture(texture);
        }
    }

    MeshSize Renderer::getMeshByteSize(const MeshHandle &handle) const
    {
        if (!isLive(handle))
        {
            spdlog::error("Mesh handle not found for getting byte size.");
            return {};
        }

        MeshSize meshSize{};
        for (const auto &subMesh : registry.meshes[handle.index].subMeshes)
        {
            meshSize.verticesByteSize += subMesh.geometry.vertexCount * sizeof(Vertex) + subMesh.geometry.colorCount * sizeof(uint32_t) + subMesh.geometry.skinCount * sizeof(SkinVertex) +
                                         subMesh.geometry.morphCount * sizeof(MorphDelta);
            meshSize.indicesByteSize += (subMesh.geometry.indexCount + subMesh.geometry.lodIndexCount) * sizeof(uint32_t);
        }
        return meshSize;
    }
}
