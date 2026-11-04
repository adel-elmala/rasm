#include "rasm/mesh.h"
#include "rasm/engine.h"
#include "assets/meshImport.h"
#include "platform/paths.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "tiny_gltf.h"
#include "tiny_obj_loader.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <string>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <vector>

namespace rasm
{
    namespace
    {
        // An accessor's elements copied out of the buffer, tightly packed (count * components * componentSize bytes),
        // with byteStride and sparse substitution already applied.
        struct AccessorData
        {
            std::vector<unsigned char> bytes;
            size_t count = 0;
            uint32_t components = 0;
            uint32_t componentSize = 0;
            int componentType = -1;
            bool normalized = false;
        };

        // Returns `size` bytes of buffer view `viewIndex` starting at `offset`, or nullptr if the view or the range is invalid.
        const unsigned char *getBufferViewData(const tinygltf::Model &model, int viewIndex, size_t offset, size_t size)
        {
            if (viewIndex < 0 || static_cast<size_t>(viewIndex) >= model.bufferViews.size())
                return nullptr;
            const tinygltf::BufferView &view = model.bufferViews[viewIndex];
            if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= model.buffers.size())
                return nullptr;
            const tinygltf::Buffer &buffer = model.buffers[view.buffer];
            if (view.byteOffset > buffer.data.size() || view.byteLength > buffer.data.size() - view.byteOffset)
                return nullptr;
            if (offset > view.byteLength || size > view.byteLength - offset)
                return nullptr;
            return buffer.data.data() + view.byteOffset + offset;
        }

        // Decodes one index component. Indices stay integers; going through float would lose precision above 2^24.
        // glTF data is not guaranteed to be aligned, so components are read with memcpy.
        bool readIndexComponent(const unsigned char *src, int componentType, uint32_t &out)
        {
            switch (componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            {
                uint8_t value;
                std::memcpy(&value, src, sizeof(value));
                out = value;
                return true;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            {
                uint16_t value;
                std::memcpy(&value, src, sizeof(value));
                out = value;
                return true;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                std::memcpy(&out, src, sizeof(out));
                return true;
            default:
                return false;
            }
        }

        // Decodes one attribute component. Attributes may be quantized (KHR_mesh_quantization): normalized integers map
        // to [0, 1] or [-1, 1] as the glTF spec defines, and non-normalized integers keep their integer value.
        bool readFloatComponent(const unsigned char *src, int componentType, bool normalized, float &out)
        {
            switch (componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                std::memcpy(&out, src, sizeof(out));
                return true;
            case TINYGLTF_COMPONENT_TYPE_BYTE:
            {
                int8_t value;
                std::memcpy(&value, src, sizeof(value));
                out = normalized ? std::max(value / 127.0f, -1.0f) : value;
                return true;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            {
                uint8_t value;
                std::memcpy(&value, src, sizeof(value));
                out = normalized ? value / 255.0f : value;
                return true;
            }
            case TINYGLTF_COMPONENT_TYPE_SHORT:
            {
                int16_t value;
                std::memcpy(&value, src, sizeof(value));
                out = normalized ? std::max(value / 32767.0f, -1.0f) : value;
                return true;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            {
                uint16_t value;
                std::memcpy(&value, src, sizeof(value));
                out = normalized ? value / 65535.0f : value;
                return true;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            {
                uint32_t value;
                std::memcpy(&value, src, sizeof(value));
                out = static_cast<float>(value);
                return true;
            }
            default:
                return false;
            }
        }

        // Copies an accessor's elements into `out`. Handles byteStride, accessors without a buffer view (all zeros) and
        // sparse substitution. Returns false if the accessor is malformed or reads outside its buffer view.
        bool gatherAccessor(const tinygltf::Model &model, int accessorIndex, AccessorData &out)
        {
            if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size())
                return false;
            const tinygltf::Accessor &acc = model.accessors[accessorIndex];

            const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(acc.componentType));
            const int numComponents = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type));
            if (componentSize <= 0 || numComponents <= 0)
                return false;

            out.count = acc.count;
            out.components = static_cast<uint32_t>(numComponents);
            out.componentSize = static_cast<uint32_t>(componentSize);
            out.componentType = acc.componentType;
            out.normalized = acc.normalized;

            const size_t elementSize = static_cast<size_t>(componentSize) * out.components;
            out.bytes.assign(acc.count * elementSize, 0);
            if (acc.count == 0)
                return true;

            if (acc.bufferView >= 0)
            {
                if (static_cast<size_t>(acc.bufferView) >= model.bufferViews.size())
                    return false;
                const int stride = acc.ByteStride(model.bufferViews[acc.bufferView]);
                if (stride <= 0 || static_cast<size_t>(stride) < elementSize)
                    return false;

                const size_t byteSize = (acc.count - 1) * static_cast<size_t>(stride) + elementSize;
                const unsigned char *data = getBufferViewData(model, acc.bufferView, acc.byteOffset, byteSize);
                if (!data)
                    return false;

                for (size_t i = 0; i < acc.count; ++i)
                    std::memcpy(out.bytes.data() + i * elementSize, data + i * stride, elementSize);
            }

            if (acc.sparse.isSparse && acc.sparse.count > 0)
            {
                const auto &sparse = acc.sparse;
                const size_t sparseCount = static_cast<size_t>(sparse.count);
                const int indexSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(sparse.indices.componentType));
                if (indexSize <= 0 || sparseCount > acc.count)
                    return false;

                // Sparse indices and values are tightly packed.
                const unsigned char *indexData = getBufferViewData(model, sparse.indices.bufferView, sparse.indices.byteOffset, sparseCount * indexSize);
                const unsigned char *valueData = getBufferViewData(model, sparse.values.bufferView, sparse.values.byteOffset, sparseCount * elementSize);
                if (!indexData || !valueData)
                    return false;

                for (size_t s = 0; s < sparseCount; ++s)
                {
                    uint32_t target = 0;
                    if (!readIndexComponent(indexData + s * indexSize, sparse.indices.componentType, target) || target >= acc.count)
                        return false;
                    std::memcpy(out.bytes.data() + target * elementSize, valueData + s * elementSize, elementSize);
                }
            }

            return true;
        }

