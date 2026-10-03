#include "rasm/core/mesh.h"
#include "rasm/core/engine.h"
#include "rasm/gfx/types.h"

#define TINYGLTF_IMPLEMENTATION
#include "tiny_gltf.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include <glm/gtc/type_ptr.hpp>

namespace rasm
{
    MeshHandle Engine::createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    {
        auto handle = handleManager.getNextMeshHandle();
        if (handle.index >= MAX_MESHES)
        {
            spdlog::error("Exceeded maximum number of meshes.");
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
            mesh.data = processGLTF(model);
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

            for (const auto &shape : shapes)
            {
                for (auto &index : shape.mesh.indices)
                {
                    Vertex v = {};

                    v.pos = {attrib.vertices[index.vertex_index * 3], -attrib.vertices[index.vertex_index * 3 + 1], attrib.vertices[index.vertex_index * 3 + 2]};
                    if (attrib.normals.size() > 0)
                        v.normal = {attrib.normals[index.normal_index * 3], -attrib.normals[index.normal_index * 3 + 1], attrib.normals[index.normal_index * 3 + 2]};
                    if (attrib.texcoords.size() > 0)
                        v.uv = {attrib.texcoords[index.texcoord_index * 2], 1.0 - attrib.texcoords[index.texcoord_index * 2 + 1]};

                    out_vertices.push_back(v);
                    out_indices.push_back(static_cast<uint32_t>(out_indices.size()));
                }
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

    MeshHandle Engine::loadGLTF(const std::string &path)
    {
        return loadMesh(path);
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
            bufferDesc.buffer.offset = objRaw.vertices.size() * sizeof(Vertex);

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

    std::vector<SubGLTFRaw> Engine::extractGLTFRaws(const tinygltf::Model &model, uint32_t meshIndex, const glm::mat4 &globalTransform)
    {
        std::vector<SubGLTFRaw> subGltfs;

        const auto &mesh = model.meshes[meshIndex];
        for (const auto &primitive : mesh.primitives)
        {

            // --- 1. EXTRACT VERTEX ATTRIBUTES ---
            std::vector<Vertex> vertices;
            size_t vertexCount = 0;

            // Get pointers to the standard attributes
            const float *posPtr = nullptr;
            const float *normPtr = nullptr;
            const float *uvPtr = nullptr;

            size_t posStride = 0, normStride = 0, uvStride = 0;

            // Positions (REQUIRED for a valid mesh)
            if (primitive.attributes.count("POSITION") > 0)
            {
                const tinygltf::Accessor &acc = model.accessors[primitive.attributes.at("POSITION")];
                const tinygltf::BufferView &bv = model.bufferViews[acc.bufferView];
                posPtr = reinterpret_cast<const float *>(&(model.buffers[bv.buffer].data[acc.byteOffset + bv.byteOffset]));
                vertexCount = acc.count;
                posStride = acc.ByteStride(bv) / sizeof(float);
            }

            // Normals (OPTIONAL)
            if (primitive.attributes.count("NORMAL") > 0)
            {
                const tinygltf::Accessor &acc = model.accessors[primitive.attributes.at("NORMAL")];
                const tinygltf::BufferView &bv = model.bufferViews[acc.bufferView];
                normPtr = reinterpret_cast<const float *>(&(model.buffers[bv.buffer].data[acc.byteOffset + bv.byteOffset]));
                normStride = acc.ByteStride(bv) / sizeof(float);
            }

            // Texture Coordinates / UVs (OPTIONAL)
            if (primitive.attributes.count("TEXCOORD_0") > 0)
            {
                const tinygltf::Accessor &acc = model.accessors[primitive.attributes.at("TEXCOORD_0")];
                const tinygltf::BufferView &bv = model.bufferViews[acc.bufferView];
                uvPtr = reinterpret_cast<const float *>(&(model.buffers[bv.buffer].data[acc.byteOffset + bv.byteOffset]));
                uvStride = acc.ByteStride(bv) / sizeof(float);
            }

            // TODO: Extract all remaining vertex attributes (tangents, colors,TEXCOORD_0,...)

            // Interleave the data into our standard Vertex struct
            for (size_t i = 0; i < vertexCount; ++i)
            {
                Vertex v{};

                if (posPtr)
                {
                    v.pos[0] = posPtr[i * posStride + 0];
                    v.pos[1] = posPtr[i * posStride + 1];
                    v.pos[2] = posPtr[i * posStride + 2];
                }
                if (normPtr)
                {
                    v.normal[0] = normPtr[i * normStride + 0];
                    v.normal[1] = normPtr[i * normStride + 1];
                    v.normal[2] = normPtr[i * normStride + 2];
                }
                if (uvPtr)
                {
                    v.uv[0] = uvPtr[i * uvStride + 0];
                    v.uv[1] = uvPtr[i * uvStride + 1];
                }
                vertices.push_back(v);
            }

            // --- 2. EXTRACT INDEX BUFFER ---
            std::vector<uint32_t> indices;

            if (primitive.indices > -1)
            {
                const tinygltf::Accessor &indexAccessor = model.accessors[primitive.indices];
                const tinygltf::BufferView &indexBufferView = model.bufferViews[indexAccessor.bufferView];
                const tinygltf::Buffer &indexBuffer = model.buffers[indexBufferView.buffer];

                const unsigned char *indexData = &(indexBuffer.data[indexAccessor.byteOffset + indexBufferView.byteOffset]);

                // Indices can be 8-bit, 16-bit, or 32-bit unsigned integers
                for (size_t i = 0; i < indexAccessor.count; ++i)
                {
                    if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                    {
                        indices.push_back(reinterpret_cast<const uint32_t *>(indexData)[i]);
                    }
                    else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                    {
                        indices.push_back(reinterpret_cast<const uint16_t *>(indexData)[i]);
                    }
                    else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                    {
                        indices.push_back(reinterpret_cast<const uint8_t *>(indexData)[i]);
                    }
                }
            }

            // --- 3. EXTRACT MATERIAL INFO (if any)
            int materialIndex = primitive.material;
            MaterialHandle materialHandle = loadMaterial(model, materialIndex);
            subGltfs.push_back({{vertices, indices}, globalTransform, materialHandle});
        }

        return subGltfs;
    }

    // Helper: Convert a glTF node's properties into a local GLM matrix
    glm::mat4 getLocalTransform(const tinygltf::Node &node)
    {
        // 1. If the node has a direct 4x4 matrix defined
        if (node.matrix.size() == 16)
        {
            return glm::make_mat4(node.matrix.data());
        }

        // 2. Otherwise, construct it from Translation, Rotation, and Scale (TRS)
        glm::mat4 transform = glm::mat4(1.0f);

        // Translation (Default:)
        if (node.translation.size() == 3)
        {
            transform = glm::translate(transform, glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
        }

        // Rotation (Quaternion: [x, y, z, w], Default:)
        if (node.rotation.size() == 4)
        {
            // Note: glm::quat constructor is glm::quat(w, x, y, z)
            glm::quat q(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
            transform = transform * glm::mat4_cast(q);
        }

        // Scale (Default:)
        if (node.scale.size() == 3)
        {
            transform = glm::scale(transform, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
        }

        return transform;
    }

    // Recursive function to traverse the scene graph node tree
    std::vector<SubGLTFRaw> Engine::traverseNodes(const tinygltf::Model &model, int nodeIndex, const glm::mat4 &parentTransform)
    {
        if (nodeIndex < 0 || nodeIndex >= model.nodes.size())
            return {};

        const tinygltf::Node &node = model.nodes[nodeIndex];

        // Compute this node's local matrix, then combine with its parent's world matrix
        glm::mat4 localTransform = getLocalTransform(node);
        glm::mat4 globalTransform = parentTransform * localTransform;

        std::vector<SubGLTFRaw> subGltfs;
        // If this node instantiates a mesh, we now have its world transform!
        if (node.mesh >= 0)
        {
            auto extractedRaws = extractGLTFRaws(model, node.mesh, globalTransform);
            subGltfs.insert(subGltfs.end(), extractedRaws.begin(), extractedRaws.end());
        }

        // Recursively process all child nodes
        for (int childIndex : node.children)
        {
            auto childRaws = traverseNodes(model, childIndex, globalTransform);
            subGltfs.insert(subGltfs.end(), childRaws.begin(), childRaws.end());
        }
        return subGltfs;
    }

    // Entry point: Start traversal from the root nodes of the active scene
    GLTFRaw Engine::processGLTF(const tinygltf::Model &model)
    {
        // Default to the first scene if model.defaultScene is not specified
        int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
        if (model.scenes.empty())
            return {};

        const tinygltf::Scene &scene = model.scenes[sceneIndex];

        GLTFRaw gltfRaw;
        gltfRaw.model = model;

        // Root transform flips Y into the engine's Vulkan clip-space convention (Y down), matching what the
        // OBJ loader does per-vertex. Applied on the left so it acts after the glTF node hierarchy.
        const glm::mat4 rootTransform = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));

        for (int rootNodeIndex : scene.nodes)
        {
            auto extractedRaws = traverseNodes(model, rootNodeIndex, rootTransform);
            gltfRaw.subGltfRaws.insert(gltfRaw.subGltfRaws.end(), extractedRaws.begin(), extractedRaws.end());
        }

        return gltfRaw;
    }

    GLTFRaw Engine::processMesh(const Mesh &mesh)
    {
        if (mesh.type == Mesh::MeshType::OBJ)
        {
            return GLTFRaw{tinygltf::Model{}, std::vector<SubGLTFRaw>{SubGLTFRaw{std::get<ObjRaw>(mesh.data), glm::mat4(1.0f), MaterialHandle{}}}};
        }
        else if (mesh.type == Mesh::MeshType::GLTF)
        {
            return std::get<GLTFRaw>(mesh.data);
        }
        return GLTFRaw{};
    }

    MeshSize Engine::getMeshByteSize(const MeshHandle &handle) const
    {
        if (!handle.isValid() || handle.index >= registery.meshes.size())
        {
            spdlog::error("Mesh handle not found for getting byte size.");
            return {};
        }

        auto mesh = registery.meshes[handle.index];

        MeshSize meshSize{};

        if (mesh.type == Mesh::MeshType::GLTF)
        {
            // GLTF model should have been converted to ObjRaw before calculating byte size.
            return MeshSize{0, 0}; // Placeholder
        }
        else if (mesh.type == Mesh::MeshType::OBJ)
        {
            auto &objRaw = std::get<ObjRaw>(mesh.data);
            return MeshSize{
                objRaw.vertices.size() * sizeof(Vertex), // vertices byte size
                objRaw.indices.size() * sizeof(uint32_t) // indices byte size
            };
        }
        else
        {
            spdlog::error("Unknown mesh type for getting byte size.");
            return MeshSize{0, 0};
        }
    }
}
