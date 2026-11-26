// clang-format off
#pragma once

#include "rasm/mesh.h"
#include "rasm/terrain.h"
#include "rasm/text.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <vector>

namespace rasm
{

    // The longest a miter join may reach, in half widths; a sharper turn is beveled. Culling pads by it.
    constexpr float             MAX_VECTOR_MITER        = 2.0f;

    enum class LineJoin : uint32_t
    {
        MITER,          // the edges meet in a point (up to the miter limit, then BEVEL)
        BEVEL,          // the corner cut straight
        ROUND,
    };

    enum class LineCap : uint32_t
    {
        BUTT,           // the line ends at its end point
        SQUARE,         // half a width past it
        ROUND,
    };

    struct LineShape
    {
        LineJoin                    join            = LineJoin::ROUND;
        LineCap                     cap             = LineCap::BUTT;
        float                       miterLimit      = MAX_VECTOR_MITER; // half widths, 1 to MAX_VECTOR_MITER
        float                       drapeWidth      = 0.0f;             // metres: a draped line's whole width, built into its mesh
    };

    // Where features sit. Without a drape, at the heights of their points, on a ground whose up is up. Draped, on a
    // terrain tile: their points are placed across the tile (their own height is ignored), cut on its grid, and lifted
    // by the rule the shader draws (sampleTerrainHeight), so they lie exactly on the drawn ground. Draped features
    // must lie within the tile; make them content of the same tile as its terrain, so both are drawn together.
    struct VectorPlacement
    {
        glm::vec3                   up              = glm::vec3(0.0f, 1.0f, 0.0f); // world space
        const TerrainTileDesc*      drapeTile       = nullptr;
        TerrainHeights              drapeHeights    = {};
    };

    // Vertices and indices for a VECTOR material's mesh (the layout is in shaders/lib/vector.slang). The add functions
    // append to it; each is safe on any thread, on its own geometry.
    struct VectorGeometry
    {
        std::vector<Vertex>         vertices;
        std::vector<uint32_t>       indices;
        std::vector<MeshLabel>      labels;                             // addVectorLabels'
        std::vector<uint32_t>       colors;
    };

    // Where a label's text sits around its anchor on screen.
    enum class LabelAnchor : uint32_t
    {
        CENTER,         // the text's middle on the anchor
        BOTTOM,         // the text above the anchor (a place's name over its point)
        TOP,            // the text below the anchor
        LEFT,           // the text right of the anchor
        RIGHT,          // the text left of the anchor
    };

    struct LabelDesc
    {
        glm::dvec3                  position        = glm::dvec3(0.0);  // the anchor, world space
        const char*                 text            = nullptr;          // UTF-8; a line feed starts a new line
        float                       priority        = 0.0f;             // collision keeps the higher first
        LabelAnchor                 anchor          = LabelAnchor::CENTER;
        glm::vec2                   offset          = glm::vec2(0.0f);  // ems from the anchor, +y up
        uint32_t                    featureId       = 0;
    };

    // origin: the world position of the entity that will draw the mesh (no rotation, scale 1); the vertices are offsets
    // from it in floats, so a map far from the world's origin keeps its precision. points: every line's points, one line
    // after another, lineSizes[i] of them for line i (a line of fewer than 2 distinct points is skipped). featureIds:
    // one per line (the picking id), or null for the line's number. Draped, a line's middle is cut on the tile's grid and
    // lies on the drawn ground, but its width spreads in the plane of the triangle under the middle, so a band wider
    // than a cell sinks into the ground beside it where the ground bends. With shape.drapeWidth > 0 a draped line is
    // built that wide (metres), cut on the grid across its whole width: all of it lies on the drawn ground. It is then
    // drawn that wide in the ground's plane whatever the material's width and facing say (the outline and the dashes
    // still follow the material), and a new width takes a rebuild. It is cut at the tile's edges: give each tile the
    // lines within half the width of it, and its neighbour draws the rest.
    void                            addVectorLines(VectorGeometry& geometry, const glm::dvec3& origin, const glm::dvec3* points, const uint32_t* lineSizes, uint32_t lineCount, const LineShape& shape = {},
                                                   const VectorPlacement& placement = {}, const uint32_t* featureIds = nullptr, const uint32_t* colors = nullptr, const float* scalars = nullptr);
    // Polygons with holes: points holds every ring, one after another, ringSizes[r] points for ring r; polygon i has
    // ringCounts[i] rings, its outline first, then its holes. Rings need not be closed nor wound a given way.
    void                            addVectorPolygons(VectorGeometry& geometry, const glm::dvec3& origin, const glm::dvec3* points, const uint32_t* ringSizes, const uint32_t* ringCounts, uint32_t polygonCount,
                                                      const VectorPlacement& placement = {}, const uint32_t* featureIds = nullptr);
    // Points or markers: a disc each, or with iconRects (u0, v0, u1, v1 in the material's albedo texture) a square icon.
    void                            addVectorPoints(VectorGeometry& geometry, const glm::dvec3& origin, const glm::dvec3* points, uint32_t pointCount, const VectorPlacement& placement = {},
                                                    const uint32_t* featureIds = nullptr, const glm::vec4* iconRects = nullptr);
    // Labels: each text laid out with the font's metrics (kerning included, lines centred), a quad per glyph spread
    // across the screen from its anchor by the material's width (the text's size in pixels or metres). Each label is one
    // for collision: per view and frame the renderer keeps the higher-priority labels that do not overlap, fading them
    // in and out. Draw the mesh with a VECTOR material whose font is the atlas's Renderer::createFont.
    void                            addVectorLabels(VectorGeometry& geometry, const glm::dvec3& origin, const FontAtlas& font, const LabelDesc* labels, uint32_t labelCount,
                                                    const VectorPlacement& placement = {});
    // buildMesh with no LODs and no meshlets (the shader spreads the vertices), and the labels. Safe on any thread.
    MeshBuild                       buildVectorMesh(VectorGeometry geometry);

}
// clang-format on
