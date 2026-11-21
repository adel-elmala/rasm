#include "assets/gltfAccessor.h"

#include "tiny_gltf.h"

#include <cstring>
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
    }

    namespace
    {
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
}
