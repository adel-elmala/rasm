#include "renderer/pipelineWarmup.h"

#include "rasm/renderer.h"
#include "core/utils.h"
#include "platform/paths.h"
#include "renderer/geometryPool.h"
#include "renderer/gpuScene.h"
#include "renderer/shaderCompiler.h"

#include "tracy/Tracy.hpp"

#include <string>
#include <vector>

namespace rasm
{
    namespace
    {
        constexpr const char *MOTION_DEFINE = "#define RASM_MOTION_VECTORS 1\n";
        constexpr const char *MASK_DEFINE = "#define RASM_ALPHA_MASK 1\n";
        constexpr const char *COVERAGE_DEFINES = "#define RASM_ALPHA_MASK 1\n#define RASM_ALPHA_TO_COVERAGE 1\n";
    }

    namespace
    {
        // The stages' module names and entry points as createGraphicsPipeline, createMeshScenePipeline and
        // createComputePipeline compile them, so a warmed shader has the key the pipeline asks for.
        void addGraphics(std::vector<ShaderCompiler::Request> &requests, const std::string &name, const std::string &source, const char *vertexEntryPoint, const char *fragmentEntryPoint)
        {
            requests.push_back({.source = source, .outputPath = name + ".vert.slang", .entryPoint = vertexEntryPoint});
            requests.push_back({.source = source, .outputPath = name + ".frag.slang", .entryPoint = fragmentEntryPoint});
        }

        void addMesh(std::vector<ShaderCompiler::Request> &requests, const std::string &name, const std::string &source, const char *meshEntryPoint, const char *fragmentEntryPoint)
        {
            requests.push_back({.source = source, .outputPath = name + ".task.slang", .entryPoint = "taskMain"});
            requests.push_back({.source = source, .outputPath = name + ".mesh.slang", .entryPoint = meshEntryPoint});
            requests.push_back({.source = source, .outputPath = name + ".frag.slang", .entryPoint = fragmentEntryPoint});
        }

        void addCompute(std::vector<ShaderCompiler::Request> &requests, const std::string &name, const std::string &source, const char *entryPoint)
        {
            requests.push_back({.source = source, .outputPath = name + ".comp.slang", .entryPoint = entryPoint});
        }
    }

    // A variant's defines go before the file, and #line keeps the diagnostics' line numbers those of the file.
    std::string getShaderVariant(const std::string &defines, const std::string &file)
    {
        return defines.empty() ? file : defines + "#line 1\n" + file;
    }

    // The meshlet pipelines with task and mesh shaders can be made: the device has mesh shaders and the geometry pool
    // keeps the meshlet data they read.
    bool Renderer::hasMeshShaderPipelines() const
    {
        return ctx.hasMeshShaders() && geometryPool->meshletDataBuffer().isValid();
    }