        // Reads an attribute accessor as `count * components` floats.
        bool readFloatAccessor(const tinygltf::Model &model, int accessorIndex, std::vector<float> &out, uint32_t &components)
        {
            AccessorData data;
            if (!gatherAccessor(model, accessorIndex, data))
                return false;

            out.resize(data.count * data.components);
            for (size_t i = 0; i < out.size(); ++i)
                if (!readFloatComponent(data.bytes.data() + i * data.componentSize, data.componentType, data.normalized, out[i]))
                    return false;
            components = data.components;
            return true;
        }

        // Reads a SCALAR index accessor.
        bool readIndexAccessor(const tinygltf::Model &model, int accessorIndex, std::vector<uint32_t> &out)
        {
            AccessorData data;
            if (!gatherAccessor(model, accessorIndex, data) || data.components != 1)
                return false;

            out.resize(data.count);
            for (size_t i = 0; i < out.size(); ++i)
                if (!readIndexComponent(data.bytes.data() + i * data.componentSize, data.componentType, out[i]))
                    return false;
            return true;
        }

        // Reads an optional vertex attribute. Returns false (with a warning) if it is missing, malformed, has fewer than
        // `minComponents` components or a count different from POSITION's.
        bool readVertexAttribute(const tinygltf::Model &model, const tinygltf::Primitive &primitive, const std::string &name, size_t vertexCount,
                                 uint32_t minComponents, std::vector<float> &out, uint32_t &components, const std::string &primitiveName)
        {
            auto it = primitive.attributes.find(name);
            if (it == primitive.attributes.end())
                return false;
            if (!readFloatAccessor(model, it->second, out, components) || components < minComponents || out.size() != vertexCount * components)
            {
                spdlog::warn("glTF {}: ignoring invalid {} attribute.", primitiveName, name);
                return false;
            }
            return true;
        }

        // tinygltf leaves mode at -1 when it is absent; the glTF default is TRIANGLES.
        bool isTriangleMode(int mode)
        {
            return mode == -1 || mode == TINYGLTF_MODE_TRIANGLES || mode == TINYGLTF_MODE_TRIANGLE_STRIP || mode == TINYGLTF_MODE_TRIANGLE_FAN;
        }

