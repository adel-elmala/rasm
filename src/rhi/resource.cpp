#include "rasm/render/resource.h"

#include <string_view>

namespace rasm
{
    namespace
    {
        std::size_t hashEnum(uint32_t value)
        {
            return std::hash<uint32_t>()(value);
        }
    }

    std::size_t ShaderDescHash::operator()(const ShaderDesc &desc) const
    {
        std::string_view code(reinterpret_cast<const char *>(desc.code.data()), desc.code.size());
        std::size_t hash = std::hash<std::string>()(desc.name);
        hash = hashCombine(hash, hashEnum(static_cast<uint32_t>(desc.type)));
        hash = hashCombine(hash, std::hash<std::string>()(desc.entryPoint));
        return hashCombine(hash, std::hash<std::string_view>()(code));
    }

    std::size_t PipelineDescHash::operator()(const PipelineDesc &desc) const
    {
        std::size_t hash = std::hash<std::string>()(desc.name);
        hash = hashCombine(hash, HandleHash()(desc.vertexShader));
        hash = hashCombine(hash, HandleHash()(desc.fragmentShader));
        hash = hashCombine(hash, HandleHash()(desc.computeShader));
        hash = hashCombine(hash, HandleHash()(desc.descriptorSetLayout));
        hash = hashCombine(hash, hashEnum(static_cast<uint32_t>(desc.topology)));
        for (const Format format : desc.colorAttachmentFormats)
            hash = hashCombine(hash, hashEnum(static_cast<uint32_t>(format)));
        return hashCombine(hash, hashEnum(static_cast<uint32_t>(desc.depthStencilAttachmentFormat)));
    }

    // The list ends at the first UNKNOWN, so formats after a gap are not attachments.
    uint32_t colorAttachmentCount(const Format *formats)
    {
        uint32_t count = 0;
        while (count < MAX_COLOR_ATTACHMENTS && formats[count] != Format::UNKNOWN)
            ++count;
        return count;
    }

    std::size_t BindlessDescriptorSetLayoutDescHash::operator()(const BindlessDescriptorSetLayoutDesc &desc) const
    {
        std::size_t hash = std::hash<std::string>()(desc.name);
        hash = hashCombine(hash, hashEnum(static_cast<uint32_t>(desc.stage)));
        hash = hashCombine(hash, std::hash<uint32_t>()(desc.count));
        return hashCombine(hash, hashEnum(static_cast<uint32_t>(desc.type)));
    }
}
