// clang-format off
#pragma once

#include "animationTypes.slang"
#include "renderSpace.slang"

// Constants
static const uint   INVALID_TEXTURE_INDEX = 0xFFFFFFFF;
// A run per alpha mode and cull mode, and one blend run. Must match rasm::gfx::MAX_DRAW_RUNS.
static const uint   MAX_DRAW_RUNS       = 8;

// Selects how fragMain shades a model. Must match rasm::gfx::MaterialId.
enum class MaterialId : uint32_t { PBR, UNLIT, TERRAIN, VECTOR, POINTS, GLYPHS };

// Must match rasm::AlphaMode.
enum class AlphaMode : uint32_t { NONE, MASK, BLEND };

// The textures with a KHR_texture_transform each, in Material::textureTransforms. Must match rasm::gfx::TextureTransformSlot.
static const uint   TEXTURE_TRANSFORMS              = 5;
static const uint   TRANSFORM_BASE_COLOR            = 0;
static const uint   TRANSFORM_METALLIC_ROUGHNESS    = 1;
static const uint   TRANSFORM_NORMAL                = 2;
static const uint   TRANSFORM_OCCLUSION             = 3;
static const uint   TRANSFORM_EMISSIVE              = 4;

// MaterialData::flags. Must match rasm::gfx::MATERIAL_*.
static const uint   MATERIAL_DOUBLE_SIDED           = 1 << 0;
static const uint   MATERIAL_TEXTURE_TRANSFORM      = 1 << 1;   // a texture has a KHR_texture_transform that is not the identity
static const uint   MATERIAL_NORMAL_XY              = 1 << 2;   // the normal map has x and y only (BC5, EAC RG11): z is rebuilt

// SceneData::viewFlags. Must match rasm::gfx::VIEW_*.
static const uint   VIEW_DEPTH_ONLY                 = 1 << 0;   // a shadow map's view: it draws no VECTOR material

// Must match rasm::MAX_SHADOW_CASCADES.
static const uint MAX_SHADOW_CASCADES = 4;
// Must match rasm::MAX_VIEW_SHADOWS.
static const uint MAX_VIEW_SHADOWS = 4;
// Must match rasm::gfx::MAX_SHADOW_MAPS: MAX_VIEW_SHADOWS lights of up to MAX_SHADOW_CASCADES maps.
static const uint MAX_SHADOW_MAPS = 16;

struct VertexPosOnly    { float3 pos; };

struct DrawInfo
{
    uint32_t        indexCount;
    uint32_t        instanceCount;
    uint32_t        firstIndex;
    int32_t         vertexOffset;
    uint32_t        firstInstance;
};

enum class LightType : uint32_t { DIRECTIONAL, POINT, SPOT };

// Must match rasm::gfx::Light (scalar layout): a row of the light table, at the light's handle index.
struct Light
{
    float3      position;       // point and spot lights
    float       intensity;
    float3      color;
    float       range;          // point and spot lights; 0: unbounded, a global light
    float3      direction;      // directional and spot lights
    LightType   type;
    float       spotScale;      // the cone's falloff: saturate(dot(direction, -l) * spotScale + spotOffset)
    float       spotOffset;
    float3      positionLow;    // position is the high part; getLight gives the light with its position in render space
};

// Must match rasm::gfx::ShadowedLight: a light of the view that has a shadow, and its maps.
struct ShadowedLight
{
    uint32_t    light;          // its row in the light table
    uint32_t    firstMap;       // in SceneData::shadowMaps
    uint32_t    mapCount;       // its cascades, or 1 for a spot or a point light (a cube)
};

// Must match rasm::gfx::ShadowMap.
struct ShadowMap
{
    float4x4    matrix;         // render space to clip: a cascade's or a spot's projection * view; a point light's face projection
    uint32_t    texture;        // bindless index of the depth; a point light's is a cube (gCubeTextures)
    float       split;          // a cascade's: the view depth where it ends
};