    // Every shader a frame can need, queued at startup for the shader workers in about the order the first frames ask
    // for them; the ones a debug view adds are compiled when it is chosen (prepareSceneView). A pipeline then finds its
    // shaders ready, or waits only for the ones still compiling. The variants and names must be the ones the pipelines
    // are made with (gpuScene.cpp, renderer.cpp, ...): a shader asked for under another key is compiled on the main
    // thread, which the log says.
    void Renderer::warmPipelines()
    {
        ZoneScoped;

        const std::filesystem::path folder = pathFromUtf8(config.shaderDir);
        // What the first frame makes: the BRDF LUT, the tone map and the culling. The stats overlay and the UI are not here:
        // the constructor makes their pipelines right after this, on the main thread, while the workers start.
        std::vector<ShaderCompiler::Request> requests;
        const std::string environment = readFile(folder / "passes/environment.slang");
        addCompute(requests, "brdfLut", environment, "brdfMain");
        const std::string toneMap = readFile(folder / "passes/toneMap.slang");
        addGraphics(requests, "toneMap", toneMap, "vertMain", "fragMain");
        const std::string cull = readFile(folder / "passes/cull.slang");
        addCompute(requests, "cullCount", cull, "countMain");
        addCompute(requests, "cullWrite", cull, "writeMain");
        if (config.occlusionCulling)
            addCompute(requests, "hiz", readFile(folder / "passes/hiz.slang"), "hizMain");
        const std::string lights = readFile(folder / "passes/lightClusters.slang");
        addCompute(requests, "lightCull", lights, "lightsMain");
        addCompute(requests, "lightTiles", lights, "tilesMain");
        addCompute(requests, "lightClusters", lights, "clustersMain");
        shaderCompiler->prefetch(requests.data(), static_cast<uint32_t>(requests.size()), false);

        const AntiAliasing antiAliasing = config.antiAliasing;
        warmScenePipelines(antiAliasing == AntiAliasing::TAA, antiAliasing == AntiAliasing::MSAA_2X || antiAliasing == AntiAliasing::MSAA_4X || antiAliasing == AntiAliasing::MSAA_8X, false);

        // What a scene asks for once it has shadows, an environment, see-through or animated meshes, labels or points.
        requests.clear();
        const std::string shadow = readFile(folder / "passes/shadow.slang");
        addGraphics(requests, "shadow", shadow, "vertMain", "fragMain");
        addGraphics(requests, "shadowMask", shadow, "vertMask", "fragMask");
        // Whatever the configured meshlet path: setMeshletPath can choose another one at any frame.
        if (hasMeshShaderPipelines())
        {
            addMesh(requests, "shadowMesh", shadow, "meshMain", "fragMain");
            addMesh(requests, "shadowMeshMask", shadow, "meshMask", "fragMask");
        }
        addCompute(requests, "meshletCount", cull, "meshletCountMain");
        addCompute(requests, "meshletWrite", cull, "meshletWriteMain");
        addCompute(requests, "environmentCube", environment, "cubeMain");
        addCompute(requests, "environmentDownsample", environment, "downsampleMain");
        addCompute(requests, "environmentSpecular", environment, "specularMain");
        addCompute(requests, "environmentIrradiance", environment, "irradianceMain");
        const std::string sort = readFile(folder / "passes/sort.slang");
        for (const char *entryPoint : {"sortKeysMain", "sortMergeMain", "sortLocalMergeMain", "sortWriteMain"})
            addCompute(requests, "blendSort", sort, entryPoint);
        addCompute(requests, "animate", readFile(folder / "passes/animate.slang"), "animateMain");
        addCompute(requests, "skin", readFile(folder / "passes/skin.slang"), "skinMain");
        addCompute(requests, "labelDepth", readFile(folder / "passes/labelDepth.slang"), "labelDepthMain");
        const std::string bloom = readFile(folder / "passes/bloom.slang");
        addCompute(requests, "bloomFirstDownsample", bloom, "firstDownsampleMain");
        addCompute(requests, "bloomDownsample", bloom, "downsampleMain");
        addCompute(requests, "bloomUpsample", bloom, "upsampleMain");
        if (ctx.hasBufferInt64Atomics())
        {
            const std::string pointRaster = readFile(folder / "passes/pointRaster.slang");
            addCompute(requests, "pointReset", pointRaster, "resetMain");
            addCompute(requests, "pointClear", pointRaster, "clearMain");
            addCompute(requests, "pointRaster", pointRaster, "rasterMain");
        }
        // Last: what a debug view shows its pixels with, the Inspect pass of the RASM window, and the picks.
        const std::string glyphCull = readFile(folder / "passes/glyphCull.slang");
        addCompute(requests, "glyphReset", glyphCull, "resetMain");
        addCompute(requests, "glyphCount", glyphCull, "countMain");
        addCompute(requests, "glyphWrite", glyphCull, "writeMain");
        addGraphics(requests, "toneMapDebug", toneMap, "vertMain", "fragDebug");
        addGraphics(requests, "toneMapDebugHeat", toneMap, "vertMain", "fragDebugHeat");
        if (config.ui)
            addCompute(requests, "inspect", readFile(folder / "passes/inspect.slang"), "inspectMain");
        const std::string rasm = readFile(folder / "passes/rasm.slang");
        addGraphics(requests, "rasmPick", getShaderVariant("#define RASM_PICK 1\n", rasm), "vertMain", "pickMain");
        addGraphics(requests, "rasmPickMask", getShaderVariant(std::string("#define RASM_PICK 1\n") + MASK_DEFINE, rasm), "vertMain", "pickMain");
        addGraphics(requests, "rasmPickBlend", getShaderVariant("#define RASM_PICK 1\n#define RASM_ALPHA_BLEND 1\n", rasm), "vertMain", "pickMain");
        addGraphics(requests, "pointsPick", getShaderVariant("#define RASM_PICK 1\n", readFile(folder / "passes/points.slang")), "vertMain", "pickMain");
        addGraphics(requests, "rasmPickGlyphs", getShaderVariant("#define RASM_PICK 1\n#define RASM_GLYPHS 1\n", rasm), "vertMain", "pickMain");
        addCompute(requests, "export", readFile(folder / "passes/export.slang"), "exportMain");
        shaderCompiler->prefetch(requests.data(), static_cast<uint32_t>(requests.size()), false);
    }