        // Converts a triangle-mode primitive to a triangle list (glTF 2.0 spec, section 3.7.2.1). A trailing
        // incomplete triangle is dropped.
        std::vector<uint32_t> toTriangleList(int mode, const std::vector<uint32_t> &in)
        {
            assert(isTriangleMode(mode));
            std::vector<uint32_t> out;
            switch (mode)
            {
            case TINYGLTF_MODE_TRIANGLE_STRIP:
                // Odd triangles swap their last two vertices so every triangle keeps the strip's winding.
                for (size_t i = 0; i + 2 < in.size(); ++i)
                    out.insert(out.end(), {in[i], in[i + 1 + i % 2], in[i + 2 - i % 2]});
                break;
            case TINYGLTF_MODE_TRIANGLE_FAN:
                for (size_t i = 0; i + 2 < in.size(); ++i)
                    out.insert(out.end(), {in[i + 1], in[i + 2], in[0]});
                break;
            default:
                out.assign(in.begin(), in.begin() + (in.size() / 3) * 3);
                break;
            }
            return out;
        }

        struct PendingNode
        {
            int index;
            glm::mat4 parentTransform;
        };

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

        // tinyobj leaves this to the reader: a corner without the attribute has index -1, and a face can name
        // elements the file never defines (bunny.obj has "f 1/1/1" and no vn lines).
        bool hasObjAttribute(const std::vector<tinyobj::real_t> &values, int index, size_t components)
        {
            return index >= 0 && (static_cast<size_t>(index) + 1) * components <= values.size();
        }

