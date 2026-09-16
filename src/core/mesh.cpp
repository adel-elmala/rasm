#include "rasm/core/mesh.h"
#include "rasm/core/engine.h"

#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

namespace rasm
{
    MeshHandle Engine::createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    {
        auto handle = handleManager.getNextMeshHandle();
        if (handle.index >= MAX_MESHES)
        {
            spdlog::error("Exceeded maximum number of meshes.");
            isRunning = false;
            return {};
        }

        ObjRaw objRaw{};
        objRaw.vertices = std::move(vertices);
        objRaw.indices = std::move(indices);

        Mesh mesh{};
        mesh.type = Mesh::MeshType::OBJ;
        mesh.data = std::move(objRaw);

        registery.meshes.resize(handle.index + 1);
        registery.meshes[handle.index] = std::move(mesh);

        return handle;
    }

    MeshHandle Engine::loadMesh(const std::string &path)
    {
        // find if the mesh is already loaded, if so return existing handle
        if (registery.loadedMeshes.find(path) != registery.loadedMeshes.end())
        {
            return registery.loadedMeshes[path];
        }

        // find if the model extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedMeshExtensions.find(extension) == supportedMeshExtensions.end())
        {
            spdlog::error("Unsupported mesh format: {}", extension);
            return MeshHandle{};
        }

        auto handle = handleManager.getNextMeshHandle();
        if (handle.index >= MAX_MESHES)
        {
            spdlog::warn("Exceeded maximum number of meshes.");
            return MeshHandle{};
        }

        Mesh mesh{};
        if (extension == "gltf" || extension == "glb")
        {
            tinygltf::TinyGLTF loader;
            tinygltf::Model model;

            std::string err;
            std::string warn;
            bool ret = false;
            if (extension == "gltf")
            {
                ret = loader.LoadASCIIFromFile(&model, &err, &warn, path);
            }
            else
            {
                ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
            }

            if (!warn.empty())
            {
                spdlog::warn("GLTF loader warning: {}", warn);
            }
            if (!err.empty())
            {
                spdlog::error("GLTF loader error: {}", err);
            }
            if (!ret)
            {
                spdlog::error("Failed to load GLTF model: {}", path);
                return MeshHandle{};
            }

            mesh.type = Mesh::MeshType::GLTF;
            mesh.data = std::move(model);
        }
        else
        {
            tinyobj::attrib_t attrib;
            std::vector<tinyobj::shape_t> shapes;
            std::vector<tinyobj::material_t> materials;

            if (!tinyobj::LoadObj(&attrib, &shapes, &materials, nullptr, nullptr, path.c_str()))
            {
                spdlog::error("Failed to load OBJ model: {}", path);
                return MeshHandle{};
            }
            if (shapes.empty())
            {
                spdlog::error("OBJ model contains no shapes: {}", path);
                return MeshHandle{};
            }

            // Load vertex and index data
            std::vector<Vertex> out_vertices;
            std::vector<uint32_t> out_indices;

            for (auto &index : shapes[0].mesh.indices)
            {
                Vertex v{
                    .pos = {attrib.vertices[index.vertex_index * 3], -attrib.vertices[index.vertex_index * 3 + 1], attrib.vertices[index.vertex_index * 3 + 2]},
                    .normal = {attrib.normals[index.normal_index * 3], -attrib.normals[index.normal_index * 3 + 1], attrib.normals[index.normal_index * 3 + 2]},
                    .uv = {attrib.texcoords[index.texcoord_index * 2], 1.0 - attrib.texcoords[index.texcoord_index * 2 + 1]}};

                out_vertices.push_back(v);
                out_indices.push_back(static_cast<uint32_t>(out_indices.size()));
            }

            ObjRaw objRaw{};
            objRaw.vertices = std::move(out_vertices);
            objRaw.indices = std::move(out_indices);

            mesh.type = Mesh::MeshType::OBJ;
            mesh.data = std::move(objRaw);
        }

        registery.loadedMeshes[path] = handle;

        registery.meshes.resize(handle.index + 1);
        registery.meshes[handle.index] = std::move(mesh);

        return handle;
    }

    BufferHandle Engine::uploadMesh(const MeshHandle &handle)
    {
        // TODO: remove mesh from meshData after uploading to GPU.
        if (!handle.isValid() || handle.index >= registery.meshes.size())
        {
            spdlog::error("Mesh handle not found for upload.");
            return BufferHandle{};
        }

        auto mesh = registery.meshes[handle.index];

        if (mesh.type == Mesh::MeshType::GLTF)
        {
            // In a real implementation, this is where we'd upload the GLTF mesh data to the GPU.
            spdlog::info("Uploading GLTF mesh with handle: {}", handle.index);
        }
        else if (mesh.type == Mesh::MeshType::OBJ)
        {
            auto &objRaw = std::get<ObjRaw>(mesh.data);

            auto bufferDesc = ResourceDesc{};
            bufferDesc.name = "OBJ Vertex + index Buffer";
            bufferDesc.type = ResourceType::BUFFER;
            bufferDesc.buffer.usage = BufferUsage::VERTEXINDEX;
            bufferDesc.buffer.size = objRaw.vertices.size() * sizeof(Vertex) + objRaw.indices.size() * sizeof(uint32_t);
            bufferDesc.buffer.vertexIndexBuffer.indexCount = objRaw.indices.size();
            bufferDesc.buffer.vertexIndexBuffer.vertexCount = objRaw.vertices.size();
            bufferDesc.buffer.vertexIndexBuffer.offset = objRaw.vertices.size() * sizeof(Vertex);

            auto bufferHandle = ctx.createBuffer(bufferDesc);

            ctx.fillBuffer(bufferHandle, objRaw.vertices.data(), objRaw.vertices.size() * sizeof(Vertex), 0);
            ctx.fillBuffer(bufferHandle, objRaw.indices.data(), objRaw.indices.size() * sizeof(uint32_t), objRaw.vertices.size() * sizeof(Vertex));
            return bufferHandle;
        }
        else
        {
            spdlog::error("Unknown mesh type for upload.");
            return BufferHandle{};
        }

        return BufferHandle{};
    }

}