    // The shaded pipelines of one kind of scene target: with a motion target (TAA) or not, with several samples or one.
    // Queued once per kind, at startup for the configured anti-aliasing and when a view of another kind is made, then
    // first, since that view draws in this frame.
    void Renderer::warmScenePipelines(bool motion, bool multisampled, bool first)
    {
        ZoneScoped;

        const uint32_t variant = 1u << ((motion ? 2u : 0u) + (multisampled ? 1u : 0u));
        if ((warmedSceneVariants & variant) != 0)
            return;
        warmedSceneVariants |= variant;

        const std::filesystem::path folder = pathFromUtf8(config.shaderDir);
        const std::string motionDefine = motion ? MOTION_DEFINE : "";
        const std::string maskDefines = motionDefine + (multisampled ? COVERAGE_DEFINES : MASK_DEFINE);
        const std::string rasm = readFile(folder / "passes/rasm.slang");
        const std::string shaded = getShaderVariant(motionDefine, rasm);
        std::vector<ShaderCompiler::Request> requests;
        addGraphics(requests, "rasm", shaded, "vertMain", "fragMain");
        const bool meshShaders = hasMeshShaderPipelines();
        if (meshShaders)
            addMesh(requests, "rasmMesh", shaded, "meshMain", "fragMain");
        addGraphics(requests, "sky", getShaderVariant(motionDefine, readFile(folder / "passes/sky.slang")), "vertMain", "fragMain");
        addGraphics(requests, "rasmMask", getShaderVariant(maskDefines, rasm), "vertMain", "fragMain");
        addGraphics(requests, "rasmBlend", getShaderVariant(motionDefine + "#define RASM_ALPHA_BLEND 1\n", rasm), "vertMain", "fragMain");
        addGraphics(requests, "grid", getShaderVariant(motionDefine, readFile(folder / "passes/grid.slang")), "vertMain", "fragMain");
        const std::string points = readFile(folder / "passes/points.slang");
        addGraphics(requests, "points", getShaderVariant(motionDefine + (multisampled ? "#define RASM_ALPHA_TO_COVERAGE 1\n" : ""), points), "vertMain", "fragMain");
        if (ctx.hasBufferInt64Atomics())
            addGraphics(requests, "pointResolve", getShaderVariant(motionDefine, readFile(folder / "passes/pointRaster.slang")), "resolveVert", "resolveFrag");
        addGraphics(requests, "glyphs", getShaderVariant(motionDefine + "#define RASM_GLYPHS 1\n", rasm), "vertMain", "fragMain");
        // Weighted and additive see-through materials (weightedBlend.cpp), where the device can blend their targets apart;
        // the composite's variant follows the samples.
        if (ctx.hasIndependentBlend())
        {
            addGraphics(requests, "rasmWeighted", getShaderVariant(motionDefine + WEIGHTED_DEFINES, rasm), "vertMain", "fragMain");
            addGraphics(requests, "rasmWeightedGlyphs", getShaderVariant(motionDefine + WEIGHTED_DEFINES + "#define RASM_GLYPHS 1\n", rasm), "vertMain", "fragMain");
            addGraphics(requests, "weightedComposite",
                        getShaderVariant(motionDefine + getCompositeDefines(multisampled ? 2 : 1, ctx.hasSampleRateShading()), readFile(folder / "passes/weightedComposite.slang")), "vertMain",
                        "fragMain");
            addGraphics(requests, "rasmAdditive", getShaderVariant(motionDefine + WEIGHTED_ADDITIVE_DEFINES, rasm), "vertMain", "fragMain");
        }
        if (meshShaders)
            addMesh(requests, "rasmMeshMask", getShaderVariant(maskDefines, rasm), "meshMain", "fragMain");
        if (motion)
            addCompute(requests, "taa", readFile(folder / "passes/taa.slang"), "taaMain");
        if (multisampled)
            addCompute(requests, "msaaResolve", readFile(folder / "passes/msaaResolve.slang"), "resolveMain");
        shaderCompiler->prefetch(requests.data(), static_cast<uint32_t>(requests.size()), first);
        if (debugView != DebugView::NONE)
            warmDebugPipelines(motion);
    }

    // A debug view's shaders, for scene targets with a motion target or without: queued first when a debug view is
    // chosen (setDebugView), for each kind of target warmed so far, and with a new kind while one is on. Until they are
    // ready the views draw shaded; Renderer::waitForLoads waits for them (AssetLoader::waitForLoads does not).
    void Renderer::warmDebugPipelines(bool motion)
    {
        ZoneScoped;

        const uint32_t variant = motion ? 2u : 1u;
        if ((warmedDebugVariants & variant) != 0)
            return;
        warmedDebugVariants |= variant;

        const std::filesystem::path folder = pathFromUtf8(config.shaderDir);
        const std::string defines = std::string(motion ? MOTION_DEFINE : "") + "#define RASM_DEBUG_VIEW 1\n";
        const std::string rasm = readFile(folder / "passes/rasm.slang");
        std::vector<ShaderCompiler::Request> requests;
        addGraphics(requests, "rasmDebug", getShaderVariant(defines, rasm), "vertMain", "fragMain");
        addGraphics(requests, "rasmDebugMask", getShaderVariant(defines + MASK_DEFINE, rasm), "vertMain", "fragMain");
        addGraphics(requests, "rasmDebugBlend", getShaderVariant(defines + "#define RASM_ALPHA_BLEND 1\n", rasm), "vertMain", "fragMain");
        addGraphics(requests, "debugGhosts", getShaderVariant(motion ? MOTION_DEFINE : "", readFile(folder / "passes/debugGhosts.slang")), "vertMain", "fragMain");
        if (hasMeshShaderPipelines())
        {
            addMesh(requests, "rasmDebugMesh", getShaderVariant(defines, rasm), "meshMain", "fragMain");
            addMesh(requests, "rasmDebugMeshMask", getShaderVariant(defines + MASK_DEFINE, rasm), "meshMain", "fragMain");
        }
        shaderCompiler->prefetch(requests.data(), static_cast<uint32_t>(requests.size()), true);
    }
}