// Must match rasm::gfx::GridData: the camera's reference grid in the view's render space (passes/grid.slang).
struct GridData
{
    float3      axisX;          // the plane's X axis
    float       height;         // the view origin's height above the plane, along normal
    float3      axisZ;
    float       cellSize;       // the finest level's cells
    float3      normal;
    float       subdivisions;
    uint32_t    residueX;       // floor(the origin's X on the plane / cellSize) modulo period
    uint32_t    residueZ;
    float       fractionX;      // the rest, in cells
    float       fractionZ;
    float2      originCells;    // the origin on the plane in cells, rounded: the axes and the extent
    float2      extent;         // half sizes in cells; 0: endless
    float4      minorColor;
    float4      majorColor;
    float4      xAxisColor;     // the line along axisX, alpha 0 without axes
    float4      zAxisColor;
    float       lineWidth;      // pixels
    float       majorLineWidth;
    float       fadeDistance;   // 0: no fade
    uint32_t    levelCount;     // the levels above the finest; 0: no grid
    uint32_t    period;         // subdivisions^levelCount
    uint32_t    zero;           // keeps the size a multiple of 8
};

struct MetallicRoughnessMaterial
{
    float4      baseColorFactor;
    uint32_t    baseColorTextureIndex;
    uint32_t    baseColorTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    uint32_t    metallicRoughnessTextureIndex;
    uint32_t    metallicRoughnessTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       metallicFactor;
    float       roughnessFactor;
};

struct NormalMaterial
{
    uint32_t    normalTextureIndex;
    uint32_t    normalTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       scale;
};

struct OcclusionMaterial
{
    uint32_t    occlusionTextureIndex;
    uint32_t    occlusionTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       strength;
};

struct EmissiveMaterial
{
    uint32_t    emissiveTextureIndex;
    uint32_t    emissiveTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float3      emissiveFactor;
};

// KHR_texture_transform: uv' = (dot(matrix.xy, uv), dot(matrix.zw, uv)) + offset. Identity without the extension.
struct TextureTransform
{
    float4      matrix;
    float2      offset;
};

struct Material
{
    MetallicRoughnessMaterial   metallicRoughness;
    NormalMaterial              normal;
    OcclusionMaterial           occlusion;
    EmissiveMaterial            emissive;           // emissiveFactor holds KHR_materials_emissive_strength too
};

// A TERRAIN material's heights (lib/terrain.slang); zero for the others. Must match rasm::gfx::TerrainData.
struct TerrainData
{
    uint32_t            heights;            // bindless index
    uint32_t            heightKind;         // TERRAIN_HEIGHTS_*
    float               heightScale;        // metres = heightOffset + heightScale * sample
    float               heightOffset;
    float               minHeight;
    float               skirt;
    float               sphereRadius;       // 0: flat
};

// A VECTOR material's style (lib/vector.slang); zero for the others. Must match rasm::gfx::VectorData.
struct VectorData
{
    float4              color;              // linear, straight alpha
    float4              outlineColor;
    float               width;              // in pixels or metres (VECTOR_METRES)
    float               outlineWidth;
    float               dashLength;
    float               gapLength;
    uint32_t            flags;              // VECTOR_*
    int32_t             drawOrder;
    uint32_t            font;               // the font atlas's bindless index, INVALID_TEXTURE_INDEX without a font
    float               distanceRange;      // the atlas's: a texel holds 0.5 + distance / distanceRange
};

// The head of a view's point raster buffer (passes/pointRaster.slang): the raster and clear dispatches' group counts,
// indirect, and the pixels the compute path's chunks can reach. Must match rasm::gfx::PointRasterHeader.
struct PointRasterHeader
{
    uint32_t            rasterGroups;
    uint32_t            rasterY;
    uint32_t            rasterZ;
    uint32_t            clearGroups;
    uint32_t            clearY;
    uint32_t            clearZ;
    uint32_t            rectMinX;
    uint32_t            rectMinY;
    uint32_t            rectMaxX;
    uint32_t            rectMaxY;
    uint32_t            padding[2];
};

// The head of a view's glyph buffer (passes/glyphCull.slang): the cull's group count, indirect, the chunks it kept and
// the glyph draw's command count. Must match rasm::gfx::GlyphCullHeader.
struct GlyphCullHeader
{
    uint32_t            groups;
    uint32_t            groupsY;
    uint32_t            groupsZ;
    uint32_t            chunkCount;
    uint32_t            commandCount;
    uint32_t            padding[3];
};

// A glyph chunk the view's cull kept (lib/glyphs.slang). Must match rasm::gfx::GlyphChunkSlot.
struct GlyphChunkSlot
{
    uint32_t            row;
    uint32_t            firstBatch;         // its glyphs' slots and list entries start at firstBatch * GLYPH_BATCH_GLYPHS
    uint32_t            firstCommand;       // its shape's LODs' draws in glyphCommandsPtr
    uint32_t            lodCount;
    uint32_t            counts[MAX_MESH_LODS];  // its visible glyphs per LOD
};

