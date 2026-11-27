#include "renderer/gpuScene.h"
#include "renderer/grid.h"
#include "renderer/taa.h"
#include "rasm/scene.h"
#include "rasm/camera.h"
#include "rasm/renderer.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/blendSort.h"
#include "renderer/environmentBake.h"
#include "renderer/export.h"
#include "renderer/geometryPool.h"
#include "renderer/lightClusters.h"
#include "renderer/animation.h"
#include "renderer/localShadows.h"
#include "renderer/picking.h"
#include "renderer/renderSpace.h"
#include "renderer/shadowCascades.h"
#include "renderer/uberMaterialTypes.h"

#include "glm/gtc/matrix_transform.hpp"
#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace rasm
{
    namespace
    {
        // Byte offsets in a view's visible draw buffer (getVisibleDrawLayout).
        struct VisibleDrawLayout
        {
            uint64_t meshletDraws = 0;
            uint64_t cullSlots = 0;
            uint64_t groupCounts = 0;
            uint64_t drawCounts = 0;
            uint64_t meshletDrawCounts = 0;
            uint64_t meshletArgs = 0;
            uint64_t groupMeshlets = 0;
            uint64_t chunks = 0;
            uint64_t chunkMasks = 0;
            uint64_t chunkSums = 0;
            uint64_t size = 0;
        };
    }

    namespace
    {
        constexpr uint32_t CULL_GROUP_SIZE = 64;        // numthreads of cull.slang
        constexpr uint32_t HIZ_GROUP_SIZE = 8;          // numthreads of hiz.slang, in x and y
        constexpr uint32_t LABEL_DEPTH_GROUP_SIZE = 64; // numthreads of labelDepth.slang
        constexpr uint32_t CULL_EARLY = 0;              // the cull phases of cull.slang
        constexpr uint32_t CULL_LATE = 1;
        constexpr uint32_t GHOST_BOX_VERTICES = 36;                  // a box's 12 triangles, debugGhosts.slang
        constexpr uint32_t MESHLET_RUN_SHIFT = 4;                    // the meshlet dispatches' argument: the phase, and the run above it (cull.slang)
        constexpr uint32_t MESHLET_ARGS_SIZE = 4 * sizeof(uint32_t); // a run's dispatch group counts and its chunk count
        constexpr uint32_t MESHLET_CHUNK_GROUP = 64;                 // chunks per sum of cull.slang's chunkSumsPtr; each run's chunks start at a multiple

        // A view's visible draw buffer holds the packed draws, then the meshlet draws, so that both are DrawInfos at
        // drawInfoPtr, then culling's scratch: a slot per draw and a count per group (at most one group per
        // CULL_GROUP_SIZE draws plus one per run), the visible draws per run, which the draws read as indirect counts,
        // the visible meshlet draws per run, the meshlet dispatches' group counts per run, a chunk and meshlet count per
        // group, per chunk its entry and its visible meshlets, and the visible meshlets of each 64 chunks (cull.slang).
        VisibleDrawLayout getVisibleDrawLayout(uint32_t capacity, uint32_t meshletCapacity, uint32_t chunkCapacity)
        {
            const uint64_t groups = capacity / CULL_GROUP_SIZE + gfx::MAX_DRAW_RUNS;
            VisibleDrawLayout layout;
            layout.meshletDraws = static_cast<uint64_t>(capacity) * sizeof(gfx::DrawInfo);
            layout.cullSlots = layout.meshletDraws + static_cast<uint64_t>(meshletCapacity) * sizeof(gfx::DrawInfo);
            layout.groupCounts = layout.cullSlots + static_cast<uint64_t>(capacity) * sizeof(uint32_t);
            layout.drawCounts = layout.groupCounts + groups * sizeof(uint32_t);
            layout.meshletDrawCounts = layout.drawCounts + gfx::MAX_DRAW_RUNS * sizeof(uint32_t);
            layout.meshletArgs = layout.meshletDrawCounts + gfx::MAX_DRAW_RUNS * sizeof(uint32_t);
            layout.groupMeshlets = layout.meshletArgs + static_cast<uint64_t>(gfx::MAX_DRAW_RUNS) * MESHLET_ARGS_SIZE;
            layout.chunks = layout.groupMeshlets + groups * 2 * sizeof(uint32_t);
            layout.chunkMasks = layout.chunks + static_cast<uint64_t>(chunkCapacity) * 4 * sizeof(uint32_t);
            layout.chunkSums = layout.chunkMasks + static_cast<uint64_t>(chunkCapacity) * 2 * sizeof(uint32_t);
            layout.size = layout.chunkSums + static_cast<uint64_t>(chunkCapacity / MESHLET_CHUNK_GROUP) * sizeof(uint32_t);
            return layout;
        }

        VisibleDrawLayout getVisibleDrawLayout(const SceneView &view, uint32_t frame)
        {
            return getVisibleDrawLayout(view.visibleCapacity[frame], view.meshletCapacity[frame], view.chunkCapacity[frame]);
        }

        // The path a view draws its meshlets by. AUTO takes the mesh shaders where the device has them and leaves meshlets off
        // elsewhere: measured on an RTX 3060, the mesh shaders cost about 1% where LODs already cut the triangles and save up
        // to 30% on dense meshes, while vertex pulling costs about 11% in the first case (P28). MESH_SHADERS asked for
        // explicitly falls back to vertex pulling.
        MeshletPath resolveMeshletPath(MeshletPath path, bool meshShaders)
        {
            if (path == MeshletPath::AUTO)
                return meshShaders ? MeshletPath::MESH_SHADERS : MeshletPath::OFF;
            if (path == MeshletPath::MESH_SHADERS && !meshShaders)
                return MeshletPath::VERTEX_PULLING;
            return path;
        }

        // An orthographic view's direction, or a perspective one's position: what the meshlet cone test looks from. In
        // render space a perspective camera is at the origin.
        glm::vec4 getCullViewPoint(const glm::mat4 &projection, const glm::mat4 &view)
        {
            const glm::mat4 world = glm::inverse(view);
            if (projection[3][3] == 1.0f)
                return glm::vec4(-glm::normalize(glm::vec3(world[2])), 0.0f);
            return glm::vec4(glm::vec3(world[3]), 1.0f);
        }

        void writeRenderOrigin(gfx::SceneData &sceneData, const glm::dvec3 &origin)
        {
            const SplitPosition split = splitPosition(origin);
            sceneData.originHigh = split.high;
            sceneData.originLow = split.low;
        }

        // The blend sort's keys (a uint2 each), then the draws it copies aside (shaders/passes/sort.slang).
        uint64_t getBlendSortBufferSize(uint32_t capacity)
        {
            return static_cast<uint64_t>(capacity) * (2 * sizeof(uint32_t) + sizeof(gfx::DrawInfo));
        }

        const DebugPipelines &getDebugPipelines(const ShadedPipelines &shaded, DebugView debugView)
        {
            return debugView == DebugView::OVERDRAW ? shaded.overdraw : shaded.debug;
        }

        // A shaded view's motion and jitter. The previous view-projection is the frame before's without jitter (this
        // one's on the view's first frame, so it has no motion). A view of a TAA pass (addTaaPass) moves its projection
        // by a sub-pixel offset each frame, the same as a translation of clip space by jitter (NDC) times w, so it
        // works for any projection; the motion and the sky take it out again. The view keeps the frame before's in world
        // space, in doubles, so that it can move it into this frame's render space: the origin moves with the camera.
        void writeViewMotion(SceneView &view, gfx::SceneData &sceneData, const glm::dmat4 &worldView, const glm::dvec3 &origin, uint32_t width, uint32_t height)
        {
            const glm::dmat4 viewProjection = glm::dmat4(sceneData.projection) * worldView;
            sceneData.previousViewProjection = toRenderMatrix(view.hasPrevious ? view.previousViewProjection : viewProjection, origin);
            view.previousViewProjection = viewProjection;
            view.hasPrevious = true;
            if (!view.jitter)
                return;
            const glm::vec2 jitter = getTaaJitter(view.jitterFrame++) * glm::vec2(2.0f / static_cast<float>(width), 2.0f / static_cast<float>(height));
            sceneData.jitter = jitter;
            sceneData.projection = glm::translate(glm::mat4(1.0f), glm::vec3(jitter, 0.0f)) * sceneData.projection;
            sceneData.textureLodBias = TAA_TEXTURE_LOD_BIAS;
        }

        bool hasRun(const SceneDrawList &drawList, AlphaMode alphaMode)
        {
            for (const DrawRun &run : drawList.runs)
            {
                if (run.alphaMode == alphaMode)
                    return true;
            }
            return false;
        }

        void destroyViewBuffer(RenderContext &ctx, BufferHandle &buffer)
        {
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
            buffer = {};
        }

        // An orthographic camera can see from a near plane at or behind itself.
        float getNearZ(const Camera &camera)
        {
            return camera.projection.type == CameraProjection::Type::PERSPECTIVE ? camera.projection.perspective.nearZ : camera.projection.orthographic.nearZ;
        }

        // The view and projection of each of the shadow's maps this frame. The view that reads them and the maps' own
        // views both call it, so the maps are drawn and read with the same matrices. A directional light's cascades are
        // fitted to the camera's view, and also give their splits; a spot light's map follows its cone; a point
        // light's cube has one projection for its faces, and the view of face. False, and no shadow, when the camera's near
        // plane has moved to a directional shadow's distance or beyond since addShadowPasses.
        bool getShadowMaps(const Light &light, const Camera &camera, const glm::mat4 &projection, const glm::dmat4 &view, const SceneShadow &shadow, uint32_t mapSize, uint32_t face, ShadowCascade *maps, float *splits)
        {
            if (light.type == LightType::SPOT)
            {
                maps[0] = {.view = getSpotShadowView(light.position, light.direction), .projection = getSpotShadowProjection(light.outerConeAngle, shadow.distance)};
                return true;
            }
            if (light.type == LightType::POINT)
            {
                maps[0] = {.view = getCubeFaceView(light.position, face), .projection = getCubeFaceProjection(shadow.distance)};
                return true;
            }
            const float nearZ = getNearZ(camera);
            if (!(shadow.distance > nearZ))
                return false;
            getShadowSplits(nearZ, shadow.distance, shadow.splitLambda, shadow.mapCount, splits);
            const glm::dmat4 world = glm::inverse(view);
            for (uint32_t i = 0; i < shadow.mapCount; ++i)
                maps[i] = fitShadowCascade(projection, world, light.direction, i == 0 ? nearZ : splits[i - 1], splits[i], mapSize, shadow.distance);
            return true;
        }

        bool holdsShadow(const SceneView &view, const SceneShadow &shadow)
        {
            return std::find(view.shadows, view.shadows + view.shadowCount, shadow) != view.shadows + view.shadowCount;
        }

        // Level 0 is half the pass's size, rounded up, and each level halves the one before, down to 1x1 (occlusionCulling.slang).
        uint32_t getHizLevelCount(uint32_t width, uint32_t height, uint32_t &texelCount)
        {
            uint32_t levelWidth = (width + 1) / 2;
            uint32_t levelHeight = (height + 1) / 2;
            uint32_t levelCount = 1;
            texelCount = levelWidth * levelHeight;
            while (levelWidth > 1 || levelHeight > 1)
            {
                levelWidth = (levelWidth + 1) / 2;
                levelHeight = (levelHeight + 1) / 2;
                texelCount += levelWidth * levelHeight;
                ++levelCount;
            }
            return levelCount;
        }

        // glTF: a transform with a negative determinant mirrors the mesh, so its front faces turn clockwise (D8).
        // det(T * R * S * M) = sx * sy * sz * det(M). The blend run is one run, so that it stays in one sorted order:
        // fragBlend drops the back faces of a single-sided material instead.
        CullMode getCullMode(const Material &material, const Transform &transform, const glm::mat4 &meshTransform, bool canMirror)
        {
            if (material.doubleSided || material.alphaMode == AlphaMode::BLEND || canMirror)
                return CullMode::NONE;
            const float determinant = transform.scale.x * transform.scale.y * transform.scale.z * glm::determinant(glm::mat3(meshTransform));
            return determinant < 0.0f ? CullMode::FRONT : CullMode::BACK;
        }

        // Primitives without a material use the entity's; getMaterial resolves an invalid one to the default material.
        const Material &getSubMeshMaterial(Renderer &engine, const Entity &entity, const SubMesh &subMesh)
        {
            return engine.getMaterial(subMesh.material.isValid() ? subMesh.material : entity.material);
        }

        bool texturesLoaded(Renderer &engine, const Entity &entity, const Mesh &mesh)
        {
            for (const auto &subMesh : mesh.subMeshes)
            {
                const Material &material = getSubMeshMaterial(engine, entity, subMesh);
                for (auto texture : material.textures)
                {
                    if (texture.isValid() && !engine.isLoaded(texture))
                        return false;
                }
                if (material.terrain.heights.isValid() && !engine.isLoaded(material.terrain.heights))
                    return false;
            }
            return true;
        }

        gfx::MaterialId getGfxMaterialId(MaterialType type)
        {
            switch (type)
            {
            case MaterialType::UNLIT:
                return gfx::MaterialId::UNLIT;
            case MaterialType::TERRAIN:
                return gfx::MaterialId::TERRAIN;
            case MaterialType::VECTOR:
                return gfx::MaterialId::VECTOR;
            case MaterialType::POINTS:
                return gfx::MaterialId::POINTS;
            case MaterialType::GLYPHS:
                return gfx::MaterialId::GLYPHS;
            default:
                return gfx::MaterialId::PBR;
            }
        }

        bool isIdentity(const TextureTransform &transform)
        {
            return transform.offset == glm::vec2(0.0f) && transform.rotation == 0.0f && transform.scale == glm::vec2(1.0f);
        }

        // KHR_texture_transform's translation * rotation * scale, as the rows of a 2x2 and an offset.
        gfx::TextureTransform toGfxTextureTransform(const TextureTransform &transform)
        {
            const float c = std::cos(transform.rotation);
            const float s = std::sin(transform.rotation);
            return {.matrix = glm::vec4(c * transform.scale.x, s * transform.scale.y, -s * transform.scale.x, c * transform.scale.y), .offset = transform.offset};
        }

        // What a row is in the point run (GpuScene::instancePoints): a point cloud chunk with a POINTS material, or a glyph
        // chunk with a GLYPHS material, whose shape is culled by back face or not (two-sided, or mirrored).
        uint8_t getPointRunKind(const MeshGeometry &geometry, MaterialType material, CullMode cullMode)
        {
            if (geometry.pointCount > 0 && material == MaterialType::POINTS)
                return POINT_RUN_POINTS;
            if (geometry.glyphCount > 0 && material == MaterialType::GLYPHS)
                return cullMode == CullMode::BACK ? POINT_RUN_GLYPHS : POINT_RUN_GLYPHS_TWO_SIDED;
            return POINT_RUN_NONE;
        }

        // The draw runs come in this order: OPAQUE then MASK, each by cull mode, then the point run, then the BLEND runs:
        // the weighted one, the additive one and the sorted one, last.
        uint32_t getRunKey(AlphaMode alphaMode, CullMode cullMode, bool points, BlendMethod blendMethod)
        {
            if (points)
                return static_cast<uint32_t>(AlphaMode::BLEND) * gfx::MAX_DRAW_RUNS - 1;
            if (alphaMode == AlphaMode::BLEND)
                return static_cast<uint32_t>(AlphaMode::BLEND) * gfx::MAX_DRAW_RUNS + (blendMethod == BlendMethod::WEIGHTED ? 0u : (blendMethod == BlendMethod::ADDITIVE ? 1u : 2u));
            return static_cast<uint32_t>(alphaMode) * gfx::MAX_DRAW_RUNS + static_cast<uint32_t>(cullMode);
        }

        // How a material's BLEND draws are drawn. A GLYPHS material's glyphs cannot be sorted, so they are weighted, and a
        // VECTOR material's layers keep their drawOrder, so they are sorted. Weighted and additive draws blend their
        // targets differently (independentBlend): without it they are sorted.
        BlendMethod getBlendMethod(const Material &material, bool independentBlend)
        {
            if (material.alphaMode != AlphaMode::BLEND || material.type == MaterialType::VECTOR || material.type == MaterialType::POINTS || !independentBlend)
                return BlendMethod::SORTED;
            if (material.type == MaterialType::GLYPHS)
                return BlendMethod::WEIGHTED;
            if (material.blendMethod == BlendMethod::WEIGHTED || material.blendMethod == BlendMethod::ADDITIVE)
                return material.blendMethod;
            return BlendMethod::SORTED;
        }

        // A scalar range as the point shader reads it: logarithms of its ends for a log range, whose ends must be above 0.
        // An adaptive point's largest diameter, in 1/16 pixels in the flags' high bits (PointData keeps its 40 bytes).
        uint32_t getPointMaxSizeBits(float maxSize)
        {
            const float sixteenths = std::clamp(std::isfinite(maxSize) ? maxSize : 1.0f, 1.0f, 4095.0f) * 16.0f;
            return static_cast<uint32_t>(std::lround(sixteenths)) << gfx::POINT_MAX_SIZE_SHIFT;
        }

        void writeScalarRange(const ScalarRange &range, gfx::PointData &points)
        {
            const bool log = range.log && range.min > 0.0f && range.max > 0.0f;
            if (range.log && !log)
                spdlog::warn("A material's scalar range is logarithmic with an end at or below 0 ({} to {}): it is drawn linear.", range.min, range.max);
            points.rangeMin = log ? std::log(range.min) : range.min;
            points.rangeMax = log ? std::log(range.max) : range.max;
            points.flags |= (log ? gfx::POINT_LOG : 0u) | (range.clamp ? gfx::POINT_CLAMP : 0u);
        }

        gfx::Material convertToGfxMaterial(Renderer &engine, const Material &mat)
        {
            gfx::Material gmat{};

            // base color (albedo) and its alpha
            for (uint32_t i = 0; i < 4; ++i)
                gmat.pbr.baseColorFactor[i] = mat.params[static_cast<uint32_t>(PbrParam::ALBEDO) + i];

            // glTF texCoord set per texture; the shader selects the matching interpolated UV set.
            auto texCoordOf = [](uint32_t texCoord) -> uint32_t
            { return texCoord < MAX_VERTEX_UV_SETS ? texCoord : 0u; };

            gmat.pbr.baseColorTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)]);
            gmat.pbr.baseColorTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::ALBEDO)]);
            gmat.pbr.metallicRoughnessTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::METALLIC)]);
            gmat.pbr.metallicRoughnessTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::METALLIC)]);

            gmat.pbr.metallicFactor = mat.params[static_cast<uint32_t>(PbrParam::METALLIC)];
            gmat.pbr.roughnessFactor = mat.params[static_cast<uint32_t>(PbrParam::ROUGHNESS)];

            // normal map
            gmat.normal.normalTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::NORMAL)]);
            gmat.normal.normalTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::NORMAL)]);
            gmat.normal.scale = mat.params[static_cast<uint32_t>(PbrParam::NORMAL_SCALE)];

            // occlusion map
            gmat.occlusion.occlusionTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::OCCLUSION)]);
            gmat.occlusion.occlusionTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::OCCLUSION)]);
            gmat.occlusion.strength = mat.params[static_cast<uint32_t>(PbrParam::OCCLUSION_STRENGTH)];

            // emissive map
            gmat.emissive.emissiveTextureIndex = engine.getBindlessTextureIndex(mat.textures[static_cast<uint32_t>(PbrSlot::EMISSIVE)]);
            gmat.emissive.emissiveTextureCoordinate = texCoordOf(mat.texCoords[static_cast<uint32_t>(PbrSlot::EMISSIVE)]);
            // KHR_materials_emissive_strength scales the factor, which may then pass 1.
            const float emissiveStrength = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_STRENGTH)];
            for (uint32_t i = 0; i < 3; ++i)
                gmat.emissive.emissiveFactor[i] = mat.params[static_cast<uint32_t>(PbrParam::EMISSIVE_FACTOR) + i] * emissiveStrength;

            return gmat;
        }

        // Most materials have no texture transform, so their shading skips the transforms and does not load them.
        gfx::MaterialData toMaterialData(Renderer &engine, const Material &mat, Format normalMapFormat, Format heightsFormat, uint32_t fontSlot, float fontRange)
        {
            gfx::MaterialData row{.material = convertToGfxMaterial(engine, mat),
                                  .materialId = getGfxMaterialId(mat.type),
                                  .alphaMode = mat.alphaMode,
                                  .alphaCutoff = mat.alphaCutoff,
                                  .flags = mat.doubleSided ? gfx::MATERIAL_DOUBLE_SIDED : 0u};
            const BlendMethod blendMethod = getBlendMethod(mat, engine.getRenderContext().hasIndependentBlend());
            if (blendMethod == BlendMethod::WEIGHTED)
                row.flags |= gfx::MATERIAL_WEIGHTED;
            if (blendMethod == BlendMethod::ADDITIVE)
                row.flags |= gfx::MATERIAL_ADDITIVE;
            const PbrSlot transformSlots[gfx::TEXTURE_TRANSFORMS] = {PbrSlot::ALBEDO, PbrSlot::METALLIC, PbrSlot::NORMAL, PbrSlot::OCCLUSION, PbrSlot::EMISSIVE};
            for (uint32_t i = 0; i < gfx::TEXTURE_TRANSFORMS; ++i)
            {
                const TextureTransform &transform = mat.textureTransforms[static_cast<uint32_t>(transformSlots[i])];
                row.textureTransforms[i] = toGfxTextureTransform(transform);
                if (!isIdentity(transform))
                    row.flags |= gfx::MATERIAL_TEXTURE_TRANSFORM;
            }
            // A two-channel format holds a normal map's x and y. The flag follows the texture, so a normal map that
            // updateMaterial replaces sets it again.
            if (normalMapFormat == Format::BC5_UNORM || normalMapFormat == Format::EAC_R11G11_UNORM)
                row.flags |= gfx::MATERIAL_NORMAL_XY;
            if (mat.type == MaterialType::TERRAIN)
            {
                const TerrainMaterial &terrain = mat.terrain;
                row.terrain = {.heights = engine.getBindlessTextureIndex(terrain.heights),
                               .heightKind = heightsFormat == Format::R32_SFLOAT ? gfx::TERRAIN_HEIGHTS_FLOAT : gfx::TERRAIN_HEIGHTS_UNORM16,
                               .heightScale = terrain.heightScale,
                               .heightOffset = terrain.heightOffset,
                               .minHeight = terrain.minHeight,
                               .skirt = terrain.skirt,
                               .sphereRadius = static_cast<float>(terrain.sphereRadius)};
            }
            if (mat.type == MaterialType::VECTOR)
            {
                const VectorMaterial &vector = mat.vector;
                const bool icon = mat.textures[static_cast<uint32_t>(PbrSlot::ALBEDO)].isValid();
                row.vector = {.color = vector.color,
                              .outlineColor = vector.outlineColor,
                              .width = vector.width,
                              .outlineWidth = vector.outlineWidth,
                              .dashLength = vector.dashLength,
                              .gapLength = vector.gapLength,
                              .flags = (vector.widthUnit == VectorUnit::METRES ? gfx::VECTOR_METRES : 0u) | (vector.facing == VectorFacing::GROUND ? gfx::VECTOR_GROUND : 0u) |
                                       (vector.depth == VectorDepth::OVERLAY ? gfx::VECTOR_OVERLAY : 0u) | (icon ? gfx::VECTOR_ICON : 0u),
                              .drawOrder = std::clamp(vector.drawOrder, -127, 127),
                              .font = INVALID_TEXTURE_INDEX};
                if (fontSlot != INVALID_TEXTURE_INDEX)
                {
                    row.vector.font = fontSlot;
                    row.vector.distanceRange = fontRange;
                    row.vector.flags |= gfx::VECTOR_TEXT;
                }
                // Data lines: the vertices' scalars through a colormap, which the row's PointData holds as a POINTS
                // material's does (lib/vector.slang reads it there).
                if (vector.colorMode == PointColorMode::SCALAR)
                {
                    row.points = {.color = glm::vec4(1.0f), .flags = gfx::POINT_SCALAR, .colormap = engine.getBindlessTextureIndex(vector.colormap)};
                    writeScalarRange(vector.range, row.points);
                }
            }
            // A glyph chunk is shaded as PBR, its base color times each glyph's color or its scalar through the colormap,
            // which the row's PointData holds (lib/glyphs.slang).
            if (mat.type == MaterialType::GLYPHS)
            {
                const GlyphMaterial &glyphs = mat.glyphs;
                row.points = {.color = glm::vec4(1.0f),
                              .flags = glyphs.colorMode == PointColorMode::SCALAR ? gfx::POINT_SCALAR : 0u,
                              .colormap = engine.getBindlessTextureIndex(glyphs.colormap)};
                writeScalarRange(glyphs.range, row.points);
            }
            if (mat.type == MaterialType::POINTS)
            {
                const PointMaterial &points = mat.points;
                const uint32_t shapes[] = {0u, gfx::POINT_ROUND, gfx::POINT_SPHERE};
                row.points = {.color = points.color,
                              .size = points.size,
                              .flags = (points.sizeUnit == VectorUnit::METRES ? gfx::POINT_METRES : 0u) | shapes[std::min(static_cast<uint32_t>(points.shape), 2u)] |
                                       (points.colorMode == PointColorMode::SCALAR ? gfx::POINT_SCALAR : 0u) | (points.sizeMode == PointSizeMode::ADAPTIVE ? gfx::POINT_ADAPTIVE : 0u) | getPointMaxSizeBits(points.maxSize),
                              .colormap = engine.getBindlessTextureIndex(points.colormap),
                              .density = std::max(points.density, 0.0f)};
                writeScalarRange(points.range, row.points);
            }
            return row;
        }
    }

    static_assert(MAX_SCENES <= 16, "EntityDrawState::scenes has one bit per scene");
    static_assert(2 * (static_cast<uint32_t>(CullMode::BACK) + 1) + 4 == gfx::MAX_DRAW_RUNS, "a draw list has a run per cull mode for OPAQUE and MASK, a point run and three BLEND runs");
    static_assert(static_cast<uint32_t>(gfx::TextureTransformSlot::EMISSIVE) + 1 == gfx::TEXTURE_TRANSFORMS, "every texture transform slot has a row");
    static_assert(static_cast<uint32_t>(BlendSortStep::WRITE) + 1 == BLEND_SORT_STEPS, "every sort step has a pipeline");

    // The pipelines that match the view's target: its sample count picks the set.
    // A target with a MOTION_FORMAT second color (one sample) draws motion too, so its pipelines have two colors.
    ShadedPipelines &getShadedPipelines(GpuScene &gpuScene, RenderContext &ctx, const SceneView &view)
    {
        const RenderTarget &target = ctx.getRenderTarget(view.renderTarget);
        if (target.colorCount == 2)
            return gpuScene.shaded[MOTION_VARIANT];
        return gpuScene.shaded[std::min(static_cast<uint32_t>(std::countr_zero(target.samples)), SAMPLE_VARIANTS - 1)];
    }

    // The define that makes a shaded pipeline write motion too, for a target with a MOTION_FORMAT second color.
    const char *getMotionDefine(RenderContext &ctx, const SceneView &view)
    {
        return ctx.getRenderTarget(view.renderTarget).colorCount == 2 ? "#define RASM_MOTION_VECTORS 1\n" : "";
    }

    uint64_t getSceneDataAddress(RenderContext &ctx, const GpuScene &gpuScene, uint32_t frame, uint32_t view)
    {
        return ctx.getBufferDeviceAddress(gpuScene.sceneData[frame]) + view * sizeof(gfx::SceneData);
    }

    // The planes of clip space (-w <= x, y <= w and 0 <= z <= w) are sums of rows of projection * view, in world space.
    // The infinite far plane of reversed-Z comes out as (0, 0, 0, nearZ), which every sphere passes.
    void getFrustumPlanes(const glm::mat4 &viewProjection, glm::vec4 planes[6])
    {
        const glm::mat4 rows = glm::transpose(viewProjection);
        planes[0] = rows[3] + rows[0];
        planes[1] = rows[3] - rows[0];
        planes[2] = rows[3] + rows[1];
        planes[3] = rows[3] - rows[1];
        planes[4] = rows[2];
        planes[5] = rows[3] - rows[2];
        for (uint32_t i = 0; i < 6; ++i)
        {
            const float length = glm::length(glm::vec3(planes[i]));
            if (length > 0.0f)
                planes[i] /= length;
        }
    }

    bool Renderer::createGpuScene()
    {
        // Material rows are indexed by handle, from 1, so the table has a row for each of maxMaterials materials and row 0.
        if (config.maxMaterials == 0 || config.maxMaterials == UINT32_MAX)
        {
            spdlog::error("RendererConfig::maxMaterials is {}; it must be 1 to {}.", config.maxMaterials, UINT32_MAX - 1);
            return false;
        }
        if (!gpuScene->instances.create(ctx, "Instance Table", sizeof(gfx::InstanceData), config.maxInstances) ||
            !gpuScene->materials.create(ctx, "Material Table", sizeof(gfx::MaterialData), config.maxMaterials + 1))
        {
            spdlog::error("Failed to create the GPU scene tables (RendererConfig::maxInstances = {}, maxMaterials = {}).", config.maxInstances, config.maxMaterials);
            return false;
        }
        if (config.meshletPath == MeshletPath::MESH_SHADERS && !ctx.hasMeshShaders())
            spdlog::warn("RendererConfig::meshletPath is MESH_SHADERS, and this GPU has no mesh shaders (VK_EXT_mesh_shader): meshlets are drawn by vertex pulling.");
        gpuScene->instanceRows.reset(config.maxInstances);
        gpuScene->instanceCullModes.assign(config.maxInstances, CullMode::NONE);
        gpuScene->instanceAlphaModes.assign(config.maxInstances, AlphaMode::NONE);
        gpuScene->instanceBlendMethods.assign(config.maxInstances, BlendMethod::SORTED);
        gpuScene->instancePoints.assign(config.maxInstances, 0);
        // A word of meshlet visibility bits per instance row on average: 32 meshlets. Each view with occlusion culling
        // has this many words (createSceneView); a row that finds none left has no bits, and is only culled less.
        gpuScene->meshletWords.reset(config.maxInstances);
        gpuScene->labelSlots.reset(MAX_LABELS);
        gpuScene->labelStamps.assign(MAX_LABELS, 0);
        gpuScene->instanceMeshlets.assign(config.maxInstances, 0);
        gpuScene->instanceMeshletWords.assign(config.maxInstances, -1);
        gpuScene->instanceMeshletWordCounts.assign(config.maxInstances, 0);
        picks->owners.reset(config.maxInstances);

        // The CPU rewrites each view's SceneData every frame, so it stays host-visible. The picks' come after the views'.
        for (auto &buffer : gpuScene->sceneData)
        {
            buffer = ctx.createBuffer({.name = "Scene Data Buffer", .usage = BufferUsage::STORAGE, .size = (MAX_SCENE_VIEWS + MAX_FRAME_PICKS) * sizeof(gfx::SceneData), .hostVisible = true});
            if (!buffer.isValid())
            {
                spdlog::error("Failed to create the scene data buffer.");
                return false;
            }
        }

        // The frame's visible draws and triangles (low, high), which updateFrameStats reads back once the slot's fence passed.
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            gpuScene->cullStats[i] = ctx.createBuffer({.name = "Cull Stats Buffer " + std::to_string(i), .usage = BufferUsage::STORAGE, .size = CULL_STATS_WORDS * sizeof(uint32_t), .hostVisible = true, .hostReadable = true});
            if (!gpuScene->cullStats[i].isValid())
            {
                spdlog::error("Failed to create the culling resources.");
                return false;
            }
        }
        return true;
    }

    // Runs once per frame, after the fence wait for this frame's slot, so the slot's buffers are free to rewrite.
    // Its cost follows what changed: only the scenes that hold a changed entity rebuild their draw lists (P7).
    void Renderer::syncGpuScene()
    {
        ZoneScoped;

        // Motion: entities that moved before the last sync and not since stopped, so their rows' previousWorld becomes
        // their world. Those that moved since the last sync draw their new matrix in this frame, which the next frame's
        // previousWorld is.
        // destroyEntity takes a destroyed entity off both lists, so they hold live entities only.
        const uint32_t window = gpuScene->syncCount++;
        for (EntityHandle entity : gpuScene->settlingEntities)
        {
            if (gpuScene->entities[entity.index].movedSync != window)
                writeInstanceRows(entity, false);
        }
        for (EntityHandle entity : gpuScene->movedEntities)
            gpuScene->entities[entity.index].syncedTransform = registry.entities[entity.index].transform;
        // A swap, not a move: both lists keep their capacity, so a frame that moves entities allocates nothing.
        std::swap(gpuScene->settlingEntities, gpuScene->movedEntities);
        gpuScene->movedEntities.clear();

        // A glyph chunk's shape: a ready mesh whose first sub-mesh has triangles.
        auto isGlyphShape = [this](MeshHandle shape)
        {
            if (!isLive(shape) || registry.meshes[shape.index].state != MeshState::READY || registry.meshes[shape.index].subMeshes.empty())
                return false;
            const MeshGeometry &geometry = registry.meshes[shape.index].subMeshes[0].geometry;
            return geometry.vertexCount > 0 && geometry.indexCount > 0 && geometry.pointCount == 0 && geometry.glyphCount == 0;
        };
        auto &waitingForMesh = gpuScene->entitiesWaitingForMesh;
        for (size_t i = 0; i < waitingForMesh.size();)
        {
            const EntityHandle entity = waitingForMesh[i];
            const bool alive = isLive(entity);
            const Entity &entityRef = registry.entities[entity.index];
            // A glyph chunk also waits for its shape.
            const MeshHandle shape = alive && isLive(entityRef.mesh) ? registry.meshes[entityRef.mesh.index].glyphShape : MeshHandle{};
            if (alive && isLive(entityRef.mesh) && (registry.meshes[entityRef.mesh.index].state == MeshState::LOADING || (isLive(shape) && registry.meshes[shape.index].state == MeshState::LOADING)))
            {
                ++i;
                continue;
            }

            // A destroyed entity, or one whose mesh was destroyed (destroyMesh cleared it), just leaves the list. Not
            // isLoaded: a ready glTF mesh still fills its textures over frames, and the entity waits for them below.
            const bool hasMesh = alive && entityRef.mesh.isValid();
            if (hasMesh && (!isLive(entityRef.mesh) || registry.meshes[entityRef.mesh.index].state != MeshState::READY))
            {
                spdlog::warn("Entity {} is not drawn: its mesh failed to load.", entity.index);
            }
            else if (hasMesh && registry.meshes[entityRef.mesh.index].subMeshes[0].geometry.glyphCount > 0 && !isGlyphShape(shape))
            {
                spdlog::warn("Entity {} is not drawn: its glyph chunk's shape is not a live mesh with vertices and indices.", entity.index);
            }
            else if (hasMesh)
            {
                const Mesh &mesh = registry.meshes[entityRef.mesh.index];
                const auto rowCount = static_cast<uint32_t>(mesh.subMeshes.size());
                uint64_t firstRow = 0;
                if (!gpuScene->instanceRows.allocate(rowCount, firstRow))
                {
                    spdlog::error("The instance table is full: entity {} needs {} rows ({}/{} in use, the largest free range {}). RendererConfig::maxInstances sets its size.",
                                  entity.index, rowCount, gpuScene->instanceRows.used(), gpuScene->instances.capacity(), gpuScene->instanceRows.largestFree());
                }
                else
                {
                    for (const auto &subMesh : mesh.subMeshes)
                    {
                        if ((subMesh.geometry.vertexCount == 0 && subMesh.geometry.pointCount == 0 && subMesh.geometry.glyphCount == 0) || subMesh.geometry.indexCount == 0)
                            spdlog::warn("Skipping a mesh of entity {}: it has {} vertices and {} indices, and drawing needs both.",
                                         entity.index, subMesh.geometry.vertexCount, subMesh.geometry.indexCount);
                        if (subMesh.geometry.pointCount > 0 && getSubMeshMaterial(*this, entityRef, subMesh).type != MaterialType::POINTS)
                            spdlog::warn("Entity {} draws a point cloud chunk without a POINTS material: its points are not drawn.", entity.index);
                        if (subMesh.geometry.glyphCount > 0 && getSubMeshMaterial(*this, entityRef, subMesh).type != MaterialType::GLYPHS)
                            spdlog::warn("Entity {} draws a glyph chunk without a GLYPHS material: its glyphs are not drawn.", entity.index);
                    }

                    // The rows go back when the entity or its mesh is destroyed (releaseEntityRows).
                    if (gpuScene->entities.size() <= entity.index)
                        gpuScene->entities.resize(entity.index + 1);
                    EntityDrawState &state = gpuScene->entities[entity.index];
                    state.firstInstance = static_cast<uint32_t>(firstRow);
                    state.instanceCount = rowCount;
                    picks->owners.assign(state.firstInstance, rowCount, entity, entityRef.mesh);
                    state.waitingForTextures = !texturesLoaded(*this, entityRef, mesh);
                    for (uint32_t r = 0; r < rowCount; ++r)
                    {
                        gpuScene->instanceCullModes[firstRow + r] = CullMode::NONE;
                        gpuScene->instanceAlphaModes[firstRow + r] = AlphaMode::NONE;
                        gpuScene->instanceBlendMethods[firstRow + r] = BlendMethod::SORTED;
                        gpuScene->instancePoints[firstRow + r] = 0;
                        // The bits of the LOD with the most meshlets, so every LOD's fit.
                        gpuScene->instanceMeshlets[firstRow + r] = mesh.subMeshes[r].geometry.maxLodMeshlets;
                        const auto words = (mesh.subMeshes[r].geometry.maxLodMeshlets + 31) / 32;
                        uint64_t firstWord = 0;
                        const bool bits = words > 0 && gpuScene->meshletWords.allocate(words, firstWord);
                        if (words > 0 && !bits && !gpuScene->meshletWordsFullLogged)
                        {
                            spdlog::warn("The meshlet visibility bits are full ({} words, RendererConfig::maxInstances): entity {}'s meshlets are not occlusion culled.",
                                         gpuScene->meshletWords.capacity(), entity.index);
                            gpuScene->meshletWordsFullLogged = true;
                        }
                        gpuScene->instanceMeshletWords[firstRow + r] = bits ? static_cast<int32_t>(firstWord) : -1;
                        gpuScene->instanceMeshletWordCounts[firstRow + r] = bits ? words : 0;
                    }
                    // Its labels' slots: each view keeps their visibility by slot.
                    state.firstLabel = -1;
                    state.labelCount = 0;
                    uint64_t firstLabel = 0;
                    const auto labelCount = static_cast<uint32_t>(mesh.labels.size());
                    if (labelCount > 0 && gpuScene->labelSlots.allocate(labelCount, firstLabel))
                    {
                        state.firstLabel = static_cast<int32_t>(firstLabel);
                        state.labelCount = labelCount;
                        for (uint32_t l = 0; l < labelCount; ++l)
                            ++gpuScene->labelStamps[firstLabel + l];
                    }
                    else if (labelCount > 0 && !gpuScene->labelSlotsFullLogged)
                    {
                        spdlog::warn("The label slots are full (MAX_LABELS, {}): entity {}'s {} labels are not drawn.", MAX_LABELS, entity.index, labelCount);
                        gpuScene->labelSlotsFullLogged = true;
                    }
                    state.syncedTransform = entityRef.transform;
                    writeInstanceRows(entity, false);
                    startPendingAnimation(entity);
                    poseAtRest(entity, gpuScene->syncCount);
                    if (state.waitingForTextures)
                        gpuScene->entitiesWaitingForTextures.push_back(entity);
                    else
                        markDrawListsDirty(entity);
                }
            }

            waitingForMesh[i] = waitingForMesh.back();
            waitingForMesh.pop_back();
        }

        // An entity is drawn only once the textures of all its sub-meshes are loaded, so nothing is drawn half-loaded.
        if (gpuScene->texturesFinished)
        {
            gpuScene->texturesFinished = false;
            auto &waitingForTextures = gpuScene->entitiesWaitingForTextures;
            for (size_t i = 0; i < waitingForTextures.size();)
            {
                const EntityHandle entity = waitingForTextures[i];
                const Entity &entityRef = registry.entities[entity.index];
                // Destroyed, or its rows released with its mesh: it waits no more.
                if (!isLive(entity) || gpuScene->entities[entity.index].instanceCount == 0 || !gpuScene->entities[entity.index].waitingForTextures)
                {
                    waitingForTextures[i] = waitingForTextures.back();
                    waitingForTextures.pop_back();
                    continue;
                }
                if (!texturesLoaded(*this, entityRef, registry.meshes[entityRef.mesh.index]))
                {
                    ++i;
                    continue;
                }

                gpuScene->entities[entity.index].waitingForTextures = false;
                markDrawListsDirty(entity);
                waitingForTextures[i] = waitingForTextures.back();
                waitingForTextures.pop_back();
            }
        }

        for (uint32_t i = 0; i < MAX_SCENES && i < registry.scenes.size(); ++i)
        {
            if (gpuScene->drawLists[i].rebuild)
                rebuildDrawList(registry.scenes[i].handle);
        }

        const auto frame = static_cast<uint32_t>(frameCount);
        syncGpuAnimation();
        picks->owners.sync(frame);
        gpuScene->instances.upload(ctx, frame);
        gpuScene->materials.upload(ctx, frame);
        for (auto &drawList : gpuScene->drawLists)
            uploadDrawList(drawList);
        syncGpuLights();
    }

    // Its rows are allocated once the mesh is ready (syncGpuScene).
    void Renderer::queueInstanceRows(EntityHandle entity)
    {
        gpuScene->entitiesWaitingForMesh.push_back(entity);
    }

    // A row's previousWorld is the matrix the frame before drew: the entity's matrix at the last sync (syncedTransform).
    // A write that moves the entity (moved) puts it on movedEntities, so the next sync makes the new matrix the synced
    // one, and the sync after writes the rows again with previousWorld = world, so a stopped entity has no motion.
    void Renderer::writeInstanceRows(EntityHandle entity, bool moved)
    {
        if (entity.index >= gpuScene->entities.size())
            return;
        EntityDrawState &state = gpuScene->entities[entity.index];
        if (state.instanceCount == 0)
            return;
        if (moved && state.movedSync != gpuScene->syncCount)
        {
            state.movedSync = gpuScene->syncCount;
            gpuScene->movedEntities.push_back(entity);
        }

        const Entity &entityRef = registry.entities[entity.index];
        const Mesh &mesh = registry.meshes[entityRef.mesh.index];
        // In doubles up to the row, which keeps the translation as a high and a low float (writeWorldRows).
        const glm::dmat4 entityMatrix = entityRef.transform.toMatrix();
        const glm::dmat4 syncedMatrix = state.syncedTransform.toMatrix();

        for (uint32_t i = 0; i < state.instanceCount; ++i)
        {
            const SubMesh &subMesh = mesh.subMeshes[i];
            const glm::dmat4 model = entityMatrix * glm::dmat4(subMesh.transform);
            gfx::InstanceData row{.normalMatrix = glm::transpose(glm::inverse(glm::mat3(glm::dmat3(model)))),
                                  .geometryIndex = subMesh.geometry.geometryIndex,
                                  .materialIndex = static_cast<uint32_t>(getSubMeshMaterial(*this, entityRef, subMesh).handle.index),
                                  .animationRow = getAnimationRow(entity, i),
                                  .meshletVisibility = gpuScene->instanceMeshletWords[state.firstInstance + i]};
            // The word meshlets would use: a point chunk's LOD, a glyph chunk's shape row, a vector mesh's first label slot.
            if (subMesh.geometry.pointCount > 0)
                row.meshletVisibility = state.pointLod;
            else if (subMesh.geometry.glyphCount > 0)
                row.meshletVisibility = isLive(mesh.glyphShape) ? static_cast<int32_t>(registry.meshes[mesh.glyphShape.index].subMeshes[0].geometry.geometryIndex) : -1;
            else if (state.labelCount > 0)
                row.meshletVisibility = state.firstLabel;
            writeWorldRows(model, row.world, row.translationLow);
            writePreviousRows(syncedMatrix * glm::dmat4(subMesh.transform), model, row.previousWorld);
            gpuScene->instances.setRow(state.firstInstance + i, &row);
        }

        // An animated entity's matrix also goes to its row of the animated entity table.
        writeAnimationRow(entity);
        // A transform can start or stop mirroring the entity (D8).
        updateDrawModes(entity);
    }

    // A changed cull mode or alpha mode moves the entity's draws to another run, so its scenes rebuild their draw lists.
    void Renderer::updateDrawModes(EntityHandle entity)
    {
        const EntityDrawState &state = gpuScene->entities[entity.index];
        const Entity &entityRef = registry.entities[entity.index];
        const Mesh &mesh = registry.meshes[entityRef.mesh.index];

        bool changed = false;
        for (uint32_t i = 0; i < state.instanceCount; ++i)
        {
            const SubMesh &subMesh = mesh.subMeshes[i];
            const Material &material = getSubMeshMaterial(*this, entityRef, subMesh);
            const CullMode cullMode = getCullMode(material, entityRef.transform, subMesh.transform, animationCanMirror(entityRef.mesh, i));
            CullMode &storedCullMode = gpuScene->instanceCullModes[state.firstInstance + i];
            AlphaMode &storedAlphaMode = gpuScene->instanceAlphaModes[state.firstInstance + i];
            // A point cloud chunk is drawn only with a POINTS material, by the point run, which is not culled by face. A
            // glyph chunk with a GLYPHS material is in the point run too, and drawn from the glyph list (glyphCull.cpp).
            const auto points = getPointRunKind(subMesh.geometry, material.type, cullMode);
            const CullMode wantedCullMode = points != POINT_RUN_NONE ? CullMode::NONE : cullMode;
            uint8_t &storedPoints = gpuScene->instancePoints[state.firstInstance + i];
            BlendMethod &storedBlendMethod = gpuScene->instanceBlendMethods[state.firstInstance + i];
            const BlendMethod blendMethod = getBlendMethod(material, ctx.hasIndependentBlend());
            if (!ctx.hasIndependentBlend() && getBlendMethod(material, true) != BlendMethod::SORTED && !gpuScene->blendMethodLogged)
            {
                spdlog::warn("This GPU has no independentBlend feature: weighted and additive materials are drawn sorted.");
                gpuScene->blendMethodLogged = true;
            }
            changed = changed || storedCullMode != wantedCullMode || storedAlphaMode != material.alphaMode || storedPoints != points || storedBlendMethod != blendMethod;
            storedCullMode = wantedCullMode;
            storedAlphaMode = material.alphaMode;
            storedPoints = points;
            storedBlendMethod = blendMethod;
        }

        // An entity that waits for textures is in no draw list yet.
        if (changed && !state.waitingForTextures)
            markDrawListsDirty(entity);
    }

    // updateMaterial keeps the material's row index, so no instance row changes. Its instances may change cull mode,
    // and an entity that got a texture that still loads leaves the draw lists until the texture finishes.
    void Renderer::refreshEntitiesAfterMaterialChange()
    {
        for (size_t i = 0; i < gpuScene->entities.size(); ++i)
        {
            EntityDrawState &state = gpuScene->entities[i];
            if (state.instanceCount == 0)
                continue;

            const Entity &entityRef = registry.entities[i];
            updateDrawModes(entityRef.handle);
            if (!state.waitingForTextures && !texturesLoaded(*this, entityRef, registry.meshes[entityRef.mesh.index]))
            {
                state.waitingForTextures = true;
                gpuScene->entitiesWaitingForTextures.push_back(entityRef.handle);
                markDrawListsDirty(entityRef.handle);
            }
        }
    }

    void Renderer::writeMaterialRow(MaterialHandle material)
    {
        const Material &materialRef = registry.materials[material.index];
        const TextureHandle normalMap = materialRef.textures[static_cast<uint32_t>(PbrSlot::NORMAL)];
        const Format normalMapFormat = normalMap.isValid() ? ctx.getTextureDesc(normalMap).format : Format::UNKNOWN;
        const TextureHandle heights = materialRef.terrain.heights;
        const Format heightsFormat = heights.isValid() ? ctx.getTextureDesc(heights).format : Format::UNKNOWN;
        uint32_t fontSlot = INVALID_TEXTURE_INDEX;
        float fontRange = 0.0f;
        if (materialRef.type == MaterialType::VECTOR && isLive(materialRef.vector.font))
        {
            const Font &font = registry.fonts[materialRef.vector.font.index];
            fontSlot = getBindlessTextureIndex(font.atlas);
            fontRange = font.distanceRange;
        }
        const gfx::MaterialData row = toMaterialData(*this, materialRef, normalMapFormat, heightsFormat, fontSlot, fontRange);
        gpuScene->materials.setRow(static_cast<uint32_t>(material.index), &row);
    }

    // The rows are free for another entity at once: each frame slot has its own copy of the table, written only after its
    // fence, and the draw lists that name them are rebuilt first.
    void Renderer::releaseEntityRows(EntityHandle entity)
    {
        if (entity.index >= gpuScene->entities.size())
            return;
        EntityDrawState &state = gpuScene->entities[entity.index];
        if (state.instanceCount > 0)
        {
            markDrawListsDirty(entity);
            gpuScene->instanceRows.free(state.firstInstance, state.instanceCount);
            // The next row to take these words finds this one's bits: a hint the late pass corrects.
            for (uint32_t row = state.firstInstance; row < state.firstInstance + state.instanceCount; ++row)
            {
                if (gpuScene->instanceMeshletWords[row] >= 0)
                    gpuScene->meshletWords.free(static_cast<uint64_t>(gpuScene->instanceMeshletWords[row]), gpuScene->instanceMeshletWordCounts[row]);
                gpuScene->instanceMeshletWords[row] = -1;
                gpuScene->instanceMeshletWordCounts[row] = 0;
            }
        }
        if (state.labelCount > 0)
            gpuScene->labelSlots.free(static_cast<uint64_t>(state.firstLabel), state.labelCount);
        state.firstLabel = -1;
        state.labelCount = 0;
        state.firstInstance = 0;
        state.instanceCount = 0;
        state.waitingForTextures = false;
        releaseEntityAnimation(entity);
    }

    bool Renderer::addEntityToDrawList(SceneHandle scene, EntityHandle entity)
    {
        if (gpuScene->entities.size() <= entity.index)
            gpuScene->entities.resize(entity.index + 1);
        EntityDrawState &state = gpuScene->entities[entity.index];
        const auto bit = static_cast<uint16_t>(1u << scene.index);
        if ((state.scenes & bit) != 0)
            return false;
        state.scenes |= bit;

        // An entity whose mesh or textures still load joins the draw list when they finish (syncGpuScene).
        if (state.instanceCount > 0 && !state.waitingForTextures)
            gpuScene->drawLists[scene.index].rebuild = true;
        return true;
    }

    void Renderer::markDrawListsDirty(EntityHandle entity)
    {
        const uint16_t scenes = gpuScene->entities[entity.index].scenes;
        for (uint32_t i = 0; i < MAX_SCENES; ++i)
        {
            if ((scenes & (1u << i)) != 0)
                gpuScene->drawLists[i].rebuild = true;
        }
    }

    // Removing an entity only clears its scene bit, so the list is compacted where it is walked anyway: an entry stays
    // while its entity lives and is in the scene, once (removed and added again, it may be there twice).
    void Renderer::compactSceneEntities(SceneHandle scene)
    {
        const auto bit = static_cast<uint16_t>(1u << scene.index);
        const uint32_t stamp = ++gpuScene->rebuildStamp;
        std::erase_if(registry.scenes[scene.index].entities, [this, bit, stamp](const EntityHandle &entity)
                      {
                          if (!isLive(entity) || entity.index >= gpuScene->entities.size())
                              return true;
                          EntityDrawState &state = gpuScene->entities[entity.index];
                          if ((state.scenes & bit) == 0 || state.listStamp == stamp)
                              return true;
                          state.listStamp = stamp;
                          return false; });
    }

    void Renderer::rebuildDrawList(SceneHandle scene)
    {
        ZoneScoped;

        auto &drawList = gpuScene->drawLists[scene.index];
        drawList.rebuild = false;
        drawList.draws.clear();
        drawList.runs.clear();
        drawList.labelEntities.clear();
        drawList.triangleCount = 0;
        drawList.glyphChunks = 0;
        drawList.glyphBatches = 0;
        drawList.glyphCullMode = CullMode::BACK;
        drawList.weightedGlyphChunks = 0;

        compactSceneEntities(scene);
        for (auto entity : registry.scenes[scene.index].entities)
        {
            const EntityDrawState &state = gpuScene->entities[entity.index];
            if (state.instanceCount == 0 || state.waitingForTextures)
                continue;

            const Mesh &mesh = registry.meshes[registry.entities[entity.index].mesh.index];
            if (state.labelCount > 0)
                drawList.labelEntities.push_back(entity);
            for (uint32_t i = 0; i < state.instanceCount; ++i)
            {
                const MeshGeometry &geometry = mesh.subMeshes[i].geometry;
                const uint8_t kind = gpuScene->instancePoints[state.firstInstance + i];
                const bool points = kind != POINT_RUN_NONE;
                if ((geometry.vertexCount == 0 && !points) || geometry.indexCount == 0)
                    continue;
                // The glyph cull's batches, and the face culling of the one glyph draw: by back face only when every
                // chunk's shape is single-sided and not mirrored.
                const bool glyphs = kind == POINT_RUN_GLYPHS || kind == POINT_RUN_GLYPHS_TWO_SIDED;
                if (glyphs)
                {
                    ++drawList.glyphChunks;
                    drawList.glyphBatches += (geometry.glyphCount + gfx::GLYPH_BATCH_GLYPHS - 1) / gfx::GLYPH_BATCH_GLYPHS;
                    if (kind == POINT_RUN_GLYPHS_TWO_SIDED)
                        drawList.glyphCullMode = CullMode::NONE;
                    if (gpuScene->instanceBlendMethods[state.firstInstance + i] == BlendMethod::WEIGHTED)
                        ++drawList.weightedGlyphChunks;
                }
                // Indices are local to their sub-mesh; vertexOffset moves them to its place in the geometry pool. A glyph
                // chunk's draw has no instance: only its glyphs are drawn, so a view drawn unculled draws nothing of it.
                drawList.draws.push_back({.indexCount = geometry.indexCount,
                                          .instanceCount = glyphs ? 0u : 1u,
                                          .firstIndex = geometry.firstIndex,
                                          .vertexOffset = static_cast<int32_t>(geometry.firstVertex),
                                          .firstInstance = state.firstInstance + i});
                drawList.triangleCount += glyphs ? 0 : geometry.indexCount / 3;
            }
        }

        // drawSceneView issues one indirect draw per run of draws with the same alpha mode and cull mode, and
        // drawTransparent one for the blend run, which comes last.
        const auto &cullModes = gpuScene->instanceCullModes;
        const auto &alphaModes = gpuScene->instanceAlphaModes;
        const auto &points = gpuScene->instancePoints;
        const auto &blendMethods = gpuScene->instanceBlendMethods;
        std::stable_sort(drawList.draws.begin(), drawList.draws.end(), [&](const gfx::DrawInfo &a, const gfx::DrawInfo &b)
                         { return getRunKey(alphaModes[a.firstInstance], cullModes[a.firstInstance], points[a.firstInstance] != 0, blendMethods[a.firstInstance]) <
                                  getRunKey(alphaModes[b.firstInstance], cullModes[b.firstInstance], points[b.firstInstance] != 0, blendMethods[b.firstInstance]); });
        for (uint32_t i = 0; i < drawList.draws.size(); ++i)
        {
            const bool pointDraw = points[drawList.draws[i].firstInstance] != 0;
            const AlphaMode alphaMode = pointDraw ? AlphaMode::NONE : alphaModes[drawList.draws[i].firstInstance];
            const CullMode cullMode = cullModes[drawList.draws[i].firstInstance];
            const BlendMethod blendMethod = pointDraw ? BlendMethod::SORTED : blendMethods[drawList.draws[i].firstInstance];
            const DrawRun *last = drawList.runs.empty() ? nullptr : &drawList.runs.back();
            if (last == nullptr || last->alphaMode != alphaMode || last->cullMode != cullMode || last->points != pointDraw || last->blendMethod != blendMethod)
                drawList.runs.push_back({.alphaMode = alphaMode, .cullMode = cullMode, .points = pointDraw, .blendMethod = blendMethod, .first = i, .count = 0});
            DrawRun &run = drawList.runs.back();
            ++run.count;
            // What the run's draws could take as meshlets, at most: prepareSceneView sizes the meshlet draws by it.
            const uint32_t meshlets = gpuScene->instanceMeshlets[drawList.draws[i].firstInstance];
            if (meshlets > 0)
            {
                ++run.meshletDraws;
                run.meshlets += meshlets;
                run.chunks += (meshlets + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE;
            }
        }

        drawList.staleSlots = static_cast<uint8_t>((1u << MAX_FRAMES_IN_FLIGHT) - 1);
    }

    void Renderer::uploadDrawList(SceneDrawList &drawList)
    {
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto bit = static_cast<uint8_t>(1u << frame);
        if ((drawList.staleSlots & bit) == 0 || drawList.draws.empty())
            return;

        BufferHandle &buffer = drawList.buffers[frame];
        uint32_t &capacity = drawList.bufferCapacity[frame];
        const auto drawCount = static_cast<uint32_t>(drawList.draws.size());
        if (capacity < drawCount)
        {
            // Only this slot's last frame used the old buffer, and the fence wait covered it.
            if (buffer.isValid())
                ctx.destroyBuffer(buffer);
            capacity = std::max(drawCount, capacity * 2);
            buffer = ctx.createBuffer({.name = "Draw Info Buffer", .usage = BufferUsage::INDIRECT, .size = static_cast<uint64_t>(capacity) * sizeof(gfx::DrawInfo)});
            if (!buffer.isValid())
            {
                spdlog::error("Failed to create a draw info buffer of {} draws.", capacity);
                capacity = 0;
                return;
            }
        }

        if (ctx.fillBuffer(buffer, drawList.draws.data(), drawList.draws.size() * sizeof(gfx::DrawInfo), 0))
            drawList.staleSlots &= static_cast<uint8_t>(~bit);
    }

    // The scene in passes of the graph: culling, the draws, and with occlusion culling the Hi-Z build, the late culling
    // and the late draws. They draw into renderTarget, so later passes on that target load what they drew. A
    // depth-only target gets only depth (passes named "Shadow ...", or "Cascade i ...", "Spot ..." and "Face i ..." for
    // a shadow's maps). Each view (scene, camera, target, face, shadows) culls into buffers of its own, kept from frame
    // to frame while a graph holds the view, which its passes declare: per-slot buffers by both slots, so their handles
    // hold for every frame. The cull stats are not declared: the views only add to them atomically, and only the CPU
    // reads them, after the slot's fence, so views need no order between them. The draws read the shadows' maps, so
    // they run after the passes that drew them.
    bool Renderer::addScenePasses(RenderGraph &graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget, const SceneShadow *shadows, uint32_t shadowCount,
                                  const RenderTargetHandle *sampled, uint32_t sampledCount)
    {
        if (!isLive(camera))
        {
            spdlog::error("addScenePasses needs a valid camera handle.");
            return false;
        }
        if (!isLive(scene))
        {
            spdlog::error("addScenePasses needs a valid scene handle.");
            return false;
        }
        // The pipeline's formats must match the attachments' for the draw to be defined.
        auto isDepthOnly = [this](const RenderTargetHandle &target)
        {
            const auto &targetData = ctx.getRenderTarget(target);
            return targetData.colorCount == 0 && targetData.depthFormat == DEPTH_FORMAT;
        };
        // The scene's radiance is unbounded, so a color target is HDR, and a tone-map pass reads it.
        if (!renderTarget.isValid())
        {
            spdlog::error("addScenePasses needs a valid render target.");
            return false;
        }
        const auto &renderTargetData = ctx.getRenderTarget(renderTarget);
        const bool depthOnly = isDepthOnly(renderTarget);
        // A second color, of MOTION_FORMAT, takes each pixel's motion for TAA; with one sample only, since TAA and MSAA
        // exclude each other.
        const bool motion = renderTargetData.colorCount == 2 && renderTargetData.colorFormats[1] == MOTION_FORMAT && renderTargetData.samples == 1;
        const bool color = (renderTargetData.colorCount == 1 || motion) && renderTargetData.colorFormats[0] == SCENE_COLOR_FORMAT && renderTargetData.depthFormat == DEPTH_FORMAT;
        if ((!depthOnly && !color) || ctx.getRenderTargetDesc(renderTarget).cube)
        {
            spdlog::error("addScenePasses needs a render target with a SCENE_COLOR_FORMAT color, a MOTION_FORMAT color only with one sample, and D32_SFLOAT depth, or D32_SFLOAT depth only, not a cube.");
            return false;
        }
        if (shadowCount > 0 && depthOnly)
        {
            spdlog::error("addScenePasses draws a depth-only target without shading, so it takes no shadow.");
            return false;
        }
        if (shadowCount > MAX_VIEW_SHADOWS || (shadowCount > 0 && shadows == nullptr))
        {
            spdlog::error("addScenePasses takes at most {} shadows.", MAX_VIEW_SHADOWS);
            return false;
        }
        // The view reads the maps as fitted to its camera, so they must be drawn from the same camera.
        for (uint32_t s = 0; s < shadowCount; ++s)
        {
            const SceneShadow &shadow = shadows[s];
            bool drawn = shadow.mapCount > 0 && shadow.mapCount <= MAX_SHADOW_CASCADES;
            for (uint32_t i = 0; drawn && i < shadow.mapCount; ++i)
            {
                drawn = false;
                for (uint32_t heldView : graph.sceneViews)
                {
                    const SceneView &held = gpuScene->views[heldView];
                    drawn = drawn || (held.scene == scene && held.camera == camera && held.renderTarget == shadow.maps[i] && held.depthOnly && held.shadows[0] == shadow);
                }
            }
            if (!drawn)
            {
                spdlog::error("addScenePasses needs shadows that addShadowPasses made for the same scene and camera in this graph.");
                return false;
            }
            for (uint32_t other = 0; other < s; ++other)
            {
                if (shadows[other].light == shadow.light)
                {
                    spdlog::error("addScenePasses: two shadows of one light.");
                    return false;
                }
            }
            // A directional light's cascades are fitted to the aspect ratio of the one view that reads them.
            for (uint32_t heldView : graph.sceneViews)
            {
                const SceneView &held = gpuScene->views[heldView];
                if (!held.depthOnly && holdsShadow(held, shadow))
                {
                    spdlog::error("addScenePasses: a shadow from addShadowPasses is read by one view only.");
                    return false;
                }
            }
        }

        // A view texture its materials sample orders the draws after its writer; drawing into it while it is sampled
        // is not defined, and a target with no color has nothing to sample.
        if (sampledCount > 0 && sampled == nullptr)
        {
            spdlog::error("addScenePasses: sampledCount is {} and sampled is null.", sampledCount);
            return false;
        }
        for (uint32_t i = 0; i < sampledCount; ++i)
        {
            if (sampled[i] == renderTarget || !sampled[i].isValid() || ctx.getRenderTarget(sampled[i]).colorCount == 0)
            {
                spdlog::error("addScenePasses: a sampled target is a valid render target with a color, other than the one the view draws.");
                return false;
            }
        }

        return addSceneViewPasses(graph, scene, camera, renderTarget, 0, shadows, shadowCount, depthOnly, sampled, sampledCount);
    }

    // A light's shadow: depth-only maps, each drawn by its own scene view, with LODs from the camera. A directional
    // light's cascades cover slices of the camera's view; a spot light's map looks along its cone; a point light's
    // cube is drawn by a view per face. The view that reads the shadow gets it from addScenePasses.
    SceneShadow Renderer::addShadowPasses(RenderGraph &graph, SceneHandle scene, CameraHandle camera, const ShadowDesc &desc)
    {
        if (!isLive(camera))
        {
            spdlog::error("addShadowPasses needs a valid camera handle.");
            return {};
        }
        if (!isLive(scene))
        {
            spdlog::error("addShadowPasses needs a valid scene handle.");
            return {};
        }
        if (!isLive(desc.light))
        {
            spdlog::error("addShadowPasses needs a valid light.");
            return {};
        }
        const LightType type = registry.lights[desc.light.index].type;
        const bool directional = type == LightType::DIRECTIONAL;
        // The checks are written so that NaN fails them.
        if (directional && (desc.cascadeCount == 0 || desc.cascadeCount > MAX_SHADOW_CASCADES || desc.size == 0 || !(desc.distance > getNearZ(registry.cameras[camera.index])) ||
                            !(desc.splitLambda >= 0.0f && desc.splitLambda <= 1.0f)))
        {
            spdlog::error("addShadowPasses needs 1 to {} cascades, a size above 0, a distance beyond the camera's near plane and a splitLambda from 0 to 1.", MAX_SHADOW_CASCADES);
            return {};
        }
        if (!directional && (desc.size == 0 || !(desc.distance > 0.0f) || std::isinf(desc.distance)))
        {
            spdlog::error("addShadowPasses needs a size above 0 and a finite distance above 0.");
            return {};
        }

        // Each map, and each face of a cube, is a new view, so check for room before adding any, and leave no map
        // without a reader.
        const uint32_t viewCount = directional ? desc.cascadeCount : (type == LightType::POINT ? CUBE_FACES : 1);
        uint32_t freeViews = 0;
        for (const SceneView &view : gpuScene->views)
            freeViews += view.graphs == 0 ? 1 : 0;
        if (freeViews < viewCount)
        {
            spdlog::error("addShadowPasses needs {} free scene views, and {} of {} are free.", viewCount, freeViews, MAX_SCENE_VIEWS);
            return {};
        }

        SceneShadow shadow = {.light = desc.light, .mapCount = directional ? desc.cascadeCount : 1, .distance = desc.distance, .splitLambda = directional ? desc.splitLambda : 0.0f};
        for (uint32_t i = 0; i < shadow.mapCount; ++i)
        {
            std::string name = "shadow_cascade_" + std::to_string(i);
            if (!directional)
                name = (type == LightType::POINT ? "shadow_cube_" : "shadow_spot_") + std::to_string(desc.light.index);
            shadow.maps[i] = graph.createRenderTarget({.name = name, .width = desc.size, .height = desc.size, .depthFormat = DEPTH_FORMAT, .cube = type == LightType::POINT});
            if (!shadow.maps[i].isValid())
            {
                spdlog::error("addShadowPasses could not create the shadow's render targets.");
                return {};
            }
        }
        for (uint32_t i = 0; i < viewCount; ++i)
        {
            const bool cube = type == LightType::POINT;
            if (!addSceneViewPasses(graph, scene, camera, shadow.maps[cube ? 0 : i], cube ? i : 0, &shadow, 1, true, nullptr, 0))
                return {};
        }
        return shadow;
    }

    bool Renderer::addSceneViewPasses(RenderGraph &graph, SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget, uint32_t face, const SceneShadow *shadows, uint32_t shadowCount, bool depthOnly,
                                      const RenderTargetHandle *sampled, uint32_t sampledCount)
    {
        // A view's passes run once per frame: a second set would read its own visibility as last frame's.
        for (uint32_t heldView : graph.sceneViews)
        {
            const SceneView &held = gpuScene->views[heldView];
            if (held.scene == scene && held.camera == camera && held.renderTarget == renderTarget && held.face == face)
            {
                spdlog::error("addScenePasses was called twice for the same scene, camera and target in one graph.");
                return false;
            }
        }

        const uint32_t view = createSceneView(scene, camera, renderTarget, face, shadows, shadowCount, depthOnly);
        if (view == UINT32_MAX)
            return false;
        graph.sceneViews.push_back(view);
        // Every view of the graph draws the animated sub-meshes with the matrices of one Animate pass.
        addAnimationPass(graph);
        const SceneView &sceneView = gpuScene->views[view];
        const bool occlusion = sceneView.visibility.isValid();
        std::string drawName = depthOnly ? "Shadow" : "Scene";
        if (depthOnly && shadowCount > 0)
        {
            // Local lights' passes name the light, like their targets, so two of them can be told apart.
            const LightType type = registry.lights[shadows[0].light.index].type;
            const std::string light = std::to_string(shadows[0].light.index);
            if (type == LightType::SPOT)
                drawName = "Spot " + light;
            if (type == LightType::POINT)
                drawName = "Cube " + light + " Face " + std::to_string(face);
            for (uint32_t i = 0; type == LightType::DIRECTIONAL && i < shadows[0].mapCount; ++i)
            {
                if (shadows[0].maps[i] == renderTarget)
                    drawName = "Cascade " + std::to_string(i);
            }
        }
        const std::string prefix = depthOnly ? drawName + " " : "";

        // A shaded view's first culling pass also sorts its lights into clusters, which its draw passes read.
        const ViewClusters &clusters = gpuLights->views[view];
        // Each frame uses only its slot's draw list and Hi-Z, so culling the next frame does not wait for
        // this frame's draws on the other queue. A shaded view's culling also sorts the blend run, in the late pass,
        // or in the early one in a frame without a Hi-Z.
        const GpuAnimation &animation = *gpuAnimation;
        auto writeCulling = [&sceneView, &animation](PassBuilder &builder)
        {
            builder.read(animation.transforms);
            builder.write(sceneView.visibleDraws);
            if (!sceneView.depthOnly)
                builder.write(sceneView.sortBuffers);
            if (sceneView.pointRaster[0].isValid())
                builder.write(sceneView.pointRaster);
            if (sceneView.glyphCull[0].isValid())
                builder.write(sceneView.glyphCull);
        };
        // A shaded view's draws also read where the frame before drew a skinned vertex, for motion.
        auto drawScenePass = [&sceneView, &clusters, &animation, renderTarget, face, sampled, sampledCount](PassBuilder &builder)
        {
            for (uint32_t i = 0; i < sampledCount; ++i)
                builder.read(sampled[i]);
            builder.read(animation.transforms);
            builder.read(animation.skinnedVertices);
            if (!sceneView.depthOnly)
                builder.readPrevious(animation.skinnedVertices);
            builder.read(sceneView.visibleDraws);
            if (sceneView.pointRaster[0].isValid())
                builder.read(sceneView.pointRaster);
            if (sceneView.glyphCull[0].isValid())
                builder.read(sceneView.glyphCull);
            if (clusters.buffers[0].isValid())
                builder.read(clusters.buffers);
            for (uint32_t s = 0; !sceneView.depthOnly && s < sceneView.shadowCount; ++s)
            {
                for (uint32_t i = 0; i < sceneView.shadows[s].mapCount; ++i)
                    builder.readDepth(sceneView.shadows[s].maps[i]);
            }
            builder.write(renderTarget, face);
        };

        graph.addAsyncComputePass(
            prefix + (occlusion ? "Cull 1" : "Cull"),
            [&](PassBuilder &builder)
            {
                writeCulling(builder);
                if (clusters.buffers[0].isValid())
                    builder.write(clusters.buffers);
                if (occlusion)
                    builder.readPrevious(sceneView.visibility);
                if (sceneView.meshletVisibility.isValid())
                    builder.readPrevious(sceneView.meshletVisibility);
            },
            [this, view](RenderContext &)
            {
                prepareSceneView(view);
                recordSceneCulling(view, CULL_EARLY);
                if (gpuScene->views[view].ready)
                    recordLightClusters(view, getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), view));
            });
        // The sky goes behind what the view's last draw pass of the frame drew: the late pass, unless this frame has
        // none (no Hi-Z, or frozen culling).
        // With mesh shaders the draw passes cull the meshlets themselves (lib/meshletTask.slang): the early one reads the
        // meshlet bits, the late one reads the Hi-Z and writes the bits, as the culling passes do on the other path. The
        // graph is fixed and the path is not, so this holds wherever the device has them: with async compute, Cull 1 then
        // also waits for the frame before's Scene 2.
        const bool meshletBitsInDraws = ctx.hasMeshShaders() && sceneView.meshletVisibility.isValid();
        graph.addPass(
            occlusion ? drawName + " 1" : drawName,
            [&](PassBuilder &builder)
            {
                drawScenePass(builder);
                if (meshletBitsInDraws)
                    builder.readPrevious(sceneView.meshletVisibility);
            },
            [this, view](RenderContext &)
            {
                drawSceneView(view, CULL_EARLY);
                if (!gpuScene->views[view].late)
                {
                    drawViewGlyphs(view);
                    drawPointResolve(view);
                    drawSky(view);
                    drawWeighted(view);
                }
            });
        // See-through draws go over everything the draw passes and the sky left, so a shaded view ends with them. The
        // pass is there even while the scene has no blend run, since the graph is fixed and the scene is not. The
        // culling view's ghosts come last, and read the visibility the late pass left. Before it, the labels' anchors
        // are tested against the depth the draw passes left, so each label shows or hides whole. The weighted draws went
        // into the view's weighted target at the end of its last draw pass (drawWeighted); after the grid, their composite
        // and the additive draws go under the sorted ones, so labels and other ordered layers stay on top.
        auto addTransparentPass = [&]()
        {
            if (depthOnly)
                return;
            // A cube's faces keep the per-pixel test (the pass reads a 2D depth).
            if (!ctx.getRenderTargetDesc(renderTarget).cube)
                graph.addComputePass(
                    "Label Depth",
                    [&](PassBuilder &builder)
                    {
                        builder.readDepth(renderTarget);
                        builder.read(sceneView.labelProbes);
                        builder.write(sceneView.labelBuffers);
                        builder.readPrevious(sceneView.labelFades);
                        builder.write(sceneView.labelFades);
                    },
                    [this, view](RenderContext &)
                    { recordLabelDepth(view); });
            graph.addPass(
                "Transparent",
                [&](PassBuilder &builder)
                {
                    drawScenePass(builder);
                    builder.read(sceneView.labelBuffers);
                    if (occlusion)
                        builder.read(sceneView.visibility);
                },
                [this, view](RenderContext &)
                {
                    drawGrid(view);
                    drawWeightedComposite(view);
                    drawAdditive(view);
                    drawTransparent(view);
                    drawCullingGhosts(view);
                });
        };
        if (!occlusion)
        {
            addTransparentPass();
            return true;
        }

        graph.addAsyncComputePass(
            prefix + "Hi-Z",
            [&](PassBuilder &builder)
            {
                builder.readDepth(renderTarget);
                builder.write(sceneView.hiz);
            },
            [this, view](RenderContext &)
            { recordHiz(view); });
        graph.addAsyncComputePass(
            prefix + "Cull 2",
            [&](PassBuilder &builder)
            {
                builder.read(sceneView.hiz);
                writeCulling(builder);
                builder.write(sceneView.visibility);
                if (sceneView.meshletVisibility.isValid())
                    builder.write(sceneView.meshletVisibility);
            },
            [this, view](RenderContext &)
            {
                if (gpuScene->views[view].late)
                    recordSceneCulling(view, CULL_LATE);
            });
        graph.addPass(
            drawName + " 2",
            [&](PassBuilder &builder)
            {
                drawScenePass(builder);
                if (ctx.hasMeshShaders())
                    builder.read(sceneView.hiz);
                if (meshletBitsInDraws)
                    builder.write(sceneView.meshletVisibility);
            },
            [this, view](RenderContext &)
            {
                if (!gpuScene->views[view].late)
                    return;
                drawSceneView(view, CULL_LATE);
                drawViewGlyphs(view);
                drawPointResolve(view);
                drawSky(view);
                drawWeighted(view);
            });
        addTransparentPass();
        return true;
    }

    // A view lives while a graph holds it, so graphs built for the same scene, camera, target, face and shadows share
    // it. Its buffers start small and grow in prepareSceneView under the same handles.
    uint32_t Renderer::createSceneView(SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget, uint32_t face, const SceneShadow *shadows, uint32_t shadowCount, bool depthOnly)
    {
        SceneShadow viewShadows[MAX_VIEW_SHADOWS] = {};
        std::copy(shadows, shadows + shadowCount, viewShadows);
        uint32_t freeSlot = UINT32_MAX;
        for (uint32_t i = 0; i < MAX_SCENE_VIEWS; ++i)
        {
            SceneView &view = gpuScene->views[i];
            if (view.graphs == 0)
            {
                freeSlot = std::min(freeSlot, i);
                continue;
            }
            if (view.scene == scene && view.camera == camera && view.renderTarget == renderTarget && view.face == face && view.shadowCount == shadowCount &&
                std::equal(view.shadows, view.shadows + MAX_VIEW_SHADOWS, viewShadows))
            {
                ++view.graphs;
                return i;
            }
        }
        if (freeSlot == UINT32_MAX)
        {
            spdlog::error("All {} scene views (MAX_SCENE_VIEWS) are held by render graphs (each view, shadow cascade and cube face takes one); this view is not drawn.", MAX_SCENE_VIEWS);
            return UINT32_MAX;
        }
        // A view on a new kind of target draws this frame, so its shaders go to the shader workers first.
        if (!depthOnly)
        {
            const RenderTarget &target = ctx.getRenderTarget(renderTarget);
            warmScenePipelines(target.colorCount == 2, target.samples > 1, true);
        }

        SceneView view = {.scene = scene, .camera = camera, .renderTarget = renderTarget, .face = face, .shadowCount = shadowCount, .depthOnly = depthOnly, .graphs = 1};
        std::copy(viewShadows, viewShadows + MAX_VIEW_SHADOWS, view.shadows);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            view.visibleCapacity[i] = CULL_GROUP_SIZE;
            view.visibleDraws[i] = ctx.createBuffer({.name = "Visible Draw Buffer", .usage = BufferUsage::INDIRECT, .size = getVisibleDrawLayout(CULL_GROUP_SIZE, 0, 0).size});
            if (!view.visibleDraws[i].isValid())
            {
                spdlog::error("Failed to create the culling buffers of a scene view.");
                for (auto &buffer : view.visibleDraws)
                    destroyViewBuffer(ctx, buffer);
                return UINT32_MAX;
            }
        }

        // A shaded view sorts its blend run and places labels; the buffers start small and grow under the same handles
        // (placeViewLabels sizes the label buffers).
        for (uint32_t i = 0; !depthOnly && i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            view.sortCapacity[i] = BLEND_SORT_BLOCK;
            view.sortBuffers[i] = ctx.createBuffer({.name = "Blend Sort Buffer", .usage = BufferUsage::STORAGE, .size = getBlendSortBufferSize(BLEND_SORT_BLOCK)});
            view.labelBuffers[i] = ctx.createBuffer({.name = "Label Visibility Buffer", .usage = BufferUsage::STORAGE, .size = sizeof(float)});
            view.labelProbes[i] = ctx.createBuffer({.name = "Label Probe Buffer", .usage = BufferUsage::STORAGE, .size = sizeof(glm::vec4)});
            if (!view.sortBuffers[i].isValid() || !view.labelBuffers[i].isValid() || !view.labelProbes[i].isValid())
            {
                spdlog::error("Failed to create the blend sort and label buffers of a scene view.");
                for (uint32_t j = 0; j < MAX_FRAMES_IN_FLIGHT; ++j)
                {
                    destroyViewBuffer(ctx, view.visibleDraws[j]);
                    destroyViewBuffer(ctx, view.sortBuffers[j]);
                    destroyViewBuffer(ctx, view.labelBuffers[j]);
                    destroyViewBuffer(ctx, view.labelProbes[j]);
                }
                return UINT32_MAX;
            }
        }
        // A shaded view's small points go to the compute path where the GPU has 64-bit buffer atomics: its buffer
        // starts as the header alone and grows once the view has a point run (preparePointRaster). Without it, or
        // without the atomics, every point is drawn by triangles.
        for (uint32_t i = 0; !depthOnly && ctx.hasBufferInt64Atomics() && i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            view.pointRaster[i] = ctx.createBuffer({.name = "Point Raster Buffer", .usage = BufferUsage::INDIRECT, .size = sizeof(gfx::PointRasterHeader)});
            view.pointRasterBytes[i] = sizeof(gfx::PointRasterHeader);
            if (!view.pointRaster[i].isValid())
            {
                spdlog::error("Failed to create the point raster buffers of a scene view; its points are drawn by triangles.");
                for (uint32_t j = 0; j < MAX_FRAMES_IN_FLIGHT; ++j)
                    destroyViewBuffer(ctx, view.pointRaster[j]);
                break;
            }
        }
        // A shaded view's glyph chunks are culled per glyph into a list its last draw pass draws (glyphCull.cpp): the
        // buffer starts as the header alone and grows once the scene has glyph chunks. Without it, they are not drawn.
        for (uint32_t i = 0; !depthOnly && i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            view.glyphCull[i] = ctx.createBuffer({.name = "Glyph Cull Buffer", .usage = BufferUsage::INDIRECT, .size = sizeof(gfx::GlyphCullHeader)});
            view.glyphCullBytes[i] = sizeof(gfx::GlyphCullHeader);
            if (!view.glyphCull[i].isValid())
            {
                spdlog::error("Failed to create the glyph cull buffers of a scene view; its glyphs are not drawn.");
                for (uint32_t j = 0; j < MAX_FRAMES_IN_FLIGHT; ++j)
                    destroyViewBuffer(ctx, view.glyphCull[j]);
                break;
            }
        }
        if (!depthOnly)
        {
            view.labelFades = ctx.createBuffer({.name = "Label Fade Buffer", .usage = BufferUsage::STORAGE, .size = sizeof(float)});
            if (!view.labelFades.isValid())
            {
                spdlog::error("Failed to create the label buffers of a scene view.");
                for (uint32_t j = 0; j < MAX_FRAMES_IN_FLIGHT; ++j)
                {
                    destroyViewBuffer(ctx, view.visibleDraws[j]);
                    destroyViewBuffer(ctx, view.sortBuffers[j]);
                    destroyViewBuffer(ctx, view.labelBuffers[j]);
                    destroyViewBuffer(ctx, view.labelProbes[j]);
                    destroyViewBuffer(ctx, view.pointRaster[j]);
                    destroyViewBuffer(ctx, view.glyphCull[j]);
                }
                return UINT32_MAX;
            }
        }

        // The view's visibility, one flag per instance row, is kept across frames and shared by both frame slots: the
        // GPU runs the frames in order. Rows start unseen, so a new instance is drawn late first. Without these
        // buffers the view is only frustum culled, like a cube's faces: the Hi-Z build reads a 2D depth.
        if (config.occlusionCulling && !ctx.getRenderTargetDesc(renderTarget).cube)
        {
            bool created = true;
            for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
            {
                view.hizCapacity[i] = 1;
                view.hiz[i] = ctx.createBuffer({.name = "Hi-Z Buffer", .usage = BufferUsage::STORAGE, .size = sizeof(float)});
                created = created && view.hiz[i].isValid();
            }
            const std::vector<uint32_t> unseen(config.maxInstances, 0);
            view.visibility = ctx.createBuffer({.name = "Visibility Buffer", .usage = BufferUsage::STORAGE, .size = unseen.size() * sizeof(uint32_t)});
            created = created && view.visibility.isValid() && ctx.fillBuffer(view.visibility, unseen.data(), unseen.size() * sizeof(uint32_t));
            // The meshlets' own bits, one word per instance row on average (GpuScene::meshletWords).
            if (created)
            {
                view.meshletVisibility = ctx.createBuffer({.name = "Meshlet Visibility Buffer", .usage = BufferUsage::STORAGE, .size = gpuScene->meshletWords.capacity() * sizeof(uint32_t)});
                created = view.meshletVisibility.isValid() && ctx.fillBuffer(view.meshletVisibility, unseen.data(), unseen.size() * sizeof(uint32_t));
            }
            if (!created)
            {
                spdlog::error("Failed to create the occlusion culling buffers of a scene view; it is drawn with frustum culling only.");
                for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
                {
                    destroyViewBuffer(ctx, view.hiz[i]);
                    view.hizCapacity[i] = 0;
                }
                destroyViewBuffer(ctx, view.visibility);
                destroyViewBuffer(ctx, view.meshletVisibility);
            }
        }
        gpuScene->views[freeSlot] = view;
        if (!depthOnly)
            createViewClusters(freeSlot);
        return freeSlot;
    }

    // A graph lets go of a view when it is destroyed or replaced. The last one frees the view's buffers once the
    // frames that used them are done.
    void Renderer::releaseSceneView(uint32_t viewIndex)
    {
        SceneView &view = gpuScene->views[viewIndex];
        if (view.graphs == 0)
            return;
        --view.graphs;
        if (view.graphs > 0)
            return;
        ctx.waitForFence(frameFence, frameFenceValue);
        if (asyncCompute)
            ctx.waitForFence(computeFence, computeFenceValue);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            destroyViewBuffer(ctx, view.visibleDraws[i]);
            destroyViewBuffer(ctx, view.hiz[i]);
            destroyViewBuffer(ctx, view.sortBuffers[i]);
            destroyViewBuffer(ctx, view.labelBuffers[i]);
            destroyViewBuffer(ctx, view.labelProbes[i]);
            destroyViewBuffer(ctx, view.pointRaster[i]);
            destroyViewBuffer(ctx, view.glyphCull[i]);
            destroyViewBuffer(ctx, view.weightedRange[i]);
        }
        if (view.weightedTarget.isValid())
            destroyRenderTarget(view.weightedTarget);
        destroyViewBuffer(ctx, view.labelFades);
        destroyViewBuffer(ctx, view.visibility);
        destroyViewBuffer(ctx, view.meshletVisibility);
        releaseViewClusters(viewIndex);
        view = {};
    }

    // The view's first pass runs it, before anything of the view is recorded. It decides what the view's passes do
    // this frame: nothing for an empty scene or one whose entities still load (the passes keep their cleared
    // attachments), the draws without culling when the culling pipelines fail, and no late half without a Hi-Z.
    void Renderer::prepareSceneView(uint32_t viewIndex)
    {
        ZoneScoped;

        SceneView &view = gpuScene->views[viewIndex];
        view.ready = false;
        view.culled = false;
        view.sky = false;
        view.grid = false;
        view.drawMask = false;
        view.drawBlend = false;
        view.sortBlend = false;
        view.blendRun = gfx::MAX_DRAW_RUNS;
        view.weightedRun = gfx::MAX_DRAW_RUNS;
        view.additiveRun = gfx::MAX_DRAW_RUNS;
        view.drawWeighted = false;
        view.drawWeightedGlyphs = false;
        view.drawAdditive = false;
        view.groupCount = 0;
        view.hizLevelCount = 0;
        view.late = false;
        view.debugView = DebugView::NONE;
        view.meshletRuns = 0;
        view.meshShaders = false;
        view.rasterPoints = false;
        view.drawGlyphs = false;

        const auto &drawList = gpuScene->drawLists[view.scene.index];
        const auto frame = static_cast<uint32_t>(frameCount);
        // A pick of this frame finds the view drawn; one that is not ready shows nothing.
        view.preparedSync = gpuScene->syncCount;
        // An export's view draws one tile of the whole image with the camera made for the whole image, or nothing while
        // the export waits (src/renderer/export.cpp).
        const ExportViewFrame *exportFrame = exports ? getExportViewFrame(*exports, view.scene, view.camera, view.renderTarget) : nullptr;
        if (exportFrame != nullptr && exportFrame->idle)
            return;
        if (drawList.draws.empty() || (drawList.staleSlots & (1u << frame)) != 0)
        {
            // Nothing to draw, but the sky still shows, so a scene whose meshes still load is not left black. The sky
            // reads only the camera and the environment.
            if (view.depthOnly)
                return;
            const auto &target = ctx.getRenderTarget(view.renderTarget);
            const glm::dvec3 origin = registry.cameras[view.camera.index].rig.transform.position;
            const glm::dmat4 worldView = getViewMatrix(view.camera);
            gfx::SceneData sceneData{};
            sceneData.projection = getProjectionMatrix(view.camera, static_cast<float>(target.width) / static_cast<float>(target.height));
            sceneData.pixelScale = 1.0f;
            if (exportFrame != nullptr)
            {
                sceneData.projection = exportFrame->tileClip * getProjectionMatrix(view.camera, static_cast<float>(exportFrame->imageWidth) / static_cast<float>(exportFrame->imageHeight));
                sceneData.pixelScale = exportFrame->pixelScale;
            }
            sceneData.view = toRenderMatrix(worldView, origin);
            writeRenderOrigin(sceneData, origin);
            writeViewMotion(view, sceneData, worldView, origin, target.width, target.height);
            sceneData.grid = makeGridData(registry.cameras[view.camera.index].grid, origin);
            const bool sky = writeSceneEnvironment(*environments, registry.scenes[view.scene.index].environment, sceneData) && !(exportFrame != nullptr && exportFrame->transparent);
            if (!ctx.fillBuffer(gpuScene->sceneData[frame], &sceneData, sizeof(sceneData), viewIndex * sizeof(gfx::SceneData)))
                return;
            view.sky = sky;
            view.grid = sceneData.grid.levelCount != 0;
            return;
        }

        // A shader error retries every frame, so a shader fixed on disk is picked up while the application runs. The
        // shaders of these pipelines are warmed at startup (warmScenePipelines) under the same names and defines.
        ShadedPipelines &shaded = getShadedPipelines(*gpuScene, ctx, view);
        if (!view.depthOnly && !shaded.pipeline.isValid())
        {
            shaded.pipeline = createScenePipeline("rasm", "passes/rasm.slang", getMotionDefine(ctx, view), view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true);
            if (!shaded.pipeline.isValid())
            {
                spdlog::error("Failed to create the uber material pipeline for the scene.");
                return;
            }
        }
        if (view.depthOnly && !gpuScene->depthPipeline.isValid())
        {
            gpuScene->depthPipeline = createScenePipeline("shadow", "passes/shadow.slang", "", view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true);
            if (!gpuScene->depthPipeline.isValid())
            {
                spdlog::error("Failed to create the depth-only pipeline for the scene.");
                return;
            }
        }
        // The pipelines of the other alpha modes are made once a scene has their draws, so a scene without them pays
        // for no compile. A failed one leaves out only its runs. MASK and BLEND are rasm.slang's fragMain compiled with
        // a define: an entry point that called the opaque one would be a function call that reads every varying up
        // front, which costs the opaque draws, and Slang's API does not compile an entry point that calls another.
        const bool hasMask = hasRun(drawList, AlphaMode::MASK);
        const bool hasBlend = hasRun(drawList, AlphaMode::BLEND);
        // With several samples per pixel, MASK covers the samples its sharpened alpha says (alpha-to-coverage) instead of
        // discarding, so a cut-out's edge is as smooth as a triangle's.
        const bool multisampled = ctx.getRenderTarget(view.renderTarget).samples > 1;
        if (!view.depthOnly && hasMask && !shaded.maskPipeline.isValid())
        {
            const std::string defines = getMotionDefine(ctx, view) + std::string(multisampled ? "#define RASM_ALPHA_MASK 1\n#define RASM_ALPHA_TO_COVERAGE 1\n" : "#define RASM_ALPHA_MASK 1\n");
            shaded.maskPipeline = createScenePipeline("rasmMask", "passes/rasm.slang", defines, view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true, multisampled);
            if (!shaded.maskPipeline.isValid())
                spdlog::error("Failed to create the MASK pipeline; the scene is drawn without its cut-out materials.");
        }
        if (!view.depthOnly && hasBlend && !shaded.blendPipeline.isValid())
        {
            shaded.blendPipeline = createScenePipeline("rasmBlend", "passes/rasm.slang", getMotionDefine(ctx, view) + std::string("#define RASM_ALPHA_BLEND 1\n"), view.renderTarget, "vertMain", "fragMain", BlendMode::PREMULTIPLIED_ALPHA, false);
            if (!shaded.blendPipeline.isValid())
                spdlog::error("Failed to create the BLEND pipeline; the scene is drawn without its see-through materials.");
        }
        if (view.depthOnly && (hasMask || hasBlend) && !gpuScene->depthMaskPipeline.isValid())
        {
            gpuScene->depthMaskPipeline = createScenePipeline("shadowMask", "passes/shadow.slang", "", view.renderTarget, "vertMask", "fragMask", BlendMode::NONE, true);
            if (!gpuScene->depthMaskPipeline.isValid())
                spdlog::error("Failed to create the depth-only MASK pipeline; cut-out and see-through materials cast no shadow.");
        }
        view.drawMask = view.depthOnly ? gpuScene->depthMaskPipeline.isValid() : shaded.maskPipeline.isValid();
        // The point run has a pipeline of its own (passes/points.slang), made once a scene has point cloud chunks: a
        // quad per point pulled from the point pool, alpha-to-coverage with several samples. A depth-only view draws none.
        const bool hasPoints = std::any_of(drawList.runs.begin(), drawList.runs.end(), [](const DrawRun &run)
                                           { return run.points; });
        if (!view.depthOnly && hasPoints && !shaded.pointPipeline.isValid())
        {
            const std::string defines = getMotionDefine(ctx, view) + std::string(multisampled ? "#define RASM_ALPHA_TO_COVERAGE 1\n" : "");
            shaded.pointPipeline = createScenePipeline("points", "passes/points.slang", defines, view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true, multisampled);
            if (!shaded.pointPipeline.isValid())
                spdlog::error("Failed to create the point pipeline; the scene is drawn without its point clouds.");
        }
        // A debug view's pipelines are made the first time it is chosen, like MASK and BLEND: rasm.slang compiled with
        // RASM_DEBUG_VIEW, with its MASK cut-outs discarded rather than covered. OVERDRAW's add up their fragments, so
        // they have pipelines of their own, and the culling view also draws its ghosts.
        // An export draws its view shaded, whatever debug view the frames show.
        if (!view.depthOnly && debugView != DebugView::NONE && exportFrame == nullptr)
        {
            DebugPipelines &debug = debugView == DebugView::OVERDRAW ? shaded.overdraw : shaded.debug;
            const BlendMode blendMode = debugView == DebugView::OVERDRAW ? BlendMode::ADDITIVE : BlendMode::NONE;
            // Only made while a pipeline is missing: a string this long allocates, and this runs every frame (P27).
            const bool missing = !debug.pipeline.isValid() || (hasMask && !debug.maskPipeline.isValid()) || (hasBlend && !debug.blendPipeline.isValid());
            const std::string defines = missing ? getMotionDefine(ctx, view) + std::string("#define RASM_DEBUG_VIEW 1\n") : std::string();
            // A debug view's shaders are not warmed at startup: the first time it is chosen they compile on the shader
            // workers, and the view draws shaded until they are all ready, so no run of it is left out meanwhile.
            bool pending = false;
            if (!debug.pipeline.isValid())
                debug.pipeline = createScenePipeline("rasmDebug", "passes/rasm.slang", defines, view.renderTarget, "vertMain", "fragMain", blendMode, true, false, true, &pending);
            if (hasMask && !debug.maskPipeline.isValid())
            {
                bool maskPending = false;
                debug.maskPipeline = createScenePipeline("rasmDebugMask", "passes/rasm.slang", defines + "#define RASM_ALPHA_MASK 1\n", view.renderTarget, "vertMain", "fragMain", blendMode, true, false, true, &maskPending);
                if (!debug.maskPipeline.isValid() && !maskPending)
                    spdlog::error("Failed to create the debug view's MASK pipeline; the view is drawn without its cut-out materials.");
                pending = pending || maskPending;
            }
            if (hasBlend && !debug.blendPipeline.isValid())
            {
                bool blendPending = false;
                debug.blendPipeline = createScenePipeline("rasmDebugBlend", "passes/rasm.slang", defines + "#define RASM_ALPHA_BLEND 1\n", view.renderTarget, "vertMain", "fragMain", blendMode, false, false, true, &blendPending);
                if (!debug.blendPipeline.isValid() && !blendPending)
                    spdlog::error("Failed to create the debug view's BLEND pipeline; the view is drawn without its see-through materials.");
                pending = pending || blendPending;
            }
            if (debugView == DebugView::CULLING && !shaded.ghostPipeline.isValid())
            {
                bool ghostPending = false;
                shaded.ghostPipeline = createScenePipeline("debugGhosts", "passes/debugGhosts.slang", getMotionDefine(ctx, view), view.renderTarget, "vertMain", "fragMain", BlendMode::ADDITIVE, false, false, false, &ghostPending);
                if (!shaded.ghostPipeline.isValid() && !ghostPending)
                    spdlog::error("Failed to create the culling view's ghost pipeline; the view is drawn without its ghosts.");
                pending = pending || ghostPending;
            }
            // Without its pipeline the view draws its shading, which the debug display then shows as it is.
            if (debug.pipeline.isValid() && !pending)
            {
                view.debugView = debugView;
                view.drawMask = debug.maskPipeline.isValid();
            }
            else if (!pending)
                spdlog::error("Failed to create the debug view pipeline; the scene is drawn shaded.");
        }
        // Without the culling pipelines the scene is still drawn, only unculled, so a culling shader error does not blank it.
        if (!gpuScene->cullCountPipeline.isValid() || !gpuScene->cullWritePipeline.isValid())
        {
            const std::string cullSource = readFile(pathFromUtf8(config.shaderDir) / "passes/cull.slang");
            gpuScene->cullCountPipeline = createComputePipeline("cullCount", cullSource, "countMain");
            gpuScene->cullWritePipeline = createComputePipeline("cullWrite", cullSource, "writeMain");
            if (!gpuScene->cullCountPipeline.isValid() || !gpuScene->cullWritePipeline.isValid())
                spdlog::error("Failed to create the culling pipelines; the scene is drawn without culling.");
        }
        const bool culled = gpuScene->cullCountPipeline.isValid() && gpuScene->cullWritePipeline.isValid();

        // The meshlet pipelines, made once a scene has meshlets to draw. With mesh shaders: the task and mesh pipelines of
        // the runs that have meshlets; where the device has none, or they fail, the vertex-pulling passes. Without either,
        // big draws are drawn whole.
        bool meshletDraws = false;
        bool opaqueMeshlets = false;
        bool maskMeshlets = false;
        for (const DrawRun &run : drawList.runs)
        {
            const bool hasMeshlets = run.alphaMode != AlphaMode::BLEND && run.meshletDraws > 0;
            meshletDraws = meshletDraws || hasMeshlets;
            opaqueMeshlets = opaqueMeshlets || (hasMeshlets && run.alphaMode == AlphaMode::NONE);
            maskMeshlets = maskMeshlets || (hasMeshlets && run.alphaMode == AlphaMode::MASK && view.drawMask);
        }
        // The mesh shaders read the meshlet data stream, which RendererConfig::geometryPoolMeshletData = 0 leaves out.
        const MeshletPath meshletPath = resolveMeshletPath(config.meshletPath, ctx.hasMeshShaders() && geometryPool->meshletDataBuffer().isValid());
        meshletDraws = meshletDraws && culled && meshletPath != MeshletPath::OFF;
        view.meshShaders = meshletDraws && meshletPath == MeshletPath::MESH_SHADERS && createMeshletPipelines(viewIndex, opaqueMeshlets, maskMeshlets);
        // AUTO picked the mesh shaders for their cost; without their pipelines it draws whole, as where the GPU has none.
        meshletDraws = meshletDraws && (view.meshShaders || config.meshletPath != MeshletPath::AUTO);
        if (meshletDraws && !view.meshShaders && (!gpuScene->meshletCountPipeline.isValid() || !gpuScene->meshletWritePipeline.isValid()))
        {
            const std::string cullSource = readFile(pathFromUtf8(config.shaderDir) / "passes/cull.slang");
            if (!gpuScene->meshletCountPipeline.isValid())
                gpuScene->meshletCountPipeline = createComputePipeline("meshletCount", cullSource, "meshletCountMain");
            if (!gpuScene->meshletWritePipeline.isValid())
                gpuScene->meshletWritePipeline = createComputePipeline("meshletWrite", cullSource, "meshletWriteMain");
            if (!gpuScene->meshletCountPipeline.isValid() || !gpuScene->meshletWritePipeline.isValid())
                spdlog::error("Failed to create the meshlet culling pipelines; big meshes are drawn whole.");
        }
        meshletDraws = meshletDraws && (view.meshShaders || (gpuScene->meshletCountPipeline.isValid() && gpuScene->meshletWritePipeline.isValid()));

        // A shaded view draws its blend run last, back to front: the run is sorted on the GPU once culling has packed it.
        // Its weighted and additive runs are drawn in any order (weightedBlend.cpp); a depth-only view draws the weighted
        // one like the others, and leaves out the additive one, whose light casts no shadow.
        for (uint32_t i = 0; i < drawList.runs.size(); ++i)
        {
            if (drawList.runs[i].alphaMode != AlphaMode::BLEND)
                continue;
            const BlendMethod method = drawList.runs[i].blendMethod;
            if (method == BlendMethod::ADDITIVE)
                view.additiveRun = i;
            else if (!view.depthOnly && method == BlendMethod::WEIGHTED)
                view.weightedRun = i;
            else if (!view.depthOnly && method == BlendMethod::SORTED)
                view.blendRun = i;
        }
        const PipelineHandle &blendPipeline = view.debugView != DebugView::NONE ? getDebugPipelines(shaded, view.debugView).blendPipeline : shaded.blendPipeline;
        view.drawBlend = view.blendRun < gfx::MAX_DRAW_RUNS && blendPipeline.isValid();
        bool sortPipelines = true;
        for (const PipelineHandle &pipeline : gpuScene->sortPipelines)
            sortPipelines = sortPipelines && pipeline.isValid();
        if (view.drawBlend && culled && !sortPipelines)
        {
            const std::string sortSource = readFile(pathFromUtf8(config.shaderDir) / "passes/sort.slang");
            const char *entryPoints[BLEND_SORT_STEPS] = {"sortKeysMain", "sortMergeMain", "sortLocalMergeMain", "sortWriteMain"};
            sortPipelines = true;
            for (uint32_t i = 0; i < BLEND_SORT_STEPS; ++i)
            {
                if (!gpuScene->sortPipelines[i].isValid())
                    gpuScene->sortPipelines[i] = createComputePipeline("blendSort", sortSource, entryPoints[i]);
                sortPipelines = sortPipelines && gpuScene->sortPipelines[i].isValid();
            }
            if (!sortPipelines)
                spdlog::error("Failed to create the blend sort pipelines; see-through materials are drawn unsorted.");
        }
        if (view.drawBlend && culled && sortPipelines)
        {
            const uint32_t capacity = getBlendSortCapacity(drawList.runs[view.blendRun].count);
            uint32_t &sortCapacity = view.sortCapacity[frame];
            if (sortCapacity < capacity)
            {
                if (ctx.resizeBuffer(view.sortBuffers[frame], getBlendSortBufferSize(capacity)))
                    sortCapacity = capacity;
                else
                    spdlog::error("Failed to grow a blend sort buffer to {} draws; see-through materials are drawn unsorted.", capacity);
            }
            view.sortBlend = sortCapacity >= capacity;
        }

        // Without the Hi-Z pipeline it is still frustum culled.
        if (culled && config.occlusionCulling && !gpuScene->hizPipeline.isValid())
        {
            gpuScene->hizPipeline = createComputePipeline("hiz", readFile(pathFromUtf8(config.shaderDir) / "passes/hiz.slang"), "hizMain");
            if (!gpuScene->hizPipeline.isValid())
                spdlog::error("Failed to create the Hi-Z pipeline; the scene is drawn without occlusion culling.");
        }

        const auto &renderTargetData = ctx.getRenderTarget(view.renderTarget);
        const uint32_t width = renderTargetData.width;
        const uint32_t height = renderTargetData.height;
        const TextureHandle depthTexture = renderTargetData.attachments[frame].depth;

        // Each run drawn by meshlets has a range of meshlet draws: room for its meshlets, up to what is left of the
        // view's budget (RendererConfig::maxViewMeshlets), and one draw per draw of the run, for those past the budget,
        // which are drawn whole. Its chunks have room for every draw at its LOD with the most meshlets.
        uint32_t meshletNeed = 0;
        uint32_t chunkNeed = 0;
        uint32_t runChunkFirst[gfx::MAX_DRAW_RUNS] = {};
        uint32_t runMeshletBudget[gfx::MAX_DRAW_RUNS] = {};
        uint32_t meshletRuns = 0;
        uint32_t budgetLeft = config.maxViewMeshlets;
        for (uint32_t i = 0; meshletDraws && i < drawList.runs.size(); ++i)
        {
            const DrawRun &run = drawList.runs[i];
            if (run.alphaMode == AlphaMode::BLEND || run.meshletDraws == 0)
                continue;
            meshletRuns |= 1u << i;
            // A run's chunks start at a multiple of MESHLET_CHUNK_GROUP, so each sum of chunkSumsPtr holds one run's.
            runChunkFirst[i] = chunkNeed;
            chunkNeed += (run.chunks + MESHLET_CHUNK_GROUP - 1) / MESHLET_CHUNK_GROUP * MESHLET_CHUNK_GROUP;
            // The task shaders draw every visible meshlet straight away: no meshlet draws, so no budget.
            if (view.meshShaders)
            {
                runMeshletBudget[i] = UINT32_MAX;
                view.runMeshletSize[i] = 0;
                continue;
            }
            runMeshletBudget[i] = std::min(run.meshlets, budgetLeft);
            budgetLeft -= runMeshletBudget[i];
            view.runMeshletFirst[i] = meshletNeed;
            view.runMeshletSize[i] = runMeshletBudget[i] + run.meshletDraws;
            meshletNeed += view.runMeshletSize[i];
        }

        // The buffer holds the packed draws and meshlet draws, then culling's scratch. Only this slot's last frame used
        // the old buffer, and the fence wait covered it.
        const auto drawCount = static_cast<uint32_t>(drawList.draws.size());
        uint32_t &visibleCapacity = view.visibleCapacity[frame];
        uint32_t &meshletCapacity = view.meshletCapacity[frame];
        uint32_t &chunkCapacity = view.chunkCapacity[frame];
        if (culled && (visibleCapacity < drawCount || meshletCapacity < meshletNeed || chunkCapacity < chunkNeed))
        {
            const uint32_t capacity = visibleCapacity < drawCount ? std::max(drawCount, visibleCapacity * 2) : visibleCapacity;
            const uint32_t meshlets = meshletCapacity < meshletNeed ? std::max(meshletNeed, meshletCapacity * 2) : meshletCapacity;
            const uint32_t chunks = chunkCapacity < chunkNeed ? std::max(chunkNeed, chunkCapacity * 2) : chunkCapacity;
            if (ctx.resizeBuffer(view.visibleDraws[frame], getVisibleDrawLayout(capacity, meshlets, chunks).size))
            {
                visibleCapacity = capacity;
                meshletCapacity = meshlets;
                chunkCapacity = chunks;
            }
            else if (visibleCapacity < drawCount)
            {
                spdlog::error("Failed to grow a visible draw buffer to {} draws.", capacity);
                return;
            }
            else
            {
                // A failed resize keeps the old buffer, which holds the draws but not the meshlet draws.
                spdlog::error("Failed to grow a visible draw buffer to {} meshlet draws; big meshes are drawn whole.", meshlets);
                meshletRuns = 0;
            }
        }
        view.meshletRuns = culled ? meshletRuns : 0;
        const VisibleDrawLayout layout = getVisibleDrawLayout(view, frame);

        // Occlusion culling samples the pass's depth, which is in the bindless set from its creation
        // (RenderContext::createAttachments). Its Hi-Z covers the whole attachment.
        const uint32_t depthSlot = getBindlessTextureIndex(depthTexture);
        if (culled && config.occlusionCulling && gpuScene->hizPipeline.isValid() && depthSlot != UINT32_MAX && view.visibility.isValid())
        {
            uint32_t texelCount = 0;
            const uint32_t levelCount = getHizLevelCount(width, height, texelCount);
            uint32_t &hizCapacity = view.hizCapacity[frame];
            if (hizCapacity < texelCount)
            {
                if (ctx.resizeBuffer(view.hiz[frame], static_cast<uint64_t>(texelCount) * sizeof(float)))
                    hizCapacity = texelCount;
                else
                    spdlog::error("Failed to grow a Hi-Z buffer to {} texels; the scene is drawn without occlusion culling.", texelCount);
            }
            if (hizCapacity >= texelCount)
                view.hizLevelCount = levelCount;
        }
        // Frozen culling (setCullingFrozen) keeps the frustum and the LOD camera of the frame it froze in, and the
        // visibility that frame's late pass left, so only the early pass runs. That pass then also culls the blend run,
        // which no late pass takes, and its draws go unsorted. A depth-only view is not frozen: the shadow maps
        // follow the camera.
        // An export culls every tile with its own frustum, whatever the debug toggles.
        const bool frozen = cullingFrozen && !view.depthOnly && exportFrame == nullptr;
        view.late = view.hizLevelCount > 0 && !frozen;
        if (frozen && view.hizLevelCount > 0)
            view.sortBlend = false;

        const auto &sceneRef = registry.scenes[view.scene.index];
        const auto &cameraRef = registry.cameras[view.camera.index];

        // A shadow's map is fitted to the view that reads it, and takes its LODs from that view's camera and size, so a
        // surface is shadowed by the same triangles it is drawn with. Any other view uses its own camera and size.
        const bool shadowView = view.depthOnly && view.shadowCount > 0;
        // A frozen reader keeps its LOD camera, so its shadow maps take that one too.
        uint32_t cameraWidth = exportFrame != nullptr ? exportFrame->imageWidth : width;
        uint32_t cameraHeight = exportFrame != nullptr ? exportFrame->imageHeight : height;
        const SceneView *frozenReader = nullptr;
        for (uint32_t i = 0; shadowView && i < MAX_SCENE_VIEWS; ++i)
        {
            const SceneView &reader = gpuScene->views[i];
            if (reader.graphs == 0 || reader.depthOnly || reader.scene != view.scene || reader.camera != view.camera || !holdsShadow(reader, view.shadows[0]))
                continue;
            cameraWidth = ctx.getRenderTarget(reader.renderTarget).width;
            cameraHeight = ctx.getRenderTarget(reader.renderTarget).height;
            frozenReader = reader.frozen && cullingFrozen ? &reader : nullptr;
            break;
        }
        // Every view of a camera draws in that camera's render space: the world moved so that the camera is at the
        // origin. The view matrices are made in world space, in doubles, and moved into it (toRenderMatrix).
        const glm::mat4 cameraProjection = getProjectionMatrix(view.camera, static_cast<float>(cameraWidth) / static_cast<float>(cameraHeight));
        const glm::dmat4 worldCameraView = getViewMatrix(view.camera);
        const glm::dvec3 origin = cameraRef.rig.transform.position;
        const glm::mat4 cameraView = toRenderMatrix(worldCameraView, origin);

        // Each shadow's maps, while its light is in the scene. A light that is not casts no shadow, and its maps draw
        // nothing.
        bool shadowLit[MAX_VIEW_SHADOWS] = {};
        ShadowCascade shadowMaps[MAX_VIEW_SHADOWS][MAX_SHADOW_CASCADES] = {};
        float splits[MAX_VIEW_SHADOWS][MAX_SHADOW_CASCADES] = {};
        const auto &lightScenes = gpuLights->lightScenes;
        for (uint32_t s = 0; s < view.shadowCount; ++s)
        {
            const SceneShadow &shadow = view.shadows[s];
            const bool inScene = isLive(shadow.light) && shadow.light.index < lightScenes.size() && (lightScenes[shadow.light.index] & (1u << view.scene.index)) != 0;
            shadowLit[s] = inScene && getShadowMaps(registry.lights[shadow.light.index], cameraRef, cameraProjection, worldCameraView, shadow, ctx.getRenderTarget(shadow.maps[0]).width, view.face, shadowMaps[s], splits[s]);
        }
        if (shadowView && !shadowLit[0])
            return;

        gfx::SceneData sceneData{};
        sceneData.projection = exportFrame != nullptr ? exportFrame->tileClip * cameraProjection : cameraProjection;
        sceneData.view = cameraView;
        sceneData.pixelScale = exportFrame != nullptr ? exportFrame->pixelScale : 1.0f;
        for (uint32_t i = 0; shadowView && i < view.shadows[0].mapCount; ++i)
        {
            if (view.shadows[0].maps[i] != view.renderTarget)
                continue;
            sceneData.projection = shadowMaps[0][i].projection;
            sceneData.view = toRenderMatrix(shadowMaps[0][i].view, origin);
        }
        view.mirrored = glm::determinant(glm::mat3(sceneData.view)) < 0.0f;
        writeRenderOrigin(sceneData, origin);
        if (!view.depthOnly)
            writeViewMotion(view, sceneData, worldCameraView, origin, width, height);
        if (exportFrame != nullptr)
            sceneData.textureLodBias = exportFrame->textureLodBias;
        getFrustumPlanes(sceneData.projection * sceneData.view, sceneData.frustumPlanes);
        sceneData.cullViewPoint = getCullViewPoint(sceneData.projection, sceneData.view);
        sceneData.nearZ = getNearZ(cameraRef);
        sceneData.vertsPtr = ctx.getBufferDeviceAddress(geometryPool->vertexBuffer());
        sceneData.drawInfoPtr = ctx.getBufferDeviceAddress(culled ? view.visibleDraws[frame] : drawList.buffers[frame]);
        sceneData.instancesPtr = ctx.getBufferDeviceAddress(gpuScene->instances.buffer(frame));
        sceneData.materialsPtr = ctx.getBufferDeviceAddress(gpuScene->materials.buffer(frame));
        sceneData.geometryPtr = ctx.getBufferDeviceAddress(geometryPool->tableBuffer());
        sceneData.allDrawsPtr = ctx.getBufferDeviceAddress(drawList.buffers[frame]);
        if (culled)
        {
            sceneData.cullSlotsPtr = sceneData.drawInfoPtr + layout.cullSlots;
            sceneData.groupCountsPtr = sceneData.drawInfoPtr + layout.groupCounts;
            sceneData.drawCountsPtr = sceneData.drawInfoPtr + layout.drawCounts;
        }
        sceneData.meshletsPtr = ctx.getBufferDeviceAddress(geometryPool->meshletBuffer());
        if (view.meshShaders)
            sceneData.meshletDataPtr = ctx.getBufferDeviceAddress(geometryPool->meshletDataBuffer());
        if (view.meshletRuns != 0)
        {
            sceneData.meshletDrawsPtr = sceneData.drawInfoPtr + layout.meshletDraws;
            sceneData.meshletChunksPtr = sceneData.drawInfoPtr + layout.chunks;
            sceneData.chunkMasksPtr = sceneData.drawInfoPtr + layout.chunkMasks;
            sceneData.chunkSumsPtr = sceneData.drawInfoPtr + layout.chunkSums;
            sceneData.meshletArgsPtr = sceneData.drawInfoPtr + layout.meshletArgs;
            sceneData.meshletDrawCountsPtr = sceneData.drawInfoPtr + layout.meshletDrawCounts;
            sceneData.groupMeshletsPtr = sceneData.drawInfoPtr + layout.groupMeshlets;
            sceneData.meshletRuns = view.meshletRuns;
            for (uint32_t i = 0; i < gfx::MAX_DRAW_RUNS; ++i)
            {
                sceneData.runChunkFirst[i] = runChunkFirst[i];
                sceneData.runMeshletFirst[i] = view.runMeshletFirst[i];
                sceneData.runMeshletBudget[i] = runMeshletBudget[i];
            }
            // The cone test holds where the run culls back faces (or front faces, for a mirroring instance).
            for (uint32_t i = 0; i < drawList.runs.size(); ++i)
                sceneData.coneRuns |= drawList.runs[i].cullMode != CullMode::NONE ? 1u << i : 0u;
        }
        sceneData.viewFlags = view.depthOnly ? gfx::VIEW_DEPTH_ONLY : 0u;
        // Culling and the light clusters count into it.
        sceneData.cullStatsPtr = ctx.getBufferDeviceAddress(gpuScene->cullStats[frame]);
        if (view.hizLevelCount > 0)
        {
            sceneData.visibilityPtr = ctx.getBufferDeviceAddress(view.visibility);
            sceneData.hizPtr = ctx.getBufferDeviceAddress(view.hiz[frame]);
            if (view.meshletVisibility.isValid())
                sceneData.meshletVisibilityPtr = ctx.getBufferDeviceAddress(view.meshletVisibility);
        }
        sceneData.colorsPtr = ctx.getBufferDeviceAddress(geometryPool->colorBuffer());
        sceneData.pointsPtr = geometryPool->pointBuffer().isValid() ? ctx.getBufferDeviceAddress(geometryPool->pointBuffer()) : 0;
        sceneData.animatedTransformsPtr = ctx.getBufferDeviceAddress(gpuAnimation->transforms[frame]);
        sceneData.skinnedVertsPtr = ctx.getBufferDeviceAddress(gpuAnimation->skinnedVertices[frame]);
        sceneData.previousSkinnedVertsPtr = ctx.getBufferDeviceAddress(gpuAnimation->skinnedVertices[(frame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT]);
        if (view.sortBlend)
        {
            const uint32_t sortCapacity = getBlendSortCapacity(drawList.runs[view.blendRun].count);
            sceneData.sortKeysPtr = ctx.getBufferDeviceAddress(view.sortBuffers[frame]);
            sceneData.sortDrawsPtr = sceneData.sortKeysPtr + static_cast<uint64_t>(sortCapacity) * 2 * sizeof(uint32_t);
            sceneData.sortCapacity = sortCapacity;
        }
        sceneData.drawCount = drawCount;
        sceneData.runCount = static_cast<uint32_t>(drawList.runs.size());
        for (uint32_t i = 0; i < sceneData.runCount; ++i)
            sceneData.runFirst[i] = drawList.runs[i].first;
        // Culling keeps the blend run for the late phase only, and the Transparent pass draws it. A depth-only view
        // draws it like any other run, so it has no blend run.
        sceneData.blendRun = view.late || view.hizLevelCount == 0 ? view.blendRun : gfx::MAX_DRAW_RUNS;
        // Culling cuts the point run's draws to a prefix (cull.slang), and leaves them out of a depth-only view.
        sceneData.pointRun = gfx::MAX_DRAW_RUNS;
        for (uint32_t i = 0; i < sceneData.runCount; ++i)
            sceneData.pointRun = drawList.runs[i].points ? i : sceneData.pointRun;
        sceneData.hizLevelCount = view.hizLevelCount;
        sceneData.depthTexture = depthSlot;
        sceneData.depthSamples = renderTargetData.samples;
        sceneData.viewportWidth = width;
        sceneData.viewportHeight = height;
        sceneData.lodErrorPixels = config.lodErrorPixels;
        sceneData.forcedLod = config.forcedLod;
        // The shaded view's shadow maps, in one table: each shadowed light names its row, its first map and their
        // count. A point light's cube has its faces' projection; the shader makes the view from the light's position.
        uint32_t mapCount = 0;
        for (uint32_t s = 0; !view.depthOnly && s < view.shadowCount; ++s)
        {
            if (!shadowLit[s])
                continue;
            const SceneShadow &shadow = view.shadows[s];
            const bool cube = registry.lights[shadow.light.index].type == LightType::POINT;
            sceneData.shadowedLights[sceneData.shadowedLightCount++] = {.light = static_cast<uint32_t>(shadow.light.index), .firstMap = mapCount, .mapCount = shadow.mapCount};
            for (uint32_t i = 0; i < shadow.mapCount; ++i)
            {
                gfx::ShadowMap &map = sceneData.shadowMaps[mapCount++];
                map.matrix = cube ? shadowMaps[s][i].projection : toRenderMatrix(glm::dmat4(shadowMaps[s][i].projection) * shadowMaps[s][i].view, origin);
                map.texture = getBindlessTextureIndex(ctx.getRenderTarget(shadow.maps[i]).attachments[frame].depth);
                map.split = splits[s][i];
            }
        }

        // A depth-only view shades nothing, so it has no environment and no lights.
        if (!view.depthOnly)
            writeSceneLights(viewIndex, view.scene, sceneData);
        const bool sky = writeSceneEnvironment(*environments, view.depthOnly ? SceneEnvironment{} : sceneRef.environment, sceneData) && !(exportFrame != nullptr && exportFrame->transparent);
        if (!view.depthOnly)
            sceneData.grid = makeGridData(cameraRef.grid, origin);

        // LODs are chosen in the pixels sizes are given in: an export's pixelScale output pixels each.
        sceneData.lodPixelsPerUnit = std::abs(cameraProjection[1][1]) * 0.5f * static_cast<float>(cameraHeight) / sceneData.pixelScale;
        if (cameraProjection[3][3] == 0.0f)
        {
            const glm::mat4 lodViewProjection = cameraProjection * cameraView;
            sceneData.lodDepthPlane = glm::vec4(lodViewProjection[0][3], lodViewProjection[1][3], lodViewProjection[2][3], lodViewProjection[3][3]);
        }
        // The frozen values stay in the render space of the frame that froze them, and move into this frame's.
        if (frozen && !view.frozen)
        {
            std::copy(sceneData.frustumPlanes, sceneData.frustumPlanes + 6, view.frozenPlanes);
            view.frozenOrigin = origin;
            view.frozenLodPixelsPerUnit = sceneData.lodPixelsPerUnit;
            view.frozenLodDepthPlane = sceneData.lodDepthPlane;
            view.frozenViewProjection = glm::dmat4(cameraProjection) * worldCameraView;
            view.frozenViewPoint = sceneData.cullViewPoint;
        }
        view.frozen = frozen;
        if (frozen)
        {
            for (uint32_t i = 0; i < 6; ++i)
                sceneData.frustumPlanes[i] = moveRenderPlane(view.frozenPlanes[i], view.frozenOrigin, origin);
            sceneData.cullViewPoint = view.frozenViewPoint;
            if (view.frozenViewPoint.w != 0.0f)
                sceneData.cullViewPoint = glm::vec4(toRenderPosition(toWorldPosition(glm::vec3(view.frozenViewPoint), view.frozenOrigin), origin), 1.0f);
            sceneData.lodPixelsPerUnit = view.frozenLodPixelsPerUnit;
            sceneData.lodDepthPlane = moveRenderPlane(view.frozenLodDepthPlane, view.frozenOrigin, origin);
        }
        if (frozenReader != nullptr)
        {
            sceneData.lodPixelsPerUnit = frozenReader->frozenLodPixelsPerUnit;
            sceneData.lodDepthPlane = moveRenderPlane(frozenReader->frozenLodDepthPlane, frozenReader->frozenOrigin, origin);
        }
        sceneData.debugView = static_cast<uint32_t>(view.debugView);
        view.labelDepthSlots = 0;
        if (!view.depthOnly && !drawList.labelEntities.empty() && !gpuScene->labelDepthPipeline.isValid())
        {
            gpuScene->labelDepthPipeline = createComputePipeline("labelDepth", readFile(pathFromUtf8(config.shaderDir) / "passes/labelDepth.slang"), "labelDepthMain");
            if (!gpuScene->labelDepthPipeline.isValid())
                spdlog::error("Failed to create the Label Depth pipeline; labels are hidden glyph by glyph by the depth.");
        }
        if (!view.depthOnly && !drawList.labelEntities.empty())
            placeViewLabels(viewIndex, cameraProjection, worldCameraView, cameraWidth, cameraHeight, sceneData);
        preparePointRaster(viewIndex, sceneData);
        prepareGlyphCull(viewIndex, sceneData);
        prepareWeightedBlend(viewIndex, sceneData);
        ctx.fillBuffer(gpuScene->sceneData[frame], &sceneData, sizeof(sceneData), viewIndex * sizeof(gfx::SceneData));
        view.sceneData = sceneData;
        view.projection = cameraProjection;
        view.origin = origin;

        // cull.slang's groups each lie inside one run.
        if (culled)
        {
            for (const DrawRun &run : drawList.runs)
                view.groupCount += (run.count + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE;
        }

        // Culling counts its visible draws on the GPU; an unculled view draws all of them.
        DrawTotals &totals = gpuScene->drawTotals[frame];
        totals.draws += drawCount;
        totals.triangles += drawList.triangleCount;
        if (!culled)
        {
            totals.unculledDraws += drawCount;
            totals.unculledTriangles += drawList.triangleCount;
        }

        view.width = width;
        view.height = height;
        view.culled = culled;
        view.sky = sky;
        view.grid = sceneData.grid.levelCount != 0;
        view.ready = true;
    }

    // A shaded view's labels for this frame (src/renderer/labelPlacement.cpp): each label's box on screen from its
    // anchor (doubles, world space) and its material's text size, the greedy placement with its fade, then the slots'
    // alpha into the view's buffer for this frame slot, which the vertex stage reads (labelsPtr), and each anchor's
    // depth test, which the Label Depth pass runs on it (recordLabelDepth). Allocates nothing once the vectors and the
    // buffers have grown.
    void Renderer::placeViewLabels(uint32_t viewIndex, const glm::mat4 &projection, const glm::dmat4 &worldView, uint32_t width, uint32_t height, gfx::SceneData &sceneData)
    {
        ZoneScoped;
        SceneView &view = gpuScene->views[viewIndex];
        const SceneDrawList &drawList = gpuScene->drawLists[view.scene.index];
        const uint32_t frame = static_cast<uint32_t>(frameCount);
        const bool perspective = projection[3][3] == 0.0f;
        const double pixelScale = 2.0 / (std::abs(static_cast<double>(projection[1][1])) * height);
        // An export places its labels once on the whole image, with sizes in its pixelScale, at once (no fade), and its
        // probes test each anchor in the tile that keeps it.
        const ExportViewFrame *exportFrame = exports ? getExportViewFrame(*exports, view.scene, view.camera, view.renderTarget) : nullptr;
        const double sizeScale = exportFrame != nullptr ? exportFrame->pixelScale : 1.0;
        std::vector<LabelCandidate> &candidates = view.labelCandidates;
        candidates.clear();
        uint32_t lowSlot = UINT32_MAX;
        uint32_t highSlot = 0;
        for (const EntityHandle entity : drawList.labelEntities)
        {
            if (!isLive(entity))
                continue;
            const EntityDrawState &state = gpuScene->entities[entity.index];
            const Entity &entityRef = registry.entities[entity.index];
            if (state.labelCount == 0 || !isLive(entityRef.mesh))
                continue;
            const Mesh &mesh = registry.meshes[entityRef.mesh.index];
            const Material &material = getMaterial(entityRef.material);
            const VectorMaterial &style = material.vector;
            // Labels that draw nothing (no VECTOR material, no live font) take no room.
            if (material.type != MaterialType::VECTOR || !isLive(style.font))
                continue;
            const glm::dmat4 model = entityRef.transform.toMatrix();
            const bool overlay = style.depth == VectorDepth::OVERLAY;
            for (uint32_t l = 0; l < state.labelCount && l < mesh.labels.size(); ++l)
            {
                const MeshLabel &label = mesh.labels[l];
                const glm::dvec4 viewPosition = worldView * (model * glm::dvec4(glm::dvec3(label.anchor), 1.0));
                const glm::dvec4 clip = glm::dmat4(projection) * viewPosition;
                const auto slot = static_cast<uint32_t>(state.firstLabel) + l;
                LabelCandidate candidate{.slot = slot, .priority = label.priority};
                if (clip.w > 0.0)
                {
                    const glm::dvec2 ndc = glm::dvec2(clip) / clip.w;
                    // The text's size in pixels: the style's, or its metres at the anchor's depth.
                    const double pixelWorld = pixelScale * (perspective ? clip.w : 1.0);
                    const bool metres = style.widthUnit == VectorUnit::METRES;
                    const double textPixels = metres ? style.width / pixelWorld : style.width * sizeScale;
                    const double haloPixels = metres ? style.outlineWidth / pixelWorld : style.outlineWidth * sizeScale;
                    const auto center = glm::vec2((ndc.x * 0.5 + 0.5) * width, (0.5 - ndc.y * 0.5) * height);
                    const auto halfSize = glm::vec2(glm::dvec2(label.halfSize) * textPixels + haloPixels);
                    const auto boxCenter = center + glm::vec2(label.center.x, -label.center.y) * static_cast<float>(textPixels);
                    candidate.low = boxCenter - halfSize;
                    candidate.high = boxCenter + halfSize;
                    candidate.inView = std::abs(ndc.x) <= 1.0 && std::abs(ndc.y) <= 1.0;
                    // The depth the anchor must be in front of: its own, moved toward the camera by a few pixels' size
                    // there (reversed-Z: a larger depth is nearer), for any projection. An anchor off the screen keeps
                    // its fade.
                    const glm::dvec4 nearer = glm::dmat4(projection) * (viewPosition + glm::dvec4(0.0, 0.0, LABEL_DEPTH_PIXELS * sizeScale * pixelWorld, 0.0));
                    const float test = overlay ? LABEL_PROBE_OVERLAY : LABEL_PROBE_DEPTH;
                    if (candidate.inView)
                        candidate.probe = glm::vec4(center, static_cast<float>(nearer.z / nearer.w), test);
                }
                candidates.push_back(candidate);
                lowSlot = std::min(lowSlot, slot);
                highSlot = std::max(highSlot, slot);
            }
        }
        if (candidates.empty())
            return;
        // The buffers hold every slot up to the highest in use and grow to twice that, under the same handles, which the
        // frame's passes name. The fades stay in place across frames; when their buffer grows, every label starts its
        // fade over.
        const uint32_t needed = highSlot + 1;
        const uint32_t capacity = std::min(std::max(needed * 2, 1024u), MAX_LABELS);
        const bool fadesGrow = view.labelFadeCapacity < needed;
        if (fadesGrow)
            view.labelFadeCapacity = ctx.resizeBuffer(view.labelFades, static_cast<uint64_t>(capacity) * sizeof(float)) ? capacity : 0;
        // A label that was hidden (or is new to the view) starts its fade by depth at its test's answer.
        for (LabelCandidate &candidate : candidates)
        {
            const bool hidden = candidate.slot >= view.labels.alpha.size() || view.labels.alpha[candidate.slot] <= 0.0f || view.labels.stamps[candidate.slot] != gpuScene->labelStamps[candidate.slot];
            if ((hidden || fadesGrow) && candidate.probe.w != LABEL_PROBE_NONE)
                candidate.probe.w += LABEL_PROBE_RESET;
        }
        placeLabels(view.labels, candidates.data(), static_cast<uint32_t>(candidates.size()), gpuScene->labelStamps.data(), MAX_LABELS, width, height,
                    exportFrame != nullptr ? 1.0f : deltaTime / LABEL_FADE_SECONDS, LABEL_CELL_PIXELS * static_cast<float>(sizeScale));

        if (view.labelCapacity[frame] < needed)
        {
            const bool grown = ctx.resizeBuffer(view.labelBuffers[frame], static_cast<uint64_t>(capacity) * sizeof(float)) &&
                               ctx.resizeBuffer(view.labelProbes[frame], static_cast<uint64_t>(capacity) * sizeof(glm::vec4));
            view.labelCapacity[frame] = grown ? capacity : 0;
        }
        if (view.labelCapacity[frame] < needed ||
            !ctx.fillBuffer(view.labelBuffers[frame], view.labels.alpha.data() + lowSlot, static_cast<size_t>(needed - lowSlot) * sizeof(float), static_cast<size_t>(lowSlot) * sizeof(float)))
            return;
        sceneData.labelsPtr = ctx.getBufferDeviceAddress(view.labelBuffers[frame]);

        // The slots between the view's labels belong to other scenes: the pass leaves them.
        std::vector<glm::vec4> &probes = view.labelProbeData;
        // Labels that are all overlays, or off the screen, need no pass.
        probes.assign(needed - lowSlot, glm::vec4(0.0f, 0.0f, 0.0f, LABEL_PROBE_NONE));
        bool tested = false;
        for (const LabelCandidate &candidate : candidates)
        {
            glm::vec4 probe = exportFrame != nullptr ? getExportLabelProbe(*exportFrame, candidate.probe) : candidate.probe;
            probes[candidate.slot - lowSlot] = probe;
            tested = tested || probe.w == LABEL_PROBE_DEPTH || probe.w == LABEL_PROBE_DEPTH + LABEL_PROBE_RESET;
        }
        if (!tested || !gpuScene->labelDepthPipeline.isValid() || view.labelFadeCapacity < needed || sceneData.depthTexture == UINT32_MAX || ctx.getRenderTargetDesc(view.renderTarget).cube ||
            !ctx.fillBuffer(view.labelProbes[frame], probes.data(), probes.size() * sizeof(glm::vec4), static_cast<size_t>(lowSlot) * sizeof(glm::vec4)))
            return;
        sceneData.labelProbesPtr = ctx.getBufferDeviceAddress(view.labelProbes[frame]);
        sceneData.labelFadesPtr = ctx.getBufferDeviceAddress(view.labelFades);
        sceneData.labelFadeStep = exportFrame != nullptr ? 0.0f : deltaTime / LABEL_FADE_SECONDS;
        sceneData.labelSlotCount = needed;
        view.labelDepthFirst = lowSlot;
        view.labelDepthSlots = needed;
    }

    // countMain, then writeMain, which reads the counts and slots countMain wrote. The early phase draws what was
    // visible last frame; the late one, after the Hi-Z, what the early one left out.
    void Renderer::recordSceneCulling(uint32_t viewIndex, uint32_t phase)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready || !view.culled)
            return;

        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex), .argument = phase};
        if (phase == CULL_EARLY)
        {
            recordPointReset(viewIndex);
            if (view.drawGlyphs)
                recordGlyphReset(pushConstants.scene);
        }
        ctx.bindPipeline(commandBuffer, gpuScene->cullCountPipeline);
        ctx.pushConstants(commandBuffer, gpuScene->cullCountPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, view.groupCount);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        ctx.bindPipeline(commandBuffer, gpuScene->cullWritePipeline);
        ctx.pushConstants(commandBuffer, gpuScene->cullWritePipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, view.groupCount);
        if (view.meshletRuns != 0 && !view.meshShaders)
            recordMeshletCulling(viewIndex, phase);
        // The view's last cull wrote every batch of the frame: the points that take the compute path are written now.
        if (phase == CULL_LATE || !view.late)
        {
            recordPointRaster(viewIndex);
            if (view.drawGlyphs)
                recordGlyphCull(pushConstants.scene, view.glyphCull[frameCount], 0);
        }

        // The phase that keeps the blend run's draws: the late one, or the only one in a frame without a Hi-Z.
        if (view.sortBlend && (phase == CULL_LATE || view.hizLevelCount == 0))
        {
            ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
            recordBlendSort(viewIndex);
        }
    }

    // The meshlets of the chunks writeMain wrote: tested and masked per chunk, a run at a time (the chunk count is the
    // GPU's, so the dispatches are indirect), then written as draws. Each step reads what the one before wrote.
    void Renderer::recordMeshletCulling(uint32_t viewIndex, uint32_t phase)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        const auto frame = static_cast<uint32_t>(frameCount);
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const VisibleDrawLayout layout = getVisibleDrawLayout(view, frame);
        const auto runCount = static_cast<uint32_t>(gpuScene->drawLists[view.scene.index].runs.size());
        gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, frame, viewIndex), .argument = phase};
        auto dispatchRuns = [&](PipelineHandle pipeline)
        {
            ctx.bindPipeline(commandBuffer, pipeline);
            for (uint32_t run = 0; run < runCount; ++run)
            {
                if ((view.meshletRuns & (1u << run)) == 0)
                    continue;
                pushConstants.argument = phase | run << MESHLET_RUN_SHIFT;
                ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
                ctx.dispatchIndirect(commandBuffer, view.visibleDraws[frame], layout.meshletArgs + static_cast<uint64_t>(run) * MESHLET_ARGS_SIZE);
            }
        };

        const uint32_t timer = beginGpuTimer(commandBuffer, "Meshlet Cull");
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        dispatchRuns(gpuScene->meshletCountPipeline);
        ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
        dispatchRuns(gpuScene->meshletWritePipeline);
        endGpuTimer(commandBuffer, timer);
    }

    // A bitonic sort of the blend run's visible draws, back to front, in place (shaders/passes/sort.slang). Its size
    // is the run's, not its visible count, which only the GPU knows: the draws past the count sort to the end.
    void Renderer::recordBlendSort(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        const auto frame = static_cast<uint32_t>(frameCount);
        recordBlendSort(getSceneDataAddress(ctx, *gpuScene, frame, viewIndex), getBlendSortCapacity(gpuScene->drawLists[view.scene.index].runs[view.blendRun].count));
    }

    // The sort of a SceneData's blend run (a view's, or a pick's: picking.cpp), whose sort buffers hold capacity keys.
    void Renderer::recordBlendSort(uint64_t sceneDataAddress, uint32_t capacity)
    {
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        BlendSortDispatch dispatches[MAX_BLEND_SORT_DISPATCHES];
        const uint32_t dispatchCount = getBlendSortDispatches(capacity, dispatches, MAX_BLEND_SORT_DISPATCHES);
        // A cut plan would leave the draws half sorted or skip the write; unsorted draws are still valid. Every
        // capacity getBlendSortCapacity gives fits today (tests/blendSortTests.cpp).
        if (dispatchCount > MAX_BLEND_SORT_DISPATCHES)
        {
            spdlog::error("The blend sort of {} draws needs {} dispatches, more than {}; see-through materials are drawn unsorted.", capacity, dispatchCount, MAX_BLEND_SORT_DISPATCHES);
            return;
        }

        const uint32_t timer = beginGpuTimer(commandBuffer, "Blend Sort");
        gfx::PushConstants pushConstants{.scene = sceneDataAddress};
        for (uint32_t i = 0; i < dispatchCount; ++i)
        {
            if (i > 0)
                ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
            const PipelineHandle pipeline = gpuScene->sortPipelines[static_cast<uint32_t>(dispatches[i].step)];
            pushConstants.argument = dispatches[i].argument;
            ctx.bindPipeline(commandBuffer, pipeline);
            ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, dispatches[i].groups);
        }
        endGpuTimer(commandBuffer, timer);
    }

    // Each level reads the one before, level 0 the depth the early draws left.
    void Renderer::recordHiz(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.late)
            return;

        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        ctx.bindPipeline(commandBuffer, gpuScene->hizPipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->hizPipeline, bindlessDescriptorSet, 0);
        uint32_t levelWidth = view.width;
        uint32_t levelHeight = view.height;
        for (uint32_t level = 0; level < view.hizLevelCount; ++level)
        {
            if (level > 0)
                ctx.memoryBarrier(commandBuffer, PipelineStage::COMPUTE, PipelineStage::COMPUTE);
            levelWidth = (levelWidth + 1) / 2;
            levelHeight = (levelHeight + 1) / 2;
            pushConstants.argument = level;
            ctx.pushConstants(commandBuffer, gpuScene->hizPipeline, &pushConstants, sizeof(pushConstants), 0);
            ctx.dispatch(commandBuffer, (levelWidth + HIZ_GROUP_SIZE - 1) / HIZ_GROUP_SIZE, (levelHeight + HIZ_GROUP_SIZE - 1) / HIZ_GROUP_SIZE);
        }
    }

    // Each label slot of the view: whether its anchor is in front of the depth, as a fade kept across frames, times its
    // placement's alpha in the slot's label buffer, which the Transparent pass reads (shaders/passes/labelDepth.slang).
    void Renderer::recordLabelDepth(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready || view.labelDepthSlots == 0)
            return;
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        // The slots from the view's first to its last, the ones placeViewLabels wrote a probe for.
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex), .argument = view.labelDepthFirst};
        ctx.bindPipeline(commandBuffer, gpuScene->labelDepthPipeline);
        ctx.bindDescriptorSet(commandBuffer, gpuScene->labelDepthPipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, gpuScene->labelDepthPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.dispatch(commandBuffer, (view.labelDepthSlots - view.labelDepthFirst + LABEL_DEPTH_GROUP_SIZE - 1) / LABEL_DEPTH_GROUP_SIZE);
    }

    // Culling packs a run's visible draws at the start of its range and counts them, so each run is one indirect
    // draw with a count; an unculled view draws its whole list. The phase is the culling view's early or late color.
    void Renderer::drawSceneView(uint32_t viewIndex, uint32_t phase)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready)
            return;

        const auto frame = static_cast<uint32_t>(frameCount);
        const auto &drawList = gpuScene->drawLists[view.scene.index];
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const VisibleDrawLayout layout = getVisibleDrawLayout(view, frame);
        ctx.bindIndexBuffer(commandBuffer, geometryPool->indexBuffer(), 0, Format::U32_UINT);

        // Opaque draws use the pipeline that never discards; a depth-only view draws MASK and BLEND with its cut-out one,
        // and a shaded view leaves BLEND to the Transparent pass.
        PipelineHandle bound = {};
        const ShadedPipelines &shaded = getShadedPipelines(*gpuScene, ctx, view);
        const DebugPipelines &debug = getDebugPipelines(shaded, view.debugView);
        gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex), .argument = phase};
        for (uint32_t i = 0; i < drawList.runs.size(); ++i)
        {
            const DrawRun &run = drawList.runs[i];
            if (i == view.blendRun || i == view.weightedRun || i == view.additiveRun || (run.alphaMode != AlphaMode::NONE && !view.drawMask) ||
                (run.points && (view.depthOnly || !shaded.pointPipeline.isValid())))
                continue;
            PipelineHandle pipeline = view.depthOnly ? gpuScene->depthPipeline : shaded.pipeline;
            if (run.alphaMode != AlphaMode::NONE)
                pipeline = view.depthOnly ? gpuScene->depthMaskPipeline : shaded.maskPipeline;
            if (view.debugView != DebugView::NONE)
                pipeline = run.alphaMode != AlphaMode::NONE ? debug.maskPipeline : debug.pipeline;
            // A debug view shows the points as they are.
            if (run.points)
                pipeline = shaded.pointPipeline;
            if (pipeline != bound)
            {
                ctx.bindPipeline(commandBuffer, pipeline);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
                bound = pipeline;
            }
            pushConstants.firstDraw = run.first;
            // Through a mirrored view the front faces turn clockwise, so the side to cull swaps.
            CullMode cullMode = run.cullMode;
            if (view.mirrored && cullMode != CullMode::NONE)
                cullMode = cullMode == CullMode::BACK ? CullMode::FRONT : CullMode::BACK;
            ctx.setCullMode(commandBuffer, cullMode);
            ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
            if (view.culled)
                ctx.drawIndexedIndirectCount(commandBuffer, view.visibleDraws[frame], run.first * sizeof(gfx::DrawInfo), view.visibleDraws[frame],
                                             layout.drawCounts + i * sizeof(uint32_t), run.count, sizeof(gfx::DrawInfo));
            else
                ctx.drawIndexedIndirect(commandBuffer, drawList.buffers[frame], run.first * sizeof(gfx::DrawInfo), run.count, sizeof(gfx::DrawInfo));
            // With mesh shaders, the run's task draw: a task workgroup per chunk of meshlets (lib/meshletTask.slang),
            // with the group counts writeMain wrote for the vertex-pulling dispatches. The run is in the argument.
            if ((view.meshletRuns & (1u << i)) != 0 && view.meshShaders)
            {
                const bool mask = run.alphaMode != AlphaMode::NONE;
                PipelineHandle meshPipeline = mask ? shaded.meshMaskPipeline : shaded.meshPipeline;
                if (view.depthOnly)
                    meshPipeline = mask ? gpuScene->depthMeshMaskPipeline : gpuScene->depthMeshPipeline;
                else if (view.debugView != DebugView::NONE)
                    meshPipeline = mask ? debug.meshMaskPipeline : debug.meshPipeline;
                ctx.bindPipeline(commandBuffer, meshPipeline);
                ctx.bindDescriptorSet(commandBuffer, meshPipeline, bindlessDescriptorSet, 0);
                bound = meshPipeline;
                gfx::PushConstants taskConstants = pushConstants;
                taskConstants.firstDraw = 0;
                taskConstants.argument = phase | i << MESHLET_RUN_SHIFT;
                ctx.pushConstants(commandBuffer, meshPipeline, &taskConstants, sizeof(taskConstants), 0);
                ctx.drawMeshTasksIndirect(commandBuffer, view.visibleDraws[frame], layout.meshletArgs + static_cast<uint64_t>(i) * MESHLET_ARGS_SIZE, 1, MESHLET_ARGS_SIZE);
            }
            // The run's visible meshlets, after the draws in the same buffer: the vertex shader reads either one as a
            // DrawInfo at drawInfoPtr.
            else if ((view.meshletRuns & (1u << i)) != 0)
            {
                pushConstants.firstDraw = view.visibleCapacity[frame] + view.runMeshletFirst[i];
                ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
                ctx.drawIndexedIndirectCount(commandBuffer, view.visibleDraws[frame], static_cast<uint64_t>(pushConstants.firstDraw) * sizeof(gfx::DrawInfo), view.visibleDraws[frame],
                                             layout.meshletDrawCounts + i * sizeof(uint32_t), view.runMeshletSize[i], sizeof(gfx::DrawInfo));
            }
        }
        // beginPass sets NONE for every other pipeline.
        ctx.setCullMode(commandBuffer, CullMode::NONE);
    }

    // The task and mesh pipelines a view's meshlet runs need, made the first time like the MASK ones: the opaque runs'
    // and the MASK runs', shaded, in the view's debug view, or depth-only. False while one is missing (a shader error
    // retries next frame), and the view then draws its meshlets by vertex pulling (whole with AUTO).
    bool Renderer::createMeshletPipelines(uint32_t viewIndex, bool opaque, bool mask)
    {
        SceneView &view = gpuScene->views[viewIndex];
        if (view.depthOnly)
        {
            if (opaque && !gpuScene->depthMeshPipeline.isValid())
                gpuScene->depthMeshPipeline = createMeshScenePipeline("shadowMesh", "passes/shadow.slang", "", view.renderTarget, "meshMain", "fragMain", BlendMode::NONE, true);
            if (mask && !gpuScene->depthMeshMaskPipeline.isValid())
                gpuScene->depthMeshMaskPipeline = createMeshScenePipeline("shadowMeshMask", "passes/shadow.slang", "", view.renderTarget, "meshMask", "fragMask", BlendMode::NONE, true);
            return (!opaque || gpuScene->depthMeshPipeline.isValid()) && (!mask || gpuScene->depthMeshMaskPipeline.isValid());
        }

        ShadedPipelines &shaded = getShadedPipelines(*gpuScene, ctx, view);
        const bool debugVariant = view.debugView != DebugView::NONE;
        PipelineHandle &opaquePipeline = debugVariant ? (view.debugView == DebugView::OVERDRAW ? shaded.overdraw : shaded.debug).meshPipeline : shaded.meshPipeline;
        PipelineHandle &maskPipeline = debugVariant ? (view.debugView == DebugView::OVERDRAW ? shaded.overdraw : shaded.debug).meshMaskPipeline : shaded.meshMaskPipeline;
        const bool missing = (opaque && !opaquePipeline.isValid()) || (mask && !maskPipeline.isValid());
        if (missing)
        {
            // The same variants as the vertex pipelines of drawSceneView, so the image does not depend on the path.
            const bool multisampled = ctx.getRenderTarget(view.renderTarget).samples > 1;
            const BlendMode blendMode = view.debugView == DebugView::OVERDRAW ? BlendMode::ADDITIVE : BlendMode::NONE;
            const std::string defines = getMotionDefine(ctx, view) + std::string(debugVariant ? "#define RASM_DEBUG_VIEW 1\n" : "");
            const bool coverage = multisampled && !debugVariant;
            const std::string maskDefines = defines + (coverage ? "#define RASM_ALPHA_MASK 1\n#define RASM_ALPHA_TO_COVERAGE 1\n" : "#define RASM_ALPHA_MASK 1\n");
            // A debug view's are not warmed: until its shader workers have them, its meshlets take the other path.
            bool pending = false;
            bool maskPending = false;
            if (opaque && !opaquePipeline.isValid())
                opaquePipeline = createMeshScenePipeline(debugVariant ? "rasmDebugMesh" : "rasmMesh", "passes/rasm.slang", defines, view.renderTarget, "meshMain", "fragMain", blendMode, true, false,
                                                         debugVariant ? &pending : nullptr);
            if (mask && !maskPipeline.isValid())
                maskPipeline = createMeshScenePipeline(debugVariant ? "rasmDebugMeshMask" : "rasmMeshMask", "passes/rasm.slang", maskDefines, view.renderTarget, "meshMain", "fragMain", blendMode, true,
                                                       coverage, debugVariant ? &maskPending : nullptr);
            if ((opaque && !opaquePipeline.isValid() && !pending) || (mask && !maskPipeline.isValid() && !maskPending))
                spdlog::error("Failed to create the mesh shader pipelines; meshlets are drawn by vertex pulling, or whole with MeshletPath::AUTO.");
        }
        return (!opaque || opaquePipeline.isValid()) && (!mask || maskPipeline.isValid());
    }

    // The blend run, after the view's draws and its sky: depth tested against them, but not written, so each see-through
    // surface blends over what is behind it, in the order the sort left (back to front; unsorted without culling). A
    // debug view shows the weighted and additive runs the same way, unsorted, before it: a mean of normals or LODs
    // would mean nothing.
    void Renderer::drawTransparent(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.ready)
            return;
        const ShadedPipelines &shaded = getShadedPipelines(*gpuScene, ctx, view);
        const bool debug = view.debugView != DebugView::NONE;
        const PipelineHandle blendPipeline = debug ? getDebugPipelines(shaded, view.debugView).blendPipeline : shaded.blendPipeline;
        const uint32_t runs[3] = {debug ? view.weightedRun : gfx::MAX_DRAW_RUNS, debug ? view.additiveRun : gfx::MAX_DRAW_RUNS, view.drawBlend ? view.blendRun : gfx::MAX_DRAW_RUNS};
        for (uint32_t run : runs)
        {
            if (run < gfx::MAX_DRAW_RUNS && blendPipeline.isValid())
                drawBlendRun(viewIndex, run, blendPipeline);
        }
    }

    // One of a shaded view's BLEND runs, culled by the late pass (or the only one), depth tested and not written.
    void Renderer::drawBlendRun(uint32_t viewIndex, uint32_t runIndex, PipelineHandle pipeline)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        const auto frame = static_cast<uint32_t>(frameCount);
        const DrawRun &run = gpuScene->drawLists[view.scene.index].runs[runIndex];
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, frame, viewIndex), .firstDraw = run.first, .argument = view.late ? CULL_LATE : CULL_EARLY};
        ctx.bindPipeline(commandBuffer, pipeline);
        ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
        ctx.bindIndexBuffer(commandBuffer, geometryPool->indexBuffer(), 0, Format::U32_UINT);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        ctx.pushConstants(commandBuffer, pipeline, &pushConstants, sizeof(pushConstants), 0);
        if (view.culled)
            ctx.drawIndexedIndirectCount(commandBuffer, view.visibleDraws[frame], run.first * sizeof(gfx::DrawInfo), view.visibleDraws[frame],
                                         getVisibleDrawLayout(view, frame).drawCounts + runIndex * sizeof(uint32_t), run.count, sizeof(gfx::DrawInfo));
        else
            ctx.drawIndexedIndirect(commandBuffer, gpuScene->drawLists[view.scene.index].buffers[frame], run.first * sizeof(gfx::DrawInfo), run.count, sizeof(gfx::DrawInfo));
    }

    // One triangle at the far plane, after the view's draws: the depth test passes only where they left the clear
    // depth (0, reversed-Z), so the sky costs only the pixels it shows.
    void Renderer::drawSky(uint32_t viewIndex)
    {
        // A debug view shows the target's clear color behind the scene, so a count starts at 0.
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.sky || view.debugView != DebugView::NONE)
            return;
        PipelineHandle &skyPipeline = getShadedPipelines(*gpuScene, ctx, view).skyPipeline;
        if (!skyPipeline.isValid())
        {
            skyPipeline = createScenePipeline("sky", "passes/sky.slang", getMotionDefine(ctx, view), view.renderTarget, "vertMain", "fragMain", BlendMode::NONE, true);
            if (!skyPipeline.isValid())
            {
                spdlog::error("Failed to create the sky pipeline; the scene has no sky.");
                return;
            }
        }
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        ctx.bindPipeline(commandBuffer, skyPipeline);
        ctx.bindDescriptorSet(commandBuffer, skyPipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, skyPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        ctx.draw(commandBuffer, 3);
    }

    // The camera's grid (grid.slang), first in the Transparent pass: one triangle over the target whose pixels find the
    // plane's depth and test it against the opaque draws and the sky, which it covers; the see-through draws go over it.
    void Renderer::drawGrid(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        if (!view.grid || view.debugView != DebugView::NONE)
            return;
        PipelineHandle &gridPipeline = getShadedPipelines(*gpuScene, ctx, view).gridPipeline;
        if (!gridPipeline.isValid())
        {
            gridPipeline = createScenePipeline("grid", "passes/grid.slang", getMotionDefine(ctx, view), view.renderTarget, "vertMain", "fragMain", BlendMode::PREMULTIPLIED_ALPHA, false);
            if (!gridPipeline.isValid())
            {
                spdlog::error("Failed to create the grid pipeline; the view has no grid.");
                return;
            }
        }
        const auto commandBuffer = frameResources[frameCount].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, static_cast<uint32_t>(frameCount), viewIndex)};
        ctx.bindPipeline(commandBuffer, gridPipeline);
        ctx.bindDescriptorSet(commandBuffer, gridPipeline, bindlessDescriptorSet, 0);
        ctx.pushConstants(commandBuffer, gridPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.setCullMode(commandBuffer, CullMode::NONE);
        ctx.draw(commandBuffer, 3);
    }

    // The culling view's ghosts (debugGhosts.slang): a box around every draw's bounding sphere, one instance per draw of
    // the scene's list, added over what the view drew. Its vertex shader drops the draws that culling kept.
    void Renderer::drawCullingGhosts(uint32_t viewIndex)
    {
        const SceneView &view = gpuScene->views[viewIndex];
        const PipelineHandle ghostPipeline = getShadedPipelines(*gpuScene, ctx, view).ghostPipeline;
        if (!view.ready || !view.culled || view.debugView != DebugView::CULLING || !ghostPipeline.isValid())
            return;

        const auto frame = static_cast<uint32_t>(frameCount);
        const auto commandBuffer = frameResources[frame].commandBuffer;
        const gfx::PushConstants pushConstants{.scene = getSceneDataAddress(ctx, *gpuScene, frame, viewIndex)};
        ctx.bindPipeline(commandBuffer, ghostPipeline);
        ctx.bindDescriptorSet(commandBuffer, ghostPipeline, bindlessDescriptorSet, 0);
        ctx.setCullMode(commandBuffer, CullMode::BACK);
        ctx.pushConstants(commandBuffer, ghostPipeline, &pushConstants, sizeof(pushConstants), 0);
        ctx.draw(commandBuffer, GHOST_BOX_VERTICES, static_cast<uint32_t>(gpuScene->drawLists[view.scene.index].draws.size()));
        ctx.setCullMode(commandBuffer, CullMode::NONE);
    }

    void Renderer::setMeshletPath(MeshletPath path)
    {
        if (path != MeshletPath::OFF && path != MeshletPath::VERTEX_PULLING && path != MeshletPath::MESH_SHADERS && path != MeshletPath::AUTO)
        {
            spdlog::error("setMeshletPath: {} is not a MeshletPath.", static_cast<uint32_t>(path));
            return;
        }
        // Kept as asked: prepareSceneView draws by vertex pulling where the device has no mesh shaders.
        if (path == MeshletPath::MESH_SHADERS && !ctx.hasMeshShaders() && config.meshletPath != MeshletPath::MESH_SHADERS)
            spdlog::warn("setMeshletPath: this GPU has no mesh shaders (VK_EXT_mesh_shader); meshlets are drawn by vertex pulling.");
        config.meshletPath = path;
    }

    MeshletPath Renderer::getMeshletPath() const
    {
        return config.meshletPath;
    }
}
