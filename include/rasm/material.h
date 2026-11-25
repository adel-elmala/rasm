// clang-format off
#pragma once

#include "rasm/colormap.h"
#include "rasm/types.h"

namespace rasm
{
    enum class MaterialType
    {
        PBR,
        UNLIT,
        TERRAIN,        // a terrain tile: the shared grid lifted by Material::terrain's heights, shaded as PBR (rasm/terrain.h)
        VECTOR,         // lines, polygons and points (rasm/vector.h), styled by Material::vector, unlit and blended
        POINTS,         // point cloud chunks (rasm/pointCloud.h), styled by Material::points, unlit

        COUNT,
    };

    enum class PbrSlot
    {
        ALBEDO,
        METALLIC,
        ROUGHNESS,
        NORMAL,
        OCCLUSION,
        EMISSIVE,

        COUNT,
    };

    enum class PbrParam : uint32_t
    {
        ALBEDO              = 0,      // vec4
        METALLIC            = 4,      // float
        ROUGHNESS           = 5,      // float
        NORMAL_SCALE        = 6,      // float
        OCCLUSION_STRENGTH  = 7,      // float
        EMISSIVE_FACTOR     = 8,      // vec3
        EMISSIVE_STRENGTH   = 11,     // float

        COUNT = 12,
    };

    enum class AlphaMode : uint32_t
    {
        NONE,
        MASK,
        BLEND,
    };

    struct TextureTransform
    {
        glm::vec2      offset       = glm::vec2(0.0f);
        float          rotation     = 0.0f;
        glm::vec2      scale        = glm::vec2(1.0f);
    };

    // A TERRAIN material's heights. A height in metres is heightOffset + heightScale * sample, where an R16_UNORM sample is
    // its integer (0 to 65535) and an R32_SFLOAT sample its value. Neighbouring tiles get bit-equal heights on their
    // shared edge when heightScale is a power of two and heightOffset a multiple of it (rasm/terrain.h).
    struct TerrainMaterial
    {
        TextureHandle  heights;                     // R16_UNORM or R32_SFLOAT, (cells + 3) x (cells + 3): the grid's points and a border of one
        float          heightScale  = 1.0f;
        float          heightOffset = 0.0f;
        float          minHeight    = 0.0f;         // metres: the lowest and highest heights, the tile's box (getTerrainTransform)
        float          maxHeight    = 0.0f;
        float          skirt        = 0.0f;         // metres the skirt hangs below the tile's edges, to hide cracks between levels
        double         sphereRadius = 0.0;          // 0: flat; else the tile bends down by d^2 / 2R at d metres from its centre
    };

    // What a VECTOR material's width and dashes are measured in.
    enum class VectorUnit : uint32_t
    {
        PIXELS,         // on screen, whatever the distance
        METRES,         // in the world: a road's real width
    };

    // How a line or a point spreads its width.
    enum class VectorFacing : uint32_t
    {
        SCREEN,         // across the screen: the width never foreshortens (a 3D path, a marker)
        GROUND,         // in the plane of the ground under it (vector/drape up): lies flat on the terrain, foreshortened as the ground is
    };

    enum class VectorDepth : uint32_t
    {
        TESTED,         // hidden behind what is in front of it; a small bias keeps it over the ground it lies on
        OVERLAY,        // over everything, never hidden
    };

    // A VECTOR material: the style of a layer of lines, polygons or points. Every feature is at least 1 pixel wide; a
    // thinner one keeps 1 pixel and fades by its coverage, so it neither vanishes nor shimmers.
    struct VectorMaterial
    {
        glm::vec4      color        = glm::vec4(1.0f);  // linear RGB and alpha; times the vertex colors
        float          width        = 1.0f;             // a line's width, a point's diameter, in widthUnit; polygons have none
        VectorUnit     widthUnit    = VectorUnit::PIXELS;
        VectorFacing   facing       = VectorFacing::SCREEN;
        float          dashLength   = 0.0f;             // in widthUnit; 0: a solid line
        float          gapLength    = 0.0f;
        glm::vec4      outlineColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // a band along a line's or point's edge (a road's casing, a marker's ring)
        float          outlineWidth = 0.0f;             // in widthUnit, inside width; 0: none
        int32_t        drawOrder    = 0;                // -127 to 127: blended draws of a lower order come first; 0 sorts with the scene's see-through draws
        VectorDepth    depth        = VectorDepth::TESTED;
        FontHandle     font;                            // labels: their glyphs' atlas (Renderer::createFont); width is the text's size, outline its halo
    };

    enum class PointShape : uint32_t
    {
        SQUARE,
        ROUND,
        SPHERE,         // round, with the depth of a sphere
    };

    enum class PointColorMode : uint32_t
    {
        COLOR,          // the points' own colors, white without them
        SCALAR,         // the points' scalars through colormap and range
    };

    struct PointMaterial
    {
        glm::vec4      color        = glm::vec4(1.0f);  // linear RGB, times the point's color; points are opaque, alpha 0 hides them
        float          size         = 2.0f;             // the diameter, in sizeUnit; at least 1 pixel
        VectorUnit     sizeUnit     = VectorUnit::PIXELS;
        PointShape     shape        = PointShape::ROUND;
        PointColorMode colorMode    = PointColorMode::COLOR;
        TextureHandle  colormap;                        // Renderer::createColormap
        ScalarRange    range        = {};
        float          density      = 2.0f;             // points drawn per point-size square of the chunk on screen; 0: all
    };

    constexpr uint32_t MAX_TEXTURE_SLOTS = static_cast<uint32_t>(PbrSlot::COUNT);
    constexpr uint32_t MAX_PARAM_SLOTS   = static_cast<uint32_t>(PbrParam::COUNT);

    struct Material
    {
        const char*      name{};
        MaterialHandle   handle{};
        MaterialType     type{};
        TextureHandle    textures[MAX_TEXTURE_SLOTS];
        uint32_t         texCoords[MAX_TEXTURE_SLOTS];
        TextureTransform textureTransforms[MAX_TEXTURE_SLOTS];
        float            params[MAX_PARAM_SLOTS];
        bool             doubleSided{};
        AlphaMode        alphaMode{};
        float            alphaCutoff  = 0.5f;
        TerrainMaterial  terrain      = {};         // TERRAIN only; textures[ALBEDO] is the imagery, textureTransforms[ALBEDO] its part of the image
        VectorMaterial   vector       = {};         // VECTOR only; textures[ALBEDO] is a points' icon atlas
        PointMaterial    points       = {};         // POINTS only
    };

}
// clang-format on