// A POINTS material's style (lib/points.slang); zero for the others. Must match rasm::gfx::PointData.
struct PointData
{
    float4              color;              // linear, straight alpha
    float               size;               // diameter in pixels or metres (POINT_METRES)
    uint32_t            flags;              // POINT_*
    uint32_t            colormap;           // bindless index, INVALID_TEXTURE_INDEX without one
    float               rangeMin;           // the scalars the colormap spans (their logarithms with POINT_LOG)
    float               rangeMax;
    float               density;            // points drawn per point-size square of the chunk on screen; 0: all
};

// The texture transforms come after the rest, so a shader copies Material without them and reads them only for a
// material whose flags have MATERIAL_TEXTURE_TRANSFORM.
struct MaterialData
{
    Material            material;
    MaterialId          materialId;
    AlphaMode           alphaMode;
    float               alphaCutoff;
    uint32_t            flags;
    TextureTransform    textureTransforms[TEXTURE_TRANSFORMS];
    TerrainData         terrain;
    VectorData          vector;
    PointData           points;
};

struct InstanceData
{
    float4          world[3];           // the rows of the affine world matrix; w: the translation's high part
    float3          translationLow;     // the translation is w + translationLow; getWorldMatrix gives it in render space
    float3x3        normalMatrix;
    uint32_t        geometryIndex;
    uint32_t        materialIndex;
    float4          previousWorld[3];   // the rows the frame before drew, for motion; w: its translation minus this one
    int32_t         animationRow;       // its InstanceTransform in SceneData::animatedTransformsPtr, -1: the matrices above
    int32_t         meshletVisibility;  // its first word in each view's meshlet visibility bits, -1: none; a VECTOR mesh with labels (no meshlets): its first label slot; a point chunk: its PointChunkLod (lib/points.slang)
};

