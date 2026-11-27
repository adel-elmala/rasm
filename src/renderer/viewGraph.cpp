#include "renderer/viewGraph.h"
#include "rasm/renderer.h"
#include "renderer/export.h"
#include "renderer/gpuScene.h"
#include "renderer/taa.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <bit>
#include <string>

namespace rasm
{
    namespace
    {
        // The samples per pixel of an anti-aliasing mode, or the most the device has below them, with a warning.
        uint32_t getSceneSamples(RenderContext &ctx, AntiAliasing antiAliasing)
        {
            uint32_t samples = 1;
            if (antiAliasing == AntiAliasing::MSAA_2X)
                samples = 2;
            if (antiAliasing == AntiAliasing::MSAA_4X)
                samples = 4;
            if (antiAliasing == AntiAliasing::MSAA_8X)
                samples = 8;
            uint32_t supported = samples;
            while (supported > 1 && !ctx.supportsSampleCount(supported))
                supported /= 2;
            if (supported != samples)
                spdlog::warn("The device can't draw {} samples per pixel; the scene uses {}.", samples, supported);
            return supported;
        }

        // A view's setting, or the renderer's where the view keeps the default.
        AntiAliasing getAntiAliasing(const View &view, const RendererConfig &config)
        {
            const AntiAliasing mode = view.antiAliasing == AntiAliasing::DEFAULT ? config.antiAliasing : view.antiAliasing;
            return mode == AntiAliasing::DEFAULT ? AntiAliasing::NONE : mode;
        }

        bool getBloom(const View &view, const RendererConfig &config)
        {
            return view.bloom == ViewToggle::DEFAULT ? config.bloom : view.bloom == ViewToggle::ON;
        }

        // A bit per scene view a graph holds.
        uint32_t getHeldSceneViews(const GpuScene &gpuScene)
        {
            uint32_t held = 0;
            for (uint32_t i = 0; i < MAX_SCENE_VIEWS; ++i)
                held |= gpuScene.views[i].graphs > 0 ? 1u << i : 0u;
            return held;
        }

        // An invalid handle and the main window's are one window.
        bool isSameWindow(const WindowHandle &a, const WindowHandle &b)
        {
            const bool mainA = !a.isValid() || a.index == MAIN_WINDOW_INDEX;
            const bool mainB = !b.isValid() || b.index == MAIN_WINDOW_INDEX;
            return mainA ? mainB : a == b;
        }
    }

    ViewGraph::ViewGraph(Renderer &owner) : renderer(owner), graph(&owner)
    {
        // Room for every view the scene views allow, so a steady frame does not grow them (P27).
        graphViews.reserve(MAX_SCENE_VIEWS);
        graphSizes.reserve(MAX_SCENE_VIEWS);
        sizes.reserve(MAX_SCENE_VIEWS);
        states.reserve(MAX_SCENE_VIEWS);
        textures.reserve(MAX_SCENE_VIEWS);
    }

    // Each view is its scene passes into an HDR target of its output's size (multisampled and resolved, or with motion
    // and TAA, by its anti-aliasing), the bloom passes when it has bloom, and the tone-map pass into its output; the UI
    // pass comes last. The graph is built again only when the list or a view's size in pixels changes: comparing them
    // costs no allocation. A failed view is left out with an error, and the others draw. Adding the passes fails on bad
    // handles or a failed allocation, so a list that failed is not tried again until it changes. Shader errors retry
    // inside the passes.
    void ViewGraph::render(const View *views, uint32_t viewCount)
    {
        ZoneScoped;

        if (viewCount > 0 && views == nullptr)
        {
            spdlog::error("render: viewCount is {} and views is null.", viewCount);
            viewCount = 0;
        }
        // The frame's events first, then the swapchain follows the window, so the views take its new size in the same
        // frame. beginFrame polls no more after this.
        renderer.pollEvents();
        renderer.updateSwapchain();
        auto &ctx = renderer.getRenderContext();
        sizes.clear();
        for (uint32_t i = 0; i < viewCount; ++i)
        {
            // A destroyed output target or a removed window has no pixels, so the graph that draws into it is built again
            // without it.
            const RenderTargetHandle target = views[i].output.target;
            const WindowHandle window = views[i].output.window;
            const bool alive = target.isValid() ? ctx.isAlive(target) : (!window.isValid() || renderer.isLive(window));
            const glm::uvec2 image = !alive ? glm::uvec2(0) : (target.isValid() ? glm::uvec2(ctx.getRenderTarget(target).width, ctx.getRenderTarget(target).height) : ctx.getSwapchainExtent(window));
            const ViewPixels pixels = getViewPixels(views[i].output.rect, image.x, image.y);
            sizes.emplace_back(pixels.width, pixels.height);
        }

        // A running export's view joins the graph, and leaves it when the export ends (src/renderer/export.cpp).
        renderer.updateExports();
        const bool same = built && viewCount == graphViews.size() && std::equal(views, views + viewCount, graphViews.begin()) && sizes == graphSizes &&
                          exportKey == renderer.exports->graphKey;
        if (!same)
            build(views, viewCount);
        renderer.collectExports();
        // The built-in controller moves the camera of the first view in the main window: view textures come first in a list.
        CameraHandle controlled = viewCount > 0 ? views[0].camera : CameraHandle{};
        for (uint32_t i = viewCount; i-- > 0;)
        {
            const WindowHandle window = views[i].output.window;
            if (!views[i].output.target.isValid() && (!window.isValid() || window.index == MAIN_WINDOW_INDEX))
                controlled = views[i].camera;
        }
        graph.execute(controlled);
    }