        std::vector<SubGLTFRaw> extractGLTFRaws(const tinygltf::Model &model, uint32_t meshIndex, const glm::mat4 &globalTransform)
        {
            std::vector<SubGLTFRaw> subGltfs;

            if (meshIndex >= model.meshes.size())
            {
                spdlog::warn("glTF: skipping out-of-range mesh {} ({} meshes).", meshIndex, model.meshes.size());
                return subGltfs;
            }

            const auto &mesh = model.meshes[meshIndex];
            for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
            {
                const auto &primitive = mesh.primitives[primitiveIndex];
                const std::string primitiveName = "mesh " + std::to_string(meshIndex) + " ('" + mesh.name + "') primitive " + std::to_string(primitiveIndex);

                if (!isTriangleMode(primitive.mode))
                {
                    spdlog::warn("glTF {}: skipping unsupported primitive mode {} (only triangle modes are drawn).", primitiveName, primitive.mode);
                    continue;
                }

                // --- 1. EXTRACT VERTEX ATTRIBUTES ---
                // Positions are required.
                std::vector<float> positions;
                uint32_t posComponents = 0;
                auto posIt = primitive.attributes.find("POSITION");
                if (posIt == primitive.attributes.end() || !readFloatAccessor(model, posIt->second, positions, posComponents) || posComponents < 3)
                {
                    spdlog::warn("glTF {}: skipping primitive with a missing or invalid POSITION attribute.", primitiveName);
                    continue;
                }
                const size_t vertexCount = positions.size() / posComponents;
                if (vertexCount == 0 || vertexCount > UINT32_MAX)
                {
                    spdlog::warn("glTF {}: skipping primitive with {} vertices.", primitiveName, vertexCount);
                    continue;
                }

                std::vector<float> normals;
                uint32_t normComponents = 0;
                const bool hasNormals = readVertexAttribute(model, primitive, "NORMAL", vertexCount, 3, normals, normComponents, primitiveName);

                // Texture Coordinates / UVs (OPTIONAL). glTF materials reference a set per texture
                // (baseColor may use TEXCOORD_0 while the normal map uses TEXCOORD_2), so load every
                // supported set. Missing sets stay (0, 0).
                std::vector<float> uvs[MAX_VERTEX_UV_SETS];
                uint32_t uvComponents[MAX_VERTEX_UV_SETS] = {};
                bool hasUvs[MAX_VERTEX_UV_SETS] = {};
                for (uint32_t set = 0; set < MAX_VERTEX_UV_SETS; ++set)
                    hasUvs[set] = readVertexAttribute(model, primitive, "TEXCOORD_" + std::to_string(set), vertexCount, 2, uvs[set], uvComponents[set], primitiveName);

                // Tangents (OPTIONAL, VEC4). The glTF spec requires ignoring them when normals are generated.
                // Without them tangent.w stays 0 and the shader reconstructs a frame from UV derivatives.
                std::vector<float> tangents;
                uint32_t tangentComponents = 0;
                const bool hasTangents = hasNormals && readVertexAttribute(model, primitive, "TANGENT", vertexCount, 4, tangents, tangentComponents, primitiveName);

                // TODO: Extract all remaining vertex attributes (colors, joints, weights,...)

                // Interleave the data into our standard Vertex struct
                std::vector<Vertex> vertices(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i)
                {
                    Vertex &v = vertices[i];
                    v.pos = glm::make_vec3(&positions[i * posComponents]);
                    if (hasNormals)
                        v.normal = glm::make_vec3(&normals[i * normComponents]);
                    for (uint32_t set = 0; set < MAX_VERTEX_UV_SETS; ++set)
                        if (hasUvs[set])
                            v.uv[set] = glm::make_vec2(&uvs[set][i * uvComponents[set]]);
                    if (hasTangents)
                    {
                        v.tangent = glm::make_vec4(&tangents[i * tangentComponents]);
                        // w must be ±1; snapping it keeps 0 free to mean "no tangent" (quantized w can round off 1).
                        v.tangent.w = v.tangent.w < 0.0f ? -1.0f : 1.0f;
                    }
                }

                // --- 2. EXTRACT INDEX BUFFER ---
                // Non-indexed primitives draw their vertices in order, so generate the identity index list.
                std::vector<uint32_t> sourceIndices;
                if (primitive.indices > -1)
                {
                    if (!readIndexAccessor(model, primitive.indices, sourceIndices))
                    {
                        spdlog::warn("glTF {}: skipping primitive with an invalid index accessor.", primitiveName);
                        continue;
                    }
                }
                else
                {
                    sourceIndices.resize(vertexCount);
                    for (size_t i = 0; i < vertexCount; ++i)
                        sourceIndices[i] = static_cast<uint32_t>(i);
                }

                auto outOfRange = std::find_if(sourceIndices.begin(), sourceIndices.end(), [vertexCount](uint32_t index)
                                               { return index >= vertexCount; });
                if (outOfRange != sourceIndices.end())
                {
                    spdlog::warn("glTF {}: skipping primitive with index {} out of range for {} vertices.", primitiveName, *outOfRange, vertexCount);
                    continue;
                }

                std::vector<uint32_t> indices = toTriangleList(primitive.mode, sourceIndices);
                if (indices.empty())
                {
                    spdlog::warn("glTF {}: skipping primitive without a complete triangle.", primitiveName);
                    continue;
                }

                // Compute smooth normals if the primitive has no NORMAL attribute
                if (!hasNormals)
                {
                    for (size_t i = 0; i + 2 < indices.size(); i += 3)
                    {
                        Vertex &v0 = vertices[indices[i]];
                        Vertex &v1 = vertices[indices[i + 1]];
                        Vertex &v2 = vertices[indices[i + 2]];
                        glm::vec3 faceNormal = glm::cross(v1.pos - v0.pos, v2.pos - v0.pos);
                        v0.normal += faceNormal;
                        v1.normal += faceNormal;
                        v2.normal += faceNormal;
                    }
                    for (auto &vert : vertices)
                    {
                        float len2 = glm::dot(vert.normal, vert.normal);
                        if (len2 > 0.0f)
                            vert.normal *= glm::inversesqrt(len2);
                        else
                            vert.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                    }
                }

                // Materials create textures, so the main thread turns the index into a handle (finishMeshLoad).
                subGltfs.push_back({{std::move(vertices), std::move(indices)}, globalTransform, primitive.material});
            }

            return subGltfs;
        }