struct SceneData
{
    float4x4            projection;
    float4x4            view;
    float3              originHigh;         // the view's origin, its camera: render space = world - origin (toRenderSpace)
    uint32_t            shadowedLightCount; // shadowedLights in use
    VertexFull*         vertsPtr;
    DrawInfo*           drawInfoPtr;
    InstanceData*       instancesPtr;
    MaterialData*       materialsPtr;
    GeometryData*       geometryPtr;
    DrawInfo*           allDrawsPtr;        // the scene's whole draw list; cull.slang packs the visible ones into drawInfoPtr
    uint32_t*           drawCountsPtr;      // visible draws per run, written by cull.slang
    uint32_t*           cullSlotsPtr;       // cull.slang's scratch: a draw's slot among its group's visible draws, and its LOD
    uint32_t*           groupCountsPtr;     // cull.slang's scratch: visible draws per group of 64
    uint32_t*           cullStatsPtr;       // the frame's visible draws, triangles and LODs, added up by cull.slang (STATS_* there)
    uint32_t*           visibilityPtr;      // per instance row: 1 if the last occlusion test saw it, read by the early cull
    float*              hizPtr;             // the call's Hi-Z: farthest depth per texel, levels packed one after another
    uint32_t*           colorsPtr;          // the vertex color pool: RGBA8, RGB sRGB-encoded (GeometryData::colorOffset)
    uint2*              sortKeysPtr;        // the blend sort's keys: (~view depth, draw), sortCapacity of them (sort.slang)
    DrawInfo*           sortDrawsPtr;       // the blend sort's copy of the blend run's visible draws
    Light*              lightsPtr;          // the light table
    uint32_t*           sceneLightsPtr;     // the scene's light rows: globalLightCount global lights, then the local ones
    uint32_t*           clustersPtr;        // the view's light clusters (lightClusters.slang); null: every pixel shades every local light
    float4              frustumPlanes[6];   // render space, inside where dot(xyz, p) + w >= 0
    uint32_t            drawCount;
    uint32_t            runCount;
    uint32_t            runFirst[MAX_DRAW_RUNS];
    uint32_t            blendRun;           // the run the Transparent pass draws, sorted back to front; MAX_DRAW_RUNS without one
    uint32_t            pointRun;           // the run of point cloud chunks (lib/points.slang); MAX_DRAW_RUNS without one
    uint32_t            sortCapacity;
    uint32_t            hizLevelCount;      // 0: no occlusion culling for this call
    uint32_t            depthTexture;       // bindless index of the pass's depth texture
    uint32_t            viewportWidth;      // the pass's size in pixels
    uint32_t            viewportHeight;
    float               lodErrorPixels;     // culling picks the coarsest LOD whose error stays under this on screen
    int32_t             forcedLod;          // >= 0: every draw uses this LOD (or its mesh's coarsest), for debugging
    ShadowMap           shadowMaps[MAX_SHADOW_MAPS]; // the maps of the shadowed lights, found through Light::shadowMap
    float               lodPixelsPerUnit;   // the LOD camera's pixels per world unit at view depth 1 (lodSelection.slang)
    float4              lodDepthPlane;      // dot(lodDepthPlane, float4(p, 1)) is p's view depth from the LOD camera; 0 if orthographic
    uint32_t            environmentSpecular;        // bindless cube of prefiltered light; INVALID_TEXTURE_INDEX: no environment
    uint32_t            environmentSpecularMips;
    uint32_t            environmentIrradiance;      // bindless cube
    uint32_t            environmentSky;             // bindless cube drawn behind the scene; INVALID_TEXTURE_INDEX: no sky
    uint32_t            brdfLut;                    // bindless 2D: the split sum's scale and bias of f0
    float               environmentIntensity;
    float2              environmentRotation;        // cos and sin of the environment's turn around +Y
    ShadowedLight       shadowedLights[MAX_VIEW_SHADOWS];
    uint32_t            globalLightCount;   // directional lights and lights without a range, shaded at every pixel
    uint32_t            localLightCount;    // lights with a range, sorted into clusters
    float               nearZ;              // the camera's near plane, where the clusters start
    uint32_t            depthSamples;       // samples per pixel of depthTexture; the Hi-Z build reads each one
    float4x4            previousViewProjection; // the frame before's, without jitter, for motion
    float2              jitter;             // NDC offset added to clip x and y (times w): TAA's sub-pixel jitter
    float               textureLodBias;     // added to the material textures' mip level: -1 while jittered, sharper
    uint32_t            debugView;          // rasm::DebugView: what a RASM_DEBUG_VIEW pipeline shows (debugViews.slang); also keeps animatedTransformsPtr 8-byte aligned
    InstanceTransform*  animatedTransformsPtr;  // this slot's rows, written by the Animate pass
    VertexFull*         skinnedVertsPtr;        // this slot's skinned vertices, written by the Skin pass
    VertexFull*         previousSkinnedVertsPtr;    // the other slot's: the frame before's
    GeometryMeshlet*    meshletsPtr;            // the geometry pool's meshlet stream
    DrawInfo*           meshletDrawsPtr;        // the view's visible meshlet draws, each run's at runMeshletFirst; null: no meshlets
    uint4*              meshletChunksPtr;       // per chunk of up to 64 meshlets of a draw: draw, lod and flags, first meshlet, count
    uint2*              chunkMasksPtr;          // per chunk: its visible meshlets
    uint32_t*           chunkSumsPtr;           // per 64 chunks: their visible meshlets
    uint4*              meshletArgsPtr;         // per run: the meshlet dispatches' group counts, then the run's chunk count
    uint32_t*           meshletDrawCountsPtr;   // per run: its visible meshlet draws
    uint2*              groupMeshletsPtr;       // per cull group: its chunks and its meshlets
    uint32_t*           meshletVisibilityPtr;   // the view's bits: 1 where a meshlet passed its last occlusion test
    float4              cullViewPoint;          // the camera's position (w = 1), or an orthographic view's direction (w = 0)
    uint32_t            runChunkFirst[MAX_DRAW_RUNS];
    uint32_t            runMeshletFirst[MAX_DRAW_RUNS];
    uint32_t            runMeshletBudget[MAX_DRAW_RUNS];   // a run's meshlet draws past this are drawn whole
    uint32_t            meshletRuns;            // a bit per run whose big draws are drawn by meshlets
    uint32_t            coneRuns;               // a bit per run culled by face, where the cone test holds
    uint32_t            viewFlags;              // VIEW_*
    uint32_t            padding;                // keeps meshletDataPtr 8-byte aligned
    uint32_t*           meshletDataPtr;         // the meshlets' vertices and triangles, for the mesh shaders; null without them
    float3              originLow;              // the origin is originHigh + originLow
    uint32_t            zero;                   // 0, which the shader compiler cannot know (renderSpace.slang)
    float4              environmentOrientation; // quaternion (x, y, z, w): the environment's frame in the world (environment.slang)
    float*              labelsPtr;              // a shaded view's label slots' alpha (lib/vector.slang); null: every label shown
    GridData            grid;                   // the camera's grid, levelCount 0 without one
    float4*             labelProbesPtr;         // per label slot: its anchor's pixel, the depth it must be in front of, its test (LABEL_PROBE_*)
    float*              labelFadesPtr;          // the view's label slots' fade by depth, kept across frames (passes/labelDepth.slang)
    float               labelFadeStep;          // the fade's change this frame
    uint32_t            labelSlotCount;         // slots the Label Depth pass tests: 0 to this
    uint2*              pointsPtr;              // the point pool in 8 B units: each chunk's header, positions and colors (lib/points.slang)
    PointRasterHeader*  pointRasterPtr;         // the view's point raster buffer; null: every point by triangles (passes/pointRaster.slang)
    uint2*              pointBatchesPtr;        // per batch: the instance row, first point | count << 16
    uint64_t*           pointPixelsPtr;         // per pixel: depth << 32 | instance row << 16 | point, 0 where no point
    uint32_t            pointBatchCapacity;
    uint32_t            pointRasterPadding;
    GlyphCullHeader*    glyphCullPtr;           // the view's glyph buffer; null: no glyph is drawn (passes/glyphCull.slang)
    GlyphChunkSlot*     glyphChunksPtr;         // per glyph chunk the cull kept
    uint32_t*           glyphBatchesPtr;        // per batch: chunk slot << 8 | its batch in the chunk
    DrawInfo*           glyphCommandsPtr;       // per kept chunk and LOD of its shape: the glyph draw's commands
    uint32_t*           glyphSlotsPtr;          // per glyph of a kept chunk: its place among its chunk's LOD | lod << 24, or GLYPH_CULLED
    uint32_t*           glyphListPtr;           // the visible glyphs, chunk slot << 16 | glyph, each chunk's by LOD
    uint32_t            glyphChunkCapacity;
    uint32_t            glyphBatchCapacity;
};