    void ViewGraph::build(const View *views, uint32_t viewCount)
    {
        ZoneScoped;

        for (ViewState &state : states)
            state.used = false;
        for (ShadowSet &set : shadowSets)
            set.cameras.clear();
        textures.clear();
        static_assert(MAX_SCENE_VIEWS <= 32);
        const uint32_t heldBefore = getHeldSceneViews(*renderer.gpuScene);
        RenderGraph next = renderer.createRenderGraph();
        for (uint32_t i = 0; i < viewCount; ++i)
        {
            const View &view = views[i];
            bool repeated = false;
            for (uint32_t j = 0; j < i; ++j)
                repeated = repeated || (views[j].camera == view.camera && views[j].output.target == view.output.target && isSameWindow(views[j].output.window, view.output.window));
            if (repeated)
            {
                spdlog::error("render: view {} has the camera and output of an earlier view; it is left out.", i);
                continue;
            }
            // A window closed or a texture destroyed between frames is a normal change: the view draws nothing.
            if (sizes[i].x == 0 || sizes[i].y == 0)
            {
                spdlog::info("render: view {} (camera {}) has no pixels (a removed window, a destroyed target, or an empty rect); it is left out.", i, view.camera.index);
                continue;
            }
            if (!addView(next, view, sizes[i]))
                spdlog::error("render: view {} (camera {}) could not be added; it is left out this frame and until the list changes.", i, view.camera.index);
        }
        // An export whose view failed is failed: the next frame builds the graph again without it.
        const bool exportAdded = renderer.addExportPasses(next, textures.data(), static_cast<uint32_t>(textures.size()));
        exportKey = exportAdded ? renderer.exports->graphKey : renderer.exports->graphKey + 1;
        renderer.addUiPass(next, textures.data(), static_cast<uint32_t>(textures.size()));

        // The new graph holds the kept views already, so the old one lets go of them without freeing them.
        graph = std::move(next);
        for (ViewState &state : states)
        {
            if (!state.used)
                destroyState(state);
        }
        std::erase_if(states, [](const ViewState &state)
                      { return !state.used; });
        // A set no view draws any more goes; the release queue frees its maps once the frames in flight are done.
        for (const ShadowSet &set : shadowSets)
        {
            for (uint32_t i = 0; set.cameras.empty() && i < set.mapCount; ++i)
                renderer.destroyRenderTarget(set.maps[i]);
        }
        std::erase_if(shadowSets, [](const ShadowSet &set)
                      { return set.cameras.empty(); });
        // The views kept from the graph before, and their shadows, are the same scene views: their culling history,
        // Hi-Z and label fades go on.
        const uint32_t held = getHeldSceneViews(*renderer.gpuScene);
        spdlog::info("render: the views' graph holds {} scene views, {} of them new.", std::popcount(held), std::popcount(held & ~heldBefore));
        graphViews.assign(views, views + viewCount);
        graphSizes = sizes;
        built = true;
    }

