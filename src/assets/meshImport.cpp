#include "assets/meshImport.h"
#include "assets/gltfAccessor.h"
#include "assets/animationImport.h"
#include "assets/skinImport.h"
#include "assets/morphImport.h"
#include "assets/ktxImage.h"
#include "platform/paths.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "tiny_gltf.h"
#include "tiny_obj_loader.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace rasm
{
    namespace
    {
        struct PendingNode
        {
            int index;
            int parent;
        };

        // The glTF extensions a file may require: the others change what the file means, so it fails to load.
        const char *const SUPPORTED_EXTENSIONS[] = {"KHR_materials_unlit", "KHR_materials_emissive_strength", "KHR_texture_transform", "KHR_mesh_quantization", "KHR_texture_basisu"};

        // A texture a material samples, and what its slot reads.
        struct TextureUse
        {
            int texture = -1;
            KtxSlot slot = KtxSlot::DATA;
        };
    }

    namespace
    {
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
                glm::quat q(static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]));
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

        std::vector<SubGLTFRaw> extractGLTFRaws(const tinygltf::Model &model, uint32_t meshIndex, int nodeIndex, const glm::mat4 &globalTransform)
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

                // COLOR_0 (OPTIONAL, VEC3 or VEC4, float or normalized), linear. It goes into its own stream: a mesh
                // without it gets none.
                std::vector<float> colorValues;
                uint32_t colorComponents = 0;
                const bool hasColors = readVertexAttribute(model, primitive, "COLOR_0", vertexCount, 3, colorValues, colorComponents, primitiveName);
                std::vector<uint32_t> colors;
                if (hasColors)
                {
                    colors.resize(vertexCount);
                    for (size_t i = 0; i < vertexCount; ++i)
                    {
                        const float *c = &colorValues[i * colorComponents];
                        colors[i] = packVertexColor(glm::vec4(c[0], c[1], c[2], colorComponents >= 4 ? c[3] : 1.0f));
                    }
                }

                // JOINTS_0 / WEIGHTS_0 of a node with a skin, into the skin stream. A skinned primitive ignores its
                // node's matrix (glTF): its joints place it.
                std::vector<SkinVertex> skin;
                const int skinIndex = getNodeSkin(model, nodeIndex);
                if (skinIndex >= 0)
                    readSkinVertices(model, primitive, vertexCount, static_cast<uint32_t>(model.skins[skinIndex].joints.size()), primitiveName, skin);

                // The morph targets' deltas, into the morph stream.
                std::vector<MorphDelta> morphs;
                const uint32_t morphTargetCount = readMorphTargets(model, primitive, vertexCount, primitiveName, morphs);

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
                SubGLTFRaw sub{{.vertices = std::move(vertices), .indices = std::move(indices), .colors = std::move(colors), .morphs = std::move(morphs), .morphTargetCount = morphTargetCount},
                               globalTransform,
                               primitive.material,
                               nodeIndex};
                if (!skin.empty())
                {
                    sub.transform = glm::mat4(1.0f);
                    sub.skin = skinIndex;
                    sub.jointSpheres = getJointSpheres(sub.objRaw.vertices, skin, static_cast<uint32_t>(model.skins[skinIndex].joints.size()));
                    sub.objRaw.skin = std::move(skin);
                }
                subGltfs.push_back(std::move(sub));
            }

            return subGltfs;
        }

        // Depth-first walk of the node tree. glTF requires a strict tree, but nothing enforces it: an explicit stack
        // survives arbitrarily deep files, and a node that already has a parent is not visited again, which stops cycles.
        // The walk keeps each visited node's parent and matrices, which the animation import needs.
        std::vector<SubGLTFRaw> traverseNodes(const tinygltf::Model &model, const std::vector<int> &rootNodes, GltfNodeWalk &walk)
        {
            std::vector<SubGLTFRaw> subGltfs;
            walk.parents.assign(model.nodes.size(), GLTF_NODE_NOT_VISITED);
            walk.locals.assign(model.nodes.size(), glm::mat4(1.0f));
            walk.globals.assign(model.nodes.size(), glm::mat4(1.0f));

            // Pushed in reverse so nodes pop in file order.
            std::vector<PendingNode> pending;
            for (auto root = rootNodes.rbegin(); root != rootNodes.rend(); ++root)
                pending.push_back({*root, -1});

            while (!pending.empty())
            {
                const PendingNode current = pending.back();
                pending.pop_back();

                if (current.index < 0 || static_cast<size_t>(current.index) >= model.nodes.size())
                {
                    spdlog::warn("glTF: skipping out-of-range node {} ({} nodes).", current.index, model.nodes.size());
                    continue;
                }
                if (walk.parents[current.index] != GLTF_NODE_NOT_VISITED)
                {
                    spdlog::warn("glTF: skipping node {}, it has more than one parent or is part of a cycle.", current.index);
                    continue;
                }
                walk.parents[current.index] = current.parent;
                walk.order.push_back(current.index);

                const tinygltf::Node &node = model.nodes[current.index];

                // Compute this node's local matrix, then combine with its parent's world matrix
                walk.locals[current.index] = getLocalTransform(node);
                const glm::mat4 globalTransform = (current.parent < 0 ? glm::mat4(1.0f) : walk.globals[current.parent]) * walk.locals[current.index];
                walk.globals[current.index] = globalTransform;

                // If this node instantiates a mesh, we now have its world transform!
                if (node.mesh >= 0)
                {
                    auto extractedRaws = extractGLTFRaws(model, node.mesh, current.index, globalTransform);
                    subGltfs.insert(subGltfs.end(), std::make_move_iterator(extractedRaws.begin()), std::make_move_iterator(extractedRaws.end()));
                }

                for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
                    pending.push_back({*child, current.index});
            }

            return subGltfs;
        }

        // Entry point: Start traversal from the root nodes of the active scene
        std::vector<SubGLTFRaw> processGLTF(const tinygltf::Model &model, GltfNodeWalk &walk)
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

            // glTF is right-handed Y-up like the engine, so the hierarchy needs no root transform (D8).
            return traverseNodes(model, scene.nodes, walk);
        }

        // KTX2 bytes stay as they are, for transcodeKtxImages, which knows the slots that use each image. stb decodes the
        // other images, as tinygltf does by default.
        bool loadImage(tinygltf::Image *image, int imageIndex, std::string *err, std::string *warn, int width, int height, const unsigned char *bytes, int size, void *userData)
        {
            if (!isKtx2(bytes, static_cast<size_t>(size)))
                return tinygltf::LoadImageData(image, imageIndex, err, warn, width, height, bytes, size, userData);
            image->image.assign(bytes, bytes + size);
            image->as_is = true;
            image->width = image->height = image->component = image->bits = image->pixel_type = -1;
            return true;
        }

        // The slots loadMaterial fills, with the same KtxSlot it passes. Unlit reads base color only.
        uint32_t getTextureUses(const tinygltf::Material &material, TextureUse *uses)
        {
            uint32_t count = 0;
            uses[count++] = {material.pbrMetallicRoughness.baseColorTexture.index, KtxSlot::COLOR};
            if (material.extensions.count("KHR_materials_unlit") != 0)
                return count;
            uses[count++] = {material.pbrMetallicRoughness.metallicRoughnessTexture.index, KtxSlot::DATA};
            uses[count++] = {material.normalTexture.index, KtxSlot::NORMAL};
            uses[count++] = {material.occlusionTexture.index, KtxSlot::OCCLUSION};
            uses[count++] = {material.emissiveTexture.index, KtxSlot::COLOR};
            return count;
        }

        // Each KTX2 image gets the best format the device has for each kind of slot that uses it, and is transcoded once
        // per format: an ORM image read as occlusion and as metallic-roughness is one BC7 texture. Then its file bytes
        // go, like the glTF buffers (P6).
        void transcodeKtxImages(tinygltf::Model &model, uint64_t supportedTextureFormats, GLTFRaw &raw)
        {
            ZoneScoped;

            for (const tinygltf::Material &material : model.materials)
            {
                TextureUse uses[5];
                const uint32_t useCount = getTextureUses(material, uses);
                for (uint32_t i = 0; i < useCount; ++i)
                {
                    const int source = getBasisuSource(model, uses[i].texture);
                    if (source < 0)
                        continue;
                    const uint64_t slotKey = getKtxSlotKey(source, uses[i].slot);
                    const tinygltf::Image &image = model.images[source];
                    if (raw.ktxFormats.contains(slotKey) || !image.as_is)
                        continue;

                    const std::string name = image.uri.empty() ? "glTF image " + std::to_string(source) : image.uri;
                    KtxInfo info;
                    std::string error;
                    if (!readKtxInfo(image.image.data(), image.image.size(), info, error))
                    {
                        spdlog::error("Failed to read KTX2 image {}: {}", name, error);
                        continue;
                    }
                    const Format format = chooseKtxFormat(info, uses[i].slot, supportedTextureFormats);
                    if (format == Format::UNKNOWN)
                    {
                        spdlog::error("KTX2 image {} is in a format the device can't sample.", name);
                        continue;
                    }
                    const uint64_t imageKey = getKtxImageKey(source, format);
                    if (!raw.ktxImages.contains(imageKey))
                    {
                        warnAboutKtxFormat(name, info, format);
                        TextureRaw texture;
                        if (!decodeKtx(image.image.data(), image.image.size(), format, texture, error))
                        {
                            spdlog::error("Failed to decode KTX2 image {}: {}", name, error);
                            continue;
                        }
                        raw.ktxImages[imageKey] = std::move(texture);
                    }
                    raw.ktxFormats[slotKey] = format;
                }
            }

            for (tinygltf::Image &image : model.images)
            {
                if (image.as_is)
                    std::vector<unsigned char>().swap(image.image);
            }
        }

        GLTFRaw importGltf(const std::string &path, bool binary, uint64_t supportedTextureFormats)
        {
            auto model = std::make_shared<tinygltf::Model>();
            tinygltf::TinyGLTF loader;
            loader.SetImageLoader(loadImage, nullptr);

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

            bool supported = true;
            for (const std::string &extension : model->extensionsRequired)
            {
                if (std::find(std::begin(SUPPORTED_EXTENSIONS), std::end(SUPPORTED_EXTENSIONS), extension) == std::end(SUPPORTED_EXTENSIONS))
                {
                    spdlog::error("glTF {} requires the extension {}, which RASM does not support.", path, extension);
                    supported = false;
                }
            }
            if (!supported)
                return {};
            for (const std::string &extension : model->extensionsUsed)
            {
                if (std::find(std::begin(SUPPORTED_EXTENSIONS), std::end(SUPPORTED_EXTENSIONS), extension) == std::end(SUPPORTED_EXTENSIONS))
                    spdlog::debug("glTF {} uses the extension {}, which RASM ignores.", path, extension);
            }

            GLTFRaw raw;
            GltfNodeWalk walk;
            raw.subGltfRaws = processGLTF(*model, walk);
            raw.animation = importAnimations(*model, walk, raw.subGltfRaws);
            // A primitive whose skin did not import draws its bind pose and needs no skin stream. One whose morph targets
            // have no weights, or that nothing places as it is posed, draws its base shape.
            for (SubGLTFRaw &sub : raw.subGltfRaws)
            {
                if (sub.jointCount == 0)
                {
                    sub.objRaw.skin.clear();
                    sub.jointSpheres.clear();
                }
                if (sub.firstWeight < 0 || (sub.jointCount == 0 && sub.animatedNode < 0))
                {
                    sub.objRaw.morphs.clear();
                    sub.objRaw.morphTargetCount = 0;
                    sub.firstWeight = -1;
                }
            }
            // Every accessor has been read. Materials need only the decoded images, so the raw buffers go now (P6).
            model->buffers.clear();
            transcodeKtxImages(*model, supportedTextureFormats, raw);
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

                    const size_t p = 3 * static_cast<size_t>(index.vertex_index);
                    v.pos = {attrib.vertices[p], attrib.vertices[p + 1], attrib.vertices[p + 2]};
                    if (hasObjAttribute(attrib.normals, index.normal_index, 3))
                    {
                        const size_t n = 3 * static_cast<size_t>(index.normal_index);
                        v.normal = {attrib.normals[n], attrib.normals[n + 1], attrib.normals[n + 2]};
                    }
                    // OBJ puts the UV origin at the image's bottom-left; textures start at the top-left.
                    if (hasObjAttribute(attrib.texcoords, index.texcoord_index, 2))
                    {
                        const size_t t = 2 * static_cast<size_t>(index.texcoord_index);
                        v.uv[0] = {attrib.texcoords[t], 1.0f - attrib.texcoords[t + 1]};
                    }

                    objRaw.vertices.push_back(v);
                    objRaw.indices.push_back(static_cast<uint32_t>(objRaw.indices.size()));
                }
            }

            GLTFRaw raw;
            raw.subGltfRaws.push_back({std::move(objRaw), glm::mat4(1.0f), -1, -1});
            raw.loaded = true;
            return raw;
        }
    }

    // Runs on a worker thread: it must not touch the engine.
    GLTFRaw importMeshFile(const std::string &path, uint64_t supportedTextureFormats)
    {
        ZoneScoped;

        const auto extension = path.substr(path.find_last_of('.') + 1);
        if (extension == "gltf" || extension == "glb")
            return importGltf(path, extension == "glb", supportedTextureFormats);
        return importObj(path);
    }

    uint64_t getKtxSlotKey(int image, KtxSlot slot)
    {
        return (static_cast<uint64_t>(image) << 3) | static_cast<uint64_t>(slot);
    }

    uint64_t getKtxImageKey(int image, Format format)
    {
        return (static_cast<uint64_t>(image) << 8) | static_cast<uint64_t>(format);
    }

    int getBasisuSource(const tinygltf::Model &model, int textureIndex)
    {
        if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
            return -1;
        const auto it = model.textures[textureIndex].extensions.find("KHR_texture_basisu");
        if (it == model.textures[textureIndex].extensions.end() || !it->second.Get("source").IsInt())
            return -1;
        const int source = it->second.Get("source").GetNumberAsInt();
        return source >= 0 && source < static_cast<int>(model.images.size()) ? source : -1;
    }
}