// Each indirect draw restarts DrawIndex at 0, so firstDraw is added back to find the draw's DrawInfo.
// argument is the cull phase (cull.slang, and the draw passes for the culling debug view) or the Hi-Z level.
struct PushConstants
{
    SceneData* scene;
    uint32_t   firstDraw;
    uint32_t   argument;
};

// A pass with push constants of its own (passes/inspect.slang) defines RASM_OWN_PUSH_CONSTANTS before it includes
// this file, and goes without the functions that read pc.
#if !defined(RASM_OWN_PUSH_CONSTANTS)
[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

// The draw's index in the scene: the native Vulkan DrawIndex built-in plus the indirect call's first draw.
uint getDrawIndex()
{
    uint drawIndex = spirv_asm
    {
        OpExtension "SPV_KHR_shader_draw_parameters";
        OpCapability DrawParameters;
        result:$$uint = OpLoad builtin(DrawIndex:uint);
    };
    return drawIndex + pc.firstDraw;
}

// The instance's index with the draw's firstInstance in it, which Slang's SV_InstanceID leaves out: the glyph draw's
// commands start each LOD's instances at its place in the glyph list.
uint getInstanceIndex()
{
    return spirv_asm
    {
        result:$$uint = OpLoad builtin(InstanceIndex:uint);
    };
}
#endif

bool hasTexture(uint textureIndex)
{
    return textureIndex != INVALID_TEXTURE_INDEX;
}

// A clip position without the view's TAA jitter, which SceneData::projection adds to x and y (times w).
float4 removeJitter(SceneData *scene, float4 clip)
{
    return float4(clip.xy - scene->jitter * clip.w, clip.zw);
}

// The screen motion from the frame before to this one, in UV units (v grows down), from the two clip positions of the
// same point: UV now minus UV then, so the point was at uv - motion. Both are without jitter, so a still scene has none.
// A point that was behind the camera (w <= 0) had no place on the screen: its motion puts it off the screen, where
// TAA takes this frame only.
float2 getMotion(float4 currentClip, float4 previousClip)
{
    if (previousClip.w <= 1e-6)
        return float2(2.0);
    float2 delta = currentClip.xy / currentClip.w - previousClip.xy / previousClip.w;
    return delta * float2(0.5, -0.5);
}

// A material texture at the view's mip bias: TAA's jitter blends the frames into a sharper image than one frame, so it
// reads one mip sharper (SceneData::textureLodBias).
#if !defined(RASM_OWN_PUSH_CONSTANTS)
float4 sampleMaterial(Sampler2D texture, float2 uv)
{
    return texture.SampleBias(uv, pc.scene->textureLodBias);
}

// glTF base color: factor * texture * COLOR_0, or without the texture when the material has none.
float4 sampleBaseColor(Sampler2D gTextures[], MetallicRoughnessMaterial material, float2 uv, float4 vertexColor)
{
    float4 color = material.baseColorFactor * vertexColor;
    if (hasTexture(material.baseColorTextureIndex))
        color *= sampleMaterial(gTextures[material.baseColorTextureIndex], uv);
    return color;
}
#endif

// Pick the interpolated UV set a material texture asked for (glTF TEXCOORD_n). Sets we do not
// carry yet fall back to TEXCOORD_0, mirroring the CPU-side fallback in loadMaterial.
float2 selectUV(uint texCoordSet, float2 uv0, float2 uv1, float2 uv2)
{
    if (texCoordSet == 1) return uv1;
    if (texCoordSet == 2) return uv2;
    return uv0;
}

float2 applyTextureTransform(TextureTransform transform, float2 uv)
{
    return float2(dot(transform.matrix.xy, uv), dot(transform.matrix.zw, uv)) + transform.offset;
}

// The UV set one texture asked for, through its KHR_texture_transform. The flag is the same for the whole draw, so the
// branch is uniform, and a material without transforms does not load them. A shader that reads several textures
// branches once for all of them instead (fragMain): a branch per texture costs the opaque draws about 6% (05_city).
float2 getTextureUV(MaterialData *material, uint transform, uint texCoordSet, float2 uv0, float2 uv1, float2 uv2)
{
    float2 uv = selectUV(texCoordSet, uv0, uv1, uv2);
    if ((material->flags & MATERIAL_TEXTURE_TRANSFORM) == 0)
        return uv;
    return applyTextureTransform(material->textureTransforms[transform], uv);
}

float decodeSrgb(float encoded)
{
    return encoded <= 0.04045 ? encoded / 12.92 : pow((encoded + 0.055) / 1.055, 2.4);
}

// A world position stored as a high and a low float, in the view's render space: the world moved so that the camera
// is at the origin (renderSpace.slang).
float3 toRenderSpace(SceneData *scene, float3 high, float3 low)
{
    return subtractSplit(high, low, scene->originHigh, scene->originLow, scene->zero);
}

// A light row with its position in render space.
Light getLight(SceneData *scene, uint row)
{
    Light light     = scene->lightsPtr[row];
    light.position  = toRenderSpace(scene, light.position, light.positionLow);
    return light;
}

float3 getRowTranslation(float4 rows[3])
{
    return float3(rows[0].w, rows[1].w, rows[2].w);
}

float4x4 getRenderMatrix(SceneData *scene, float4 rows[3], float3 translationLow)
{
    float3 t = toRenderSpace(scene, getRowTranslation(rows), translationLow);
    return float4x4(float4(rows[0].xyz, t.x), float4(rows[1].xyz, t.y), float4(rows[2].xyz, t.z), float4(0.0, 0.0, 0.0, 1.0));
}

// An animated instance's matrices come from the Animate pass, every other one's from its instance row. The world
// matrix is in render space.
float4x4 getWorldMatrix(SceneData *scene, InstanceData *instance)
{
    if (instance->animationRow >= 0)
    {
        InstanceTransform *transform = scene->animatedTransformsPtr + instance->animationRow;
        return getRenderMatrix(scene, transform->world, transform->translationLow);
    }
    return getRenderMatrix(scene, instance->world, instance->translationLow);
}

float3x3 getNormalMatrix(SceneData *scene, InstanceData *instance)
{
    if (instance->animationRow >= 0)
        return scene->animatedTransformsPtr[instance->animationRow].normalMatrix;
    return instance->normalMatrix;
}

// Where the frame before drew an object-space position, in this frame's render space: the rows' w is the translation
// the frame before drew minus this frame's, small, so it adds to this frame's translation without losing precision.
float3 getPreviousWorldPosition(SceneData *scene, InstanceData *instance, float3 position)
{
    float4x4 world      = getWorldMatrix(scene, instance);
    float4 rows[3]      = instance->previousWorld;
    if (instance->animationRow >= 0)
        rows = scene->animatedTransformsPtr[instance->animationRow].previousWorld;
    float3 linear       = float3(dot(rows[0].xyz, position), dot(rows[1].xyz, position), dot(rows[2].xyz, position));
    return linear + float3(world[0].w, world[1].w, world[2].w) + getRowTranslation(rows);
}

// The instance's first vertex in this slot's skinned vertices, or -1 when its vertices are the geometry pool's.
int getSkinnedVertex(SceneData *scene, InstanceData *instance)
{
    if (instance->animationRow >= 0)
        return scene->animatedTransformsPtr[instance->animationRow].skinnedVertex;
    return -1;
}

// A vertex of the draw: a skinned sub-mesh's from this frame's Skin pass, any other from the geometry pool. vertexID
// is the raw index-buffer value, local to the sub-mesh.
VertexFull getVertex(SceneData *scene, InstanceData *instance, int vertexOffset, uint vertexID)
{
    int skinned = getSkinnedVertex(scene, instance);
    if (skinned >= 0)
        return scene->skinnedVertsPtr[uint(skinned) + vertexID];
    return scene->vertsPtr[vertexID + vertexOffset];
}

// The object-space position the frame before drew for a vertex: a skinned one's from the other slot's skinned
// vertices, when they hold the frame before; any other vertex does not move in object space.
float3 getPreviousObjectPosition(SceneData *scene, InstanceData *instance, uint vertexID, float3 position)
{
    int skinned = getSkinnedVertex(scene, instance);
    if (skinned < 0 || scene->animatedTransformsPtr[instance->animationRow].skinnedMotion == 0)
        return position;
    return scene->previousSkinnedVertsPtr[uint(skinned) + vertexID].pos;
}

// A vertex's COLOR_0 as linear RGBA, white for a sub-mesh without colors (the color stream, geometryPool.cpp). The
// stream is indexed like the vertices, by the sub-mesh's own index.
float4 getVertexColor(SceneData *scene, uint geometryIndex, uint vertexIndex)
{
    int colorOffset = scene->geometryPtr[geometryIndex].colorOffset;
    if (colorOffset < 0)
        return float4(1.0);
    uint packed = scene->colorsPtr[uint(colorOffset) + vertexIndex];
    float4 color = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    return float4(decodeSrgb(color.r), decodeSrgb(color.g), decodeSrgb(color.b), color.a);
}

// A data line's vertex (a VECTOR material with a colormap): its scalar, from the color stream's word, as (value, 0, 0, -1)
// for the fragment stage to map (rasm.slang, getVectorColor); white for a mesh without colors and a vertex without a value.
float4 getVertexScalar(SceneData *scene, uint geometryIndex, uint vertexIndex)
{
    int colorOffset = scene->geometryPtr[geometryIndex].colorOffset;
    if (colorOffset < 0)
        return float4(1.0);
    // A feature without values in the same mesh (a label, a polygon, a line added without them) holds white: it keeps
    // the material's color.
    uint word = scene->colorsPtr[uint(colorOffset) + vertexIndex];
    return word == 0xFFFFFFFF ? float4(1.0) : float4(asfloat(word), 0.0, 0.0, -1.0);
}

#if !defined(RASM_OWN_PUSH_CONSTANTS)
// The base color's alpha, all the alpha modes test: factor * texture * COLOR_0.
float getBaseAlpha(Sampler2D gTextures[], MaterialData *material, float2 uv0, float2 uv1, float2 uv2, float vertexAlpha)
{
    float alpha = material->material.metallicRoughness.baseColorFactor.a * vertexAlpha;
    uint texture = material->material.metallicRoughness.baseColorTextureIndex;
    if (hasTexture(texture))
    {
        float2 uv = getTextureUV(material, TRANSFORM_BASE_COLOR, material->material.metallicRoughness.baseColorTextureCoordinate, uv0, uv1, uv2);
        alpha *= sampleMaterial(gTextures[texture], uv).a;
    }
    return alpha;
}
#endif
// clang-format on