    // A view's kept state, made for its scene and anti-aliasing, at its size; then its passes.
    bool ViewGraph::addView(RenderGraph &next, const View &view, glm::uvec2 size)
    {
        const RendererConfig &config = renderer.config;
        if (!renderer.isLive(view.scene) || !renderer.isLive(view.camera))
        {
            spdlog::error("render: a view needs a valid scene and camera.");
            return false;
        }
        // A view texture is one color, no depth, one sample, not storage: what the tone map draws into.
        auto &ctx = renderer.getRenderContext();
        if (view.output.target.isValid())
        {
            const RenderTarget &target = ctx.getRenderTarget(view.output.target);
            if (target.colorCount != 1 || target.depthFormat != Format::UNKNOWN || target.samples != 1 || ctx.getRenderTargetDesc(view.output.target).storage)
            {
                spdlog::error("render: a view's output target needs one color, no depth and one sample, and must not be a storage target.");
                return false;
            }
        }
        if (view.shadowCount > MAX_VIEW_SHADOWS)
        {
            spdlog::error("render: a view has at most {} shadows (MAX_VIEW_SHADOWS), not {}.", MAX_VIEW_SHADOWS, view.shadowCount);
            return false;
        }
        for (uint32_t i = 0; i < view.shadowCount; ++i)
        {
            for (uint32_t j = 0; j < i; ++j)
            {
                if (view.shadows[j].light == view.shadows[i].light)
                {
                    spdlog::error("render: a view has two shadows of one light.");
                    return false;
                }
            }
        }
        const AntiAliasing antiAliasing = getAntiAliasing(view, config);
        auto it = std::find_if(states.begin(), states.end(), [&view](const ViewState &state)
                               { return state.camera == view.camera && state.output == view.output.target && isSameWindow(state.window, view.output.window); });
        if (it != states.end() && (it->scene != view.scene || it->antiAliasing != antiAliasing))
        {
            destroyState(*it);
            *it = {};
        }
        if (it == states.end())
        {
            states.push_back({});
            it = states.end() - 1;
        }
        ViewState &state = *it;
        state.camera = view.camera;
        state.output = view.output.target;
        state.window = view.output.window;
        state.scene = view.scene;
        state.antiAliasing = antiAliasing;
        state.used = true;

        // TAA needs each pixel's motion, as the target's second color.
        const bool taa = antiAliasing == AntiAliasing::TAA;
        if (!state.sceneTarget.isValid())
        {
            // The lowest number no other view's targets have, so a single view's target is scene_hdr.
            state.number = 0;
            while (std::any_of(states.begin(), states.end(), [&state](const ViewState &other)
                               { return &other != &state && other.sceneTarget.isValid() && other.number == state.number; }))
                ++state.number;
            const std::string name = state.number == 0 ? std::string("scene_hdr") : "scene_hdr_" + std::to_string(state.number);
            state.sceneTarget = renderer.createRenderTarget({
                .name = name,
                .width = size.x,
                .height = size.y,
                .colorFormats = {SCENE_COLOR_FORMAT, taa ? MOTION_FORMAT : Format::UNKNOWN},
                .depthFormat = Format::D32_SFLOAT,
                .clearColor = BACKGROUND_COLOR,
                .samples = getSceneSamples(ctx, antiAliasing),
            });
            if (taa && state.sceneTarget.isValid())
            {
                state.taaHistory = renderer.createRenderTarget(getTaaHistoryDesc(ctx.getRenderTargetDesc(state.sceneTarget)));
                state.taa = std::make_shared<TaaState>();
            }
        }
        if (!state.sceneTarget.isValid() || (taa && !state.taaHistory.isValid()))
            return false;
        if (!renderer.resizeRenderTarget(state.sceneTarget, size.x, size.y) || (taa && !renderer.resizeRenderTarget(state.taaHistory, size.x, size.y)))
            return false;

        // Its shadows first: each draws its set's maps from this view's camera, after the views before have read them.
        SceneShadow shadows[MAX_VIEW_SHADOWS] = {};
        for (uint32_t i = 0; i < view.shadowCount; ++i)
        {
            const RenderTargetHandle *maps = getShadowMaps(view.shadows[i], view.camera);
            if (maps != nullptr)
                shadows[i] = renderer.addShadowPasses(next, view.scene, view.camera, view.shadows[i], maps);
            if (shadows[i].mapCount == 0)
                return false;
        }
        if (!renderer.addScenePasses(next, view.scene, view.camera, state.sceneTarget, shadows, view.shadowCount, textures.data(), static_cast<uint32_t>(textures.size())))
            return false;
        RenderTargetHandle resolved = state.sceneTarget;
        if (ctx.getRenderTarget(state.sceneTarget).samples > 1)
            resolved = renderer.addMsaaResolvePass(next, state.sceneTarget, view.camera);
        if (taa)
            resolved = renderer.addTaaPass(next, state.sceneTarget, view.camera, state.taaHistory, state.taa);
        if (!resolved.isValid())
            return false;
        const RenderTargetHandle bloom = getBloom(view, config) ? renderer.addBloomPasses(next, resolved, view.camera) : RenderTargetHandle{};
        if (!renderer.addToneMapPass(next, resolved, view.camera, bloom, view.output))
            return false;
        if (view.output.target.isValid())
            textures.push_back(view.output.target);
        return true;
    }

