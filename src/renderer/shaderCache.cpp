#include "renderer/shaderCache.h"

#include "tracy/Tracy.hpp"

#include <cstring>
#include <system_error>

namespace rasm
{
    namespace
    {
        constexpr uint32_t SHADER_CACHE_MAGIC = 0x53534152; // "RASS"

        struct Writer
        {
            std::vector<uint8_t> bytes;

            void putNumber(uint32_t value)
            {
                const auto *data = reinterpret_cast<const uint8_t *>(&value);
                bytes.insert(bytes.end(), data, data + sizeof(value));
            }

            void putString(const std::string &text)
            {
                putNumber(static_cast<uint32_t>(text.size()));
                bytes.insert(bytes.end(), text.begin(), text.end());
            }
        };

        // Every read checks the bytes left, so a damaged payload fails instead of reading past its end.
        struct Reader
        {
            const uint8_t *data = nullptr;
            size_t size = 0;
            size_t position = 0;
            bool failed = false;

            uint32_t getNumber()
            {
                uint32_t value = 0;
                if (size - position < sizeof(value))
                {
                    failed = true;
                    return 0;
                }
                std::memcpy(&value, data + position, sizeof(value));
                position += sizeof(value);
                return value;
            }

            std::string getString()
            {
                const uint32_t length = getNumber();
                if (failed || size - position < length)
                {
                    failed = true;
                    return {};
                }
                std::string text(reinterpret_cast<const char *>(data + position), length);
                position += length;
                return text;
            }

            // A count of items of at least minimumBytes each, so a damaged count cannot make a huge vector.
            uint32_t getCount(size_t minimumBytes)
            {
                const uint32_t count = getNumber();
                if (!failed && count > (size - position) / minimumBytes)
                    failed = true;
                return failed ? 0 : count;
            }
        };
    }

    bool readCachedShader(const std::filesystem::path &path, const CacheKey &key, CachedShader &shader)
    {
        ZoneScoped;

        std::vector<uint8_t> payload;
        if (readCacheFile(path, SHADER_CACHE_MAGIC, SHADER_CACHE_VERSION, key, payload) != CacheFileStatus::OK)
            return false;

        Reader reader = {.data = payload.data(), .size = payload.size()};
        const uint32_t codeSize = reader.getCount(1);
        shader.code.assign(payload.begin() + static_cast<std::ptrdiff_t>(reader.position), payload.begin() + static_cast<std::ptrdiff_t>(reader.position + codeSize));
        reader.position += codeSize;
        shader.pushConstants = reader.getString();
        shader.layouts.clear();
        const uint32_t structCount = reader.getCount(16);
        for (uint32_t i = 0; i < structCount && !reader.failed; ++i)
        {
            const std::string name = reader.getString();
            StructLayout &layout = shader.layouts[name];
            layout.size = reader.getNumber();
            layout.members.resize(reader.getCount(12));
            for (StructMember &member : layout.members)
            {
                member.name = reader.getString();
                member.offset = reader.getNumber();
                member.structName = reader.getString();
            }
            layout.fields.resize(reader.getCount(28));
            for (StructField &field : layout.fields)
            {
                field.name = reader.getString();
                field.typeName = reader.getString();
                field.offset = reader.getNumber();
                field.size = reader.getNumber();
                field.type = static_cast<FieldType>(reader.getNumber());
                field.count = reader.getNumber();
                field.columns = reader.getNumber();
            }
        }
        if (reader.failed || reader.position != payload.size() || shader.code.empty())
            return false;

        // The folder keeps the files used most recently, and this one was just used.
        std::error_code error;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
        return true;
    }

    bool writeCachedShader(const std::filesystem::path &path, const CacheKey &key, const CachedShader &shader)
    {
        Writer writer;
        writer.putNumber(static_cast<uint32_t>(shader.code.size()));
        writer.bytes.insert(writer.bytes.end(), shader.code.begin(), shader.code.end());
        writer.putString(shader.pushConstants);
        writer.putNumber(static_cast<uint32_t>(shader.layouts.size()));
        for (const auto &[name, layout] : shader.layouts)
        {
            writer.putString(name);
            writer.putNumber(layout.size);
            writer.putNumber(static_cast<uint32_t>(layout.members.size()));
            for (const StructMember &member : layout.members)
            {
                writer.putString(member.name);
                writer.putNumber(member.offset);
                writer.putString(member.structName);
            }
            writer.putNumber(static_cast<uint32_t>(layout.fields.size()));
            for (const StructField &field : layout.fields)
            {
                writer.putString(field.name);
                writer.putString(field.typeName);
                writer.putNumber(field.offset);
                writer.putNumber(field.size);
                writer.putNumber(static_cast<uint32_t>(field.type));
                writer.putNumber(field.count);
                writer.putNumber(field.columns);
            }
        }
        return writeCacheFile(path, SHADER_CACHE_MAGIC, SHADER_CACHE_VERSION, key, writer.bytes.data(), writer.bytes.size());
    }
}