        // Depth-first walk of the node tree. glTF requires a strict tree, but nothing enforces it: an explicit stack
        // survives arbitrarily deep files and the visited list stops cycles.
        std::vector<SubGLTFRaw> traverseNodes(const tinygltf::Model &model, const std::vector<int> &rootNodes, const glm::mat4 &rootTransform)
        {
            std::vector<SubGLTFRaw> subGltfs;
            std::vector<bool> visited(model.nodes.size(), false);

            // Pushed in reverse so nodes pop in file order.
            std::vector<PendingNode> pending;
            for (auto root = rootNodes.rbegin(); root != rootNodes.rend(); ++root)
                pending.push_back({*root, rootTransform});

            while (!pending.empty())
            {
                const PendingNode current = pending.back();
                pending.pop_back();

                if (current.index < 0 || static_cast<size_t>(current.index) >= model.nodes.size())
                {
                    spdlog::warn("glTF: skipping out-of-range node {} ({} nodes).", current.index, model.nodes.size());
                    continue;
                }
                if (visited[current.index])
                {
                    spdlog::warn("glTF: skipping node {}, it has more than one parent or is part of a cycle.", current.index);
                    continue;
                }
                visited[current.index] = true;

                const tinygltf::Node &node = model.nodes[current.index];

                // Compute this node's local matrix, then combine with its parent's world matrix
                const glm::mat4 globalTransform = current.parentTransform * getLocalTransform(node);

                // If this node instantiates a mesh, we now have its world transform!
                if (node.mesh >= 0)
                {
                    auto extractedRaws = extractGLTFRaws(model, node.mesh, globalTransform);
                    subGltfs.insert(subGltfs.end(), std::make_move_iterator(extractedRaws.begin()), std::make_move_iterator(extractedRaws.end()));
                }

                for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
                    pending.push_back({*child, globalTransform});
            }

            return subGltfs;
        }

