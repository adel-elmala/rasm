#include "importMeshText.h"

#include "platform/paths.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

// Writes `contents` to a temp file and imports it. The random name keeps test processes that run at once apart.
rasm::GLTFRaw importMeshText(const std::string &contents, const std::string &extension, uint64_t supportedTextureFormats)
{
    const std::string name = "rasm_tests_" + std::to_string(std::random_device{}()) + extension;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    {
        std::ofstream file(path);
        file << contents;
    }

    rasm::GLTFRaw raw;
    try
    {
        raw = rasm::importMeshFile(rasm::pathToUtf8(path), supportedTextureFormats);
    }
    catch (...)
    {
        std::filesystem::remove(path);
        throw;
    }
    std::filesystem::remove(path);
    return raw;
}

// Appends `size` bytes to the buffer, 4-byte aligned, and returns the glTF bufferView that covers them.
std::string addView(std::vector<unsigned char> &buffer, const void *data, size_t size)
{
    buffer.resize((buffer.size() + 3) / 4 * 4, 0);
    const size_t offset = buffer.size();
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    buffer.insert(buffer.end(), bytes, bytes + size);
    return "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(size) + "}";
}

std::string base64(const std::vector<unsigned char> &bytes)
{
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t b0 = bytes[i];
        const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const uint32_t triple = b0 << 16 | b1 << 8 | b2;
        out += alphabet[triple >> 18 & 63];
        out += alphabet[triple >> 12 & 63];
        out += i + 1 < bytes.size() ? alphabet[triple >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[triple & 63] : '=';
    }
    return out;
}

// `rest` holds the accessors, meshes, nodes and scenes; the buffer is embedded as a data URI.
rasm::GLTFRaw importGltf(const std::vector<unsigned char> &buffer, const std::string &views, const std::string &rest)
{
    return importMeshText("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(buffer.size()) +
                              ",\"uri\":\"data:application/octet-stream;base64," + base64(buffer) + "\"}],\"bufferViews\":[" + views + "]," + rest + "}",
                          ".gltf");
}