    // The maps of a set of the light, size and count that no other view of this camera draws, made when there is none.
    // Null on a bad light or a failed allocation.
    const RenderTargetHandle *ViewGraph::getShadowMaps(const ShadowDesc &desc, CameraHandle camera)
    {
        if (!renderer.isLive(desc.light))
        {
            spdlog::error("render: a view's shadow needs a valid light.");
            return nullptr;
        }
        const LightType type = renderer.registry.lights[desc.light.index].type;
        const uint32_t mapCount = type == LightType::DIRECTIONAL ? desc.cascadeCount : 1;
        const bool cube = type == LightType::POINT;
        if (mapCount == 0 || mapCount > MAX_SHADOW_CASCADES || desc.size == 0)
        {
            spdlog::error("render: a view's shadow needs 1 to {} cascades and a size above 0.", MAX_SHADOW_CASCADES);
            return nullptr;
        }
        for (ShadowSet &set : shadowSets)
        {
            if (set.light == desc.light && set.size == desc.size && set.mapCount == mapCount && set.cube == cube && std::find(set.cameras.begin(), set.cameras.end(), camera) == set.cameras.end())
            {
                set.cameras.push_back(camera);
                return set.maps;
            }
        }

        // The lowest number no other set has, so the first set's maps have the names addShadowPasses gives.
        ShadowSet set = {.light = desc.light, .size = desc.size, .mapCount = mapCount, .cube = cube};
        while (std::any_of(shadowSets.begin(), shadowSets.end(), [&set](const ShadowSet &other)
                           { return other.number == set.number; }))
            ++set.number;
        for (uint32_t i = 0; i < mapCount; ++i)
        {
            std::string name = "shadow_cascade_" + std::to_string(i);
            if (type != LightType::DIRECTIONAL)
                name = (cube ? "shadow_cube_" : "shadow_spot_") + std::to_string(desc.light.index);
            if (set.number > 0)
                name += "_" + std::to_string(set.number);
            set.maps[i] = renderer.createRenderTarget({.name = name, .width = desc.size, .height = desc.size, .depthFormat = Renderer::DEPTH_FORMAT, .cube = cube});
            if (!set.maps[i].isValid())
            {
                spdlog::error("render: the maps of a view's shadow could not be made.");
                for (uint32_t j = 0; j < i; ++j)
                    renderer.destroyRenderTarget(set.maps[j]);
                return nullptr;
            }
        }
        set.cameras.push_back(camera);
        shadowSets.push_back(std::move(set));
        return shadowSets.back().maps;
    }

    void ViewGraph::destroyState(ViewState &state)
    {
        if (state.sceneTarget.isValid())
            renderer.destroyRenderTarget(state.sceneTarget);
        if (state.taaHistory.isValid())
            renderer.destroyRenderTarget(state.taaHistory);
        state.sceneTarget = {};
        state.taaHistory = {};
        state.taa.reset();
    }

    bool ViewGraph::draws(SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget) const
    {
        if (built && isExportView(*renderer.exports, scene, camera, renderTarget))
            return true;
        // A view's shadow maps are drawn by scene views of its scene and camera too.
        const bool shadowMap = std::any_of(shadowSets.begin(), shadowSets.end(), [&](const ShadowSet &set)
                                           { return std::find(set.maps, set.maps + set.mapCount, renderTarget) != set.maps + set.mapCount &&
                                                    std::find(set.cameras.begin(), set.cameras.end(), camera) != set.cameras.end(); });
        return built && std::any_of(states.begin(), states.end(), [&](const ViewState &state)
                                    { return state.used && state.scene == scene && state.camera == camera && (state.sceneTarget == renderTarget || shadowMap); });
    }

    RenderTargetHandle ViewGraph::getSceneTarget(const View &view) const
    {
        for (const ViewState &state : states)
        {
            if (built && state.used && state.scene == view.scene && state.camera == view.camera && state.output == view.output.target && isSameWindow(state.window, view.output.window))
                return state.sceneTarget;
        }
        return {};
    }

    // The next render builds a new graph from its list.
    void ViewGraph::release(SceneHandle scene, CameraHandle camera)
    {
        // An export of the scene fails: its view goes with the graph.
        const bool exported = scene.isValid() && failSceneExports(*renderer.exports, scene);
        if (exported)
            ++renderer.exports->graphKey;
        const bool holds = exported || std::any_of(states.begin(), states.end(), [scene, camera](const ViewState &state)
                                                   { return state.scene == scene || state.camera == camera; });
        if (!holds)
            return;
        graph = renderer.createRenderGraph();
        for (ViewState &state : states)
        {
            state.used = !(state.scene == scene || state.camera == camera);
            if (!state.used)
                destroyState(state);
        }
        std::erase_if(states, [](const ViewState &state)
                      { return !state.used; });
        built = false;
        graphViews.clear();
    }
}
