#include "rasm/assetLoader.h"
#include "rasm/text.h"
#include "platform/paths.h"

#include "spdlog/spdlog.h"
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace rasm
{
    namespace
    {
        // The largest em and atlas side buildFontAtlas makes: 4096 is a 2D image size every device supports.
        constexpr float MAX_FONT_EM_SIZE = 256.0f;
        constexpr uint32_t MAX_FONT_ATLAS_SIZE = 4096;

        // One glyph's distance field before packing.
        struct GlyphImage
        {
            GlyphMetrics metrics;
            std::vector<uint8_t> pixels;
            int width = 0;
            int height = 0;
            int x = 0; // its place in the atlas
            int y = 0;
        };
    }

    namespace
    {
        // A row of glyphs after another, each row as tall as its tallest glyph, in an atlas of the given width: its height,
        // a power of two, or 0 when a glyph is wider than the atlas.
        uint32_t packRows(std::vector<GlyphImage> &images, uint32_t width)
        {
            std::vector<GlyphImage *> byHeight;
            for (GlyphImage &image : images)
                if (image.width > 0)
                    byHeight.push_back(&image);
            std::sort(byHeight.begin(), byHeight.end(), [](const GlyphImage *a, const GlyphImage *b)
                      { return a->height > b->height || (a->height == b->height && a->metrics.codepoint < b->metrics.codepoint); });
            int x = 0;
            int y = 0;
            int rowHeight = 0;
            for (GlyphImage *image : byHeight)
            {
                if (image->width > static_cast<int>(width))
                    return 0;
                if (x + image->width > static_cast<int>(width))
                {
                    x = 0;
                    y += rowHeight + 1;
                    rowHeight = 0;
                }
                image->x = x;
                image->y = y;
                x += image->width + 1;
                rowHeight = std::max(rowHeight, image->height);
            }
            uint32_t height = 1;
            while (height < static_cast<uint32_t>(y + rowHeight))
                height *= 2;
            return height;
        }
    }

    bool buildFontAtlas(const uint8_t *data, size_t size, const FontLoadOptions &options, FontAtlas &atlas)
    {
        stbtt_fontinfo info;
        // A font starts with a table directory of 12 bytes; the offset is -1 when its tag is no font's.
        const int offset = data != nullptr && size >= 12 ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
        if (offset < 0 || !stbtt_InitFont(&info, data, offset))
        {
            spdlog::error("buildFontAtlas: the data is not a TrueType or OpenType font.");
            return false;
        }
        if (!(options.emSize >= 8.0f && options.emSize <= MAX_FONT_EM_SIZE) || !(options.distanceRange >= 1.0f && options.distanceRange <= options.emSize))
        {
            spdlog::error("buildFontAtlas: emSize {} must be 8 to {} and distanceRange {} 1 to emSize.", options.emSize, MAX_FONT_EM_SIZE, options.distanceRange);
            return false;
        }
        const float scale = stbtt_ScaleForMappingEmToPixels(&info, options.emSize);
        const float toEm = scale / options.emSize;
        int ascent = 0;
        int descent = 0;
        int lineGap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        atlas = FontAtlas{.name = "Font Atlas",
                          .distanceRange = options.distanceRange,
                          .ascent = static_cast<float>(ascent) * toEm,
                          .descent = static_cast<float>(-descent) * toEm,
                          .lineHeight = static_cast<float>(ascent - descent + lineGap) * toEm};

        // Each codepoint's distance field: stb_truetype's SDF, 128 on the edge and 255 / distanceRange per pixel, so a
        // texel holds 0.5 + distance / distanceRange; padding for half the range and the halo around it.
        const int padding = static_cast<int>(std::ceil(0.5f * options.distanceRange)) + 1;
        const float pixelDistance = 255.0f / options.distanceRange;
        std::vector<uint32_t> codepoints(options.codepoints, options.codepoints + options.codepointCount);
        if (codepoints.empty())
        {
            for (uint32_t c = 32; c < 127; ++c)
                codepoints.push_back(c);
            for (uint32_t c = 160; c < 256; ++c)
                codepoints.push_back(c);
        }
        codepoints.push_back(atlas.fallback);
        std::sort(codepoints.begin(), codepoints.end());
        codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
        std::vector<GlyphImage> images;
        std::vector<int> glyphIndices;
        for (uint32_t codepoint : codepoints)
        {
            const int glyph = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
            if (glyph == 0)
                continue;
            int advance = 0;
            int bearing = 0;
            stbtt_GetGlyphHMetrics(&info, glyph, &advance, &bearing);
            GlyphImage image;
            image.metrics.codepoint = codepoint;
            image.metrics.advance = static_cast<float>(advance) * toEm;
            int xOffset = 0;
            int yOffset = 0;
            unsigned char *field = stbtt_GetGlyphSDF(&info, scale, glyph, padding, 128, pixelDistance, &image.width, &image.height, &xOffset, &yOffset);
            if (field != nullptr)
            {
                image.pixels.assign(field, field + static_cast<size_t>(image.width) * image.height);
                stbtt_FreeSDF(field, nullptr);
                // The bitmap's top-left is (xOffset, yOffset) from the pen, y down: its bottom-left in ems, y up.
                image.metrics.offset = glm::vec2(static_cast<float>(xOffset), -static_cast<float>(yOffset + image.height)) / options.emSize;
                image.metrics.size = glm::vec2(static_cast<float>(image.width), static_cast<float>(image.height)) / options.emSize;
            }
            else
            {
                image.width = 0;
                image.height = 0;
            }
            images.push_back(std::move(image));
            glyphIndices.push_back(glyph);
        }
        if (images.empty())
        {
            spdlog::error("buildFontAtlas: the font has none of the {} codepoints asked for.", codepoints.size());
            return false;
        }

        // The narrowest atlas, from 512 texels, whose height stays within MAX_FONT_ATLAS_SIZE.
        atlas.width = options.emSize > 64.0f ? 1024 : 512;
        atlas.height = packRows(images, atlas.width);
        while ((atlas.height == 0 || atlas.height > MAX_FONT_ATLAS_SIZE) && atlas.width < MAX_FONT_ATLAS_SIZE)
        {
            atlas.width *= 2;
            atlas.height = packRows(images, atlas.width);
        }
        if (atlas.height == 0 || atlas.height > MAX_FONT_ATLAS_SIZE)
        {
            spdlog::error("buildFontAtlas: {} glyphs at {} texels per em do not fit an atlas of {} x {}.", images.size(), options.emSize, MAX_FONT_ATLAS_SIZE, MAX_FONT_ATLAS_SIZE);
            return false;
        }
        atlas.pixels.assign(static_cast<size_t>(atlas.width) * atlas.height, 0);
        for (GlyphImage &image : images)
        {
            for (int row = 0; row < image.height; ++row)
                std::copy_n(image.pixels.data() + static_cast<size_t>(row) * image.width, image.width, atlas.pixels.data() + static_cast<size_t>(image.y + row) * atlas.width + image.x);
            if (image.width > 0)
                image.metrics.uv = glm::vec4(static_cast<float>(image.x) / static_cast<float>(atlas.width), static_cast<float>(image.y) / static_cast<float>(atlas.height),
                                             static_cast<float>(image.x + image.width) / static_cast<float>(atlas.width),
                                             static_cast<float>(image.y + image.height) / static_cast<float>(atlas.height));
            atlas.glyphs.push_back(image.metrics);
        }
        // Kerning between every pair the atlas holds (the font's kern table or GPOS pairs): a lookup per pair, so a set of
        // thousands of codepoints takes seconds here.
        for (size_t a = 0; a < images.size(); ++a)
        {
            for (size_t b = 0; b < images.size(); ++b)
            {
                const int kern = stbtt_GetGlyphKernAdvance(&info, glyphIndices[a], glyphIndices[b]);
                if (kern != 0)
                    atlas.kerning.push_back({.left = images[a].metrics.codepoint, .right = images[b].metrics.codepoint, .advance = static_cast<float>(kern) * toEm});
            }
        }
        // images is in codepoint order, so the glyphs and the pairs are sorted.
        return true;
    }

    bool AssetLoader::loadFont(const std::string &path, FontAtlas &atlas, const FontLoadOptions &options)
    {
        const std::string resolved = resolveAssetPath(path);
        // Binary: a text-mode read would turn the font's CR LF bytes into LF.
        std::ifstream file(pathFromUtf8(resolved), std::ios::binary);
        const std::string data = file ? std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()) : std::string();
        if (data.empty())
        {
            spdlog::error("loadFont: '{}' could not be read.", resolved);
            return false;
        }
        if (!buildFontAtlas(reinterpret_cast<const uint8_t *>(data.data()), data.size(), options, atlas))
            return false;
        atlas.name = path;
        return true;
    }
}