        // Entry point: Start traversal from the root nodes of the active scene
        std::vector<SubGLTFRaw> processGLTF(const tinygltf::Model &model)
        {
            if (model.scenes.empty())
                return {};

            // Default to the first scene if model.defaultScene is not specified
            int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
            if (static_cast<size_t>(sceneIndex) >= model.scenes.size())
            {
                spdlog::warn("glTF: default scene {} is out of range ({} scenes), using scene 0.", sceneIndex, model.scenes.size());
                sceneIndex = 0;
            }

            const tinygltf::Scene &scene = model.scenes[sceneIndex];

            // Root transform flips Y into the engine's Vulkan clip-space convention (Y down), matching what the
            // OBJ loader does per-vertex. Applied on the left so it acts after the glTF node hierarchy.
            const glm::mat4 rootTransform = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));

            return traverseNodes(model, scene.nodes, rootTransform);
        }

        GLTFRaw importGltf(const std::string &path, bool binary)
        {
            auto model = std::make_shared<tinygltf::Model>();
            tinygltf::TinyGLTF loader;

            std::string err;
            std::string warn;
            const bool ret = binary ? loader.LoadBinaryFromFile(model.get(), &err, &warn, path)
                                    : loader.LoadASCIIFromFile(model.get(), &err, &warn, path);

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
                return {};
            }

            GLTFRaw raw;
            raw.subGltfRaws = processGLTF(*model);
            // Every accessor has been read. Materials need only the decoded images, so the raw buffers go now (P6).
            model->buffers.clear();
            raw.model = std::move(model);
            raw.loaded = true;
            return raw;
        }

        GLTFRaw importObj(const std::string &path)
        {
            tinyobj::attrib_t attrib;
            std::vector<tinyobj::shape_t> shapes;
            std::vector<tinyobj::material_t> materials;

            // tinyobj opens a file name with the ANSI code page, so it gets a stream instead.
            std::ifstream file(pathFromUtf8(path));
            if (!file.is_open() || !tinyobj::LoadObj(&attrib, &shapes, &materials, nullptr, nullptr, &file))
            {
                spdlog::error("Failed to load OBJ model: {}", path);
                return {};
            }
            if (shapes.empty())
            {
                spdlog::error("OBJ model contains no shapes: {}", path);
                return {};
            }

            // Load vertex and index data
            ObjRaw objRaw{};

            for (const auto &shape : shapes)
            {
                for (auto &index : shape.mesh.indices)
                {
                    Vertex v = {};

                    v.pos = {attrib.vertices[index.vertex_index * 3], -attrib.vertices[index.vertex_index * 3 + 1], attrib.vertices[index.vertex_index * 3 + 2]};
                    if (hasObjAttribute(attrib.normals, index.normal_index, 3))
                        v.normal = {attrib.normals[index.normal_index * 3], -attrib.normals[index.normal_index * 3 + 1], attrib.normals[index.normal_index * 3 + 2]};
                    if (hasObjAttribute(attrib.texcoords, index.texcoord_index, 2))
                        v.uv[0] = {attrib.texcoords[index.texcoord_index * 2], 1.0 - attrib.texcoords[index.texcoord_index * 2 + 1]};

                    objRaw.vertices.push_back(v);
                    objRaw.indices.push_back(static_cast<uint32_t>(objRaw.indices.size()));
                }
            }

            GLTFRaw raw;
            raw.subGltfRaws.push_back({std::move(objRaw), glm::mat4(1.0f), -1});
            raw.loaded = true;
            return raw;
        }
    }

    // Runs on a worker thread: it must not touch the engine.
    GLTFRaw importMeshFile(const std::string &path)
    {
        ZoneScoped;

        const auto extension = path.substr(path.find_last_of(".") + 1);
        if (extension == "gltf" || extension == "glb")
            return importGltf(path, extension == "glb");
        return importObj(path);
    }

    MeshHandle Engine::createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    {
        auto handle = handleManager.getNextMeshHandle();
        if (handle.index >= MAX_MESHES)
        {
            spdlog::error("Exceeded maximum number of meshes.");
            return {};
        }

        Mesh mesh{.state = MeshState::READY, .subMeshes = {SubMesh{.transform = glm::mat4(1.0f)}}};
        if (!uploadGeometry({std::move(vertices), std::move(indices)}, mesh.subMeshes[0].geometry))
            return {};

        registry.meshes.resize(handle.index + 1);
        registry.meshes[handle.index] = std::move(mesh);

        return handle;
    }

    MeshHandle Engine::loadMesh(const std::string &assetPath)
    {
        const std::string path = resolveAssetPath(assetPath);

        // A path that failed to load keeps its handle, and loading it again retries on that handle.
        MeshHandle handle;
        if (auto it = registry.loadedMeshes.find(path); it != registry.loadedMeshes.end())
        {
            if (registry.meshes[it->second.index].state != MeshState::FAILED)
                return it->second;
            handle = it->second;
        }

        // find if the model extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedMeshExtensions.find(extension) == supportedMeshExtensions.end())
        {
            spdlog::error("Unsupported mesh format: {}", extension);
            return MeshHandle{};
        }

        // A missing file is reported here, as an invalid handle. A file that does not parse fails the mesh later.
        std::error_code error;
        if (!std::filesystem::is_regular_file(pathFromUtf8(path), error))
        {
            spdlog::error("Mesh file not found: {}", path);
            return MeshHandle{};
        }

        if (!handle.isValid())
        {
            handle = handleManager.getNextMeshHandle();
            if (handle.index >= MAX_MESHES)
            {
                spdlog::warn("Exceeded maximum number of meshes.");
                return MeshHandle{};
            }
            registry.loadedMeshes[path] = handle;
            registry.meshes.resize(handle.index + 1);
        }
        registry.meshes[handle.index] = Mesh{};

        // The worker reads and decodes the file; beginFrame uploads the result (finishLoads).
        pendingMeshes.push_back({handle, path, std::async(std::launch::async, importMeshFile, path)});

        return handle;
    }

    MeshHandle Engine::loadGLTF(const std::string &path)
    {
        return loadMesh(path);
    }

    bool Engine::isLoaded(MeshHandle mesh) const
    {
        return mesh.isValid() && mesh.index < registry.meshes.size() && registry.meshes[mesh.index].state == MeshState::READY;
    }

    // Appends to the engine-wide geometry pool. Nothing is ever freed from it: meshes are not destroyed yet (D4).
    bool Engine::uploadGeometry(const ObjRaw &raw, MeshGeometry &geometry)
    {
        if (!geometryFits(raw.vertices.size(), raw.indices.size()))
            return false;

        geometry = {.firstVertex = geometryVertexCount,
                    .vertexCount = static_cast<uint32_t>(raw.vertices.size()),
                    .firstIndex = geometryIndexCount,
                    .indexCount = static_cast<uint32_t>(raw.indices.size())};

        if (!raw.vertices.empty() && !ctx.fillBuffer(geometryVertexBuffer, raw.vertices.data(), raw.vertices.size() * sizeof(Vertex), static_cast<size_t>(geometry.firstVertex) * sizeof(Vertex)))
            return false;
        if (!raw.indices.empty() && !ctx.fillBuffer(geometryIndexBuffer, raw.indices.data(), raw.indices.size() * sizeof(uint32_t), static_cast<size_t>(geometry.firstIndex) * sizeof(uint32_t)))
            return false;

        geometryVertexCount += geometry.vertexCount;
        geometryIndexCount += geometry.indexCount;
        return true;
    }

    void Engine::finishMeshLoad(PendingMeshLoad &load)
    {
        ZoneScoped;

        // The CPU copy of the geometry and the glTF model are freed when raw goes out of scope (P6).
        GLTFRaw raw = load.result.get();
        Mesh &mesh = registry.meshes[load.handle.index];
        mesh.state = MeshState::FAILED;
        if (!raw.loaded)
            return;

        // The whole mesh must fit before anything is created: nothing is freed from the pool, so a mesh failing
        // halfway would leak the geometry, materials and bindless slots of its first sub-meshes.
        size_t vertexCount = 0;
        size_t indexCount = 0;
        for (const auto &sub : raw.subGltfRaws)
        {
            vertexCount += sub.objRaw.vertices.size();
            indexCount += sub.objRaw.indices.size();
        }
        if (!geometryFits(vertexCount, indexCount))
        {
            spdlog::error("Failed to upload mesh: {}", load.path);
            return;
        }

        GltfImportCache cache;
        for (const auto &sub : raw.subGltfRaws)
        {
            SubMesh subMesh{.transform = sub.transform};
            if (raw.model)
                subMesh.material = loadMaterial(*raw.model, sub.materialIndex, cache);
            if (!uploadGeometry(sub.objRaw, subMesh.geometry))
            {
                spdlog::error("Failed to upload mesh: {}", load.path);
                mesh.subMeshes.clear();
                return;
            }
            mesh.subMeshes.push_back(subMesh);
        }

        mesh.state = MeshState::READY;
        spdlog::info("Loaded mesh {}: {} sub-meshes, geometry pool at {}/{} vertices and {}/{} indices.",
                     load.path, mesh.subMeshes.size(), geometryVertexCount, config.geometryPoolVertices, geometryIndexCount, config.geometryPoolIndices);
    }

    bool Engine::geometryFits(size_t vertexCount, size_t indexCount) const
    {
        if (vertexCount <= config.geometryPoolVertices - geometryVertexCount && indexCount <= config.geometryPoolIndices - geometryIndexCount)
            return true;
        spdlog::error("The geometry pool is full: {} vertices and {} indices do not fit ({}/{} vertices and {}/{} indices in use). "
                      "EngineConfig::geometryPoolVertices and geometryPoolIndices set its size.",
                      vertexCount, indexCount, geometryVertexCount, config.geometryPoolVertices, geometryIndexCount, config.geometryPoolIndices);
        return false;
    }

    MeshSize Engine::getMeshByteSize(const MeshHandle &handle) const
    {
        if (!handle.isValid() || handle.index >= registry.meshes.size())
        {
            spdlog::error("Mesh handle not found for getting byte size.");
            return {};
        }

        MeshSize meshSize{};
        for (const auto &subMesh : registry.meshes[handle.index].subMeshes)
        {
            meshSize.verticesByteSize += subMesh.geometry.vertexCount * sizeof(Vertex);
            meshSize.indicesByteSize += subMesh.geometry.indexCount * sizeof(uint32_t);
        }
        return meshSize;
    }
}
