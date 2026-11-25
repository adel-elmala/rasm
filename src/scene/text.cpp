#include "rasm/text.h"
#include "rasm/renderer.h"

#include "spdlog/spdlog.h"

#include <algorithm>

namespace rasm
{
    namespace
    {
        constexpr uint32_t REPLACEMENT = 0xFFFD;
    }

    const GlyphMetrics *findGlyph(const FontAtlas &font, uint32_t codepoint)
    {
        const auto byCodepoint = [](const GlyphMetrics &glyph, uint32_t value)
        { return glyph.codepoint < value; };
        auto found = std::lower_bound(font.glyphs.begin(), font.glyphs.end(), codepoint, byCodepoint);
        if (found != font.glyphs.end() && found->codepoint == codepoint)
            return &*found;
        found = std::lower_bound(font.glyphs.begin(), font.glyphs.end(), font.fallback, byCodepoint);
        return found != font.glyphs.end() && found->codepoint == font.fallback ? &*found : nullptr;
    }

    float getKerning(const FontAtlas &font, uint32_t left, uint32_t right)
    {
        const auto byPair = [](const KerningPair &pair, const KerningPair &value)
        { return pair.left < value.left || (pair.left == value.left && pair.right < value.right); };
        const KerningPair key{.left = left, .right = right};
        const auto found = std::lower_bound(font.kerning.begin(), font.kerning.end(), key, byPair);
        return found != font.kerning.end() && found->left == left && found->right == right ? found->advance : 0.0f;
    }

    uint32_t decodeUtf8(const char *text, std::vector<uint32_t> &codepoints)
    {
        if (text == nullptr)
            return 0;
        const size_t first = codepoints.size();
        const auto *bytes = reinterpret_cast<const unsigned char *>(text);
        while (*bytes != 0)
        {
            const unsigned char lead = *bytes;
            uint32_t length = 0;
            uint32_t value = 0;
            if (lead < 0x80)
            {
                length = 1;
                value = lead;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                length = 2;
                value = lead & 0x1F;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                length = 3;
                value = lead & 0x0F;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                length = 4;
                value = lead & 0x07;
            }
            bool valid = length > 0;
            for (uint32_t k = 1; valid && k < length; ++k)
            {
                valid = (bytes[k] & 0xC0) == 0x80;
                value = value << 6 | (bytes[k] & 0x3F);
            }
            // Overlong forms, surrogates and values past U+10FFFF are malformed too.
            const uint32_t smallest[5] = {0, 0, 0x80, 0x800, 0x10000};
            valid = valid && value >= smallest[length] && value <= 0x10FFFF && (value < 0xD800 || value > 0xDFFF);
            codepoints.push_back(valid ? value : REPLACEMENT);
            // A malformed sequence takes one byte, so the next one is read as a new start.
            bytes += valid ? length : 1;
        }
        return static_cast<uint32_t>(codepoints.size() - first);
    }

    bool Renderer::isLive(const FontHandle &font) const
    {
        return registry.fontHandles.isAlive(font.index, font.generation);
    }

    FontHandle Renderer::createFont(const FontAtlas &atlas)
    {
        if (atlas.width == 0 || atlas.height == 0 || atlas.pixels.size() != static_cast<size_t>(atlas.width) * atlas.height || !(atlas.distanceRange > 0.0f))
        {
            spdlog::error("createFont: the atlas '{}' is {} x {} with {} bytes and a distance range of {}; it needs a byte per texel and a range above 0.", atlas.name, atlas.width, atlas.height,
                          atlas.pixels.size(), atlas.distanceRange);
            return {};
        }
        const HandleSlot slot = registry.fontHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::error("createFont: {} fonts are alive, MAX_FONTS.", registry.fontHandles.liveCount());
            return {};
        }
        const TextureHandle texture = createTexture({.name = atlas.name.empty() ? "Font Atlas" : atlas.name, .width = atlas.width, .height = atlas.height, .format = Format::R8_UNORM, .usage = TextureUsage::SAMPLED});
        if (!texture.isValid() || !ctx.fillTexture(texture, atlas.pixels.data()))
        {
            if (texture.isValid())
                destroyTexture(texture);
            registry.fontHandles.release(slot.index, slot.generation);
            return {};
        }
        // Its own slot, clamped, so a glyph at the atlas's edge does not read the other side.
        addBindlessTexture(bindlessDescriptorSet, texture, SamplerType::LINEAR_CLAMP);
        const FontHandle handle{slot.index, slot.generation};
        registry.fonts.resize(registry.fontHandles.size());
        registry.fonts[handle.index] = {.handle = handle, .atlas = texture, .distanceRange = atlas.distanceRange};
        return handle;
    }

    void Renderer::destroyFont(FontHandle font)
    {
        if (!isLive(font))
        {
            spdlog::error("destroyFont: font handle {{{}, {}}} is invalid or was destroyed.", font.index, font.generation);
            return;
        }
        for (size_t i = 1; i < registry.materials.size(); ++i)
        {
            Material &material = registry.materials[i];
            if (!isLive(material.handle) || !(material.vector.font == font))
                continue;
            material.vector.font = {};
            writeMaterialRow(material.handle);
        }
        destroyTexture(registry.fonts[font.index].atlas);
        registry.fonts[font.index] = {};
        registry.fontHandles.release(font.index, font.generation);
    }
}
