// clang-format off
#pragma once

#include "rasm/handle.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rasm
{

    // Fonts a renderer holds at once (Renderer::createFont).
    constexpr uint32_t          MAX_FONTS               = 64;

    // A font the renderer holds: its atlas texture and how it stores distances.
    struct Font
    {
        FontHandle                  handle;
        TextureHandle               atlas;
        float                       distanceRange   = 0.0f;
    };

    // Label slots of the renderer: the labels of every entity alive at once (each label keeps its visibility per view).
    constexpr uint32_t          MAX_LABELS              = 1u << 16;
    // Ems a label's text may reach from its anchor: culling pads a label draw's sphere by this times its text size, so
    // text farther out can vanish with its draw at the screen's edge. Must match shaders/lib/vector.slang.
    constexpr float             MAX_LABEL_REACH         = 12.0f;

    // One glyph of a font atlas, in ems (1 = the font's size): the quad's corner from the pen, +y up, and its size; the
    // pen moves by advance; uv is the glyph's rect in the atlas (u0, v0, u1, v1).
    struct GlyphMetrics
    {
        uint32_t                    codepoint       = 0;
        float                       advance         = 0.0f;
        glm::vec2                   offset          = glm::vec2(0.0f); // bottom-left corner of the quad, from the pen on the baseline
        glm::vec2                   size            = glm::vec2(0.0f);
        glm::vec4                   uv              = glm::vec4(0.0f);
    };

    // The advance between two codepoints added to the left one's, in ems.
    struct KerningPair
    {
        uint32_t                    left            = 0;
        uint32_t                    right           = 0;
        float                       advance         = 0.0f;
    };

    // A font as a signed distance field atlas, on the CPU: the texture Renderer::createFont uploads and the metrics the
    // label builder lays text out with. A texel holds 0.5 + distance / distanceRange (atlas pixels, inside positive), so
    // a multi-channel atlas (MSDF) could use the same contract. AssetLoader::loadFont builds one from a TrueType file;
    // an application can bring its own.
    struct FontAtlas
    {
        std::string                 name;
        std::vector<uint8_t>        pixels;                             // width x height, one byte per texel
        uint32_t                    width           = 0;
        uint32_t                    height          = 0;
        float                       distanceRange   = 16.0f;            // atlas pixels the stored distance spans, from 0 to 1
        float                       ascent          = 0.8f;             // ems above the baseline
        float                       descent         = 0.2f;             // ems below it
        float                       lineHeight      = 1.2f;             // ems from one baseline to the next
        std::vector<GlyphMetrics>   glyphs;                             // sorted by codepoint
        std::vector<KerningPair>    kerning;                            // sorted by left, then right
        uint32_t                    fallback        = '?';              // drawn for a codepoint the atlas lacks
    };

    // The glyph of a codepoint (binary search), its fallback's, or null.
    const GlyphMetrics*             findGlyph(const FontAtlas& font, uint32_t codepoint);
    float                           getKerning(const FontAtlas& font, uint32_t left, uint32_t right);
    // The codepoints of UTF-8 text, appended; a malformed byte becomes U+FFFD. Returns how many.
    uint32_t                        decodeUtf8(const char* text, std::vector<uint32_t>& codepoints);

}
// clang-format on
