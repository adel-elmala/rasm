// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/view.h"
#include "rasm/render/renderGraph.h"

#include "glm/glm.hpp"

#include <memory>
#include <vector>

namespace rasm
{
    class Renderer;
    struct TaaState;

    // The frame's background, linear. The scene's HDR target clears to it, and the tone-map pass maps it like the
    // scene's light, so exposure changes it. A frame with no pass on the swapchain shows it unmapped.
    constexpr glm::vec4             BACKGROUND_COLOR    = glm::vec4(0.018f, 0.018f, 0.018f, 1.0f);

    // What a view keeps from one graph to the next. Its camera and output target say which view it is, not its place in
    // the list, so adding, removing or moving another view leaves it as it is.
    struct ViewState
    {
        CameraHandle                camera;
        RenderTargetHandle          output;                         // ViewOutput::target; invalid: the swapchain of window
        WindowHandle                window;                         // ViewOutput::window
        SceneHandle                 scene;
        AntiAliasing                antiAliasing        = AntiAliasing::NONE;   // the one its targets were made for
        RenderTargetHandle          sceneTarget;                    // HDR color (and motion with TAA) and depth, the scene view's target
        RenderTargetHandle          taaHistory;
        std::shared_ptr<TaaState>   taa;
        uint32_t                    number              = 0;        // names its targets: scene_hdr for 0, scene_hdr_1, ... for the others alive
        bool                        used                = false;    // in the list the graph was built from
    };

    class ViewGraph
    {
    public:
        explicit                    ViewGraph(Renderer& owner);
        void                        render(const View* views, uint32_t viewCount);
        // Lets go of its graph and of the views of this scene or camera, so they can be destroyed.
        void                        release(SceneHandle scene, CameraHandle camera);
        // Its graph holds the scene view of this scene and camera into this target.
        bool                        draws(SceneHandle scene, CameraHandle camera, const RenderTargetHandle& renderTarget) const;
        // The scene target the graph draws the view into (its HDR target), or invalid when it does not draw the view.
        RenderTargetHandle          getSceneTarget(const View& view) const;

    private:
        void                        build(const View* views, uint32_t viewCount);
        bool                        addView(RenderGraph& graph, const View& view, glm::uvec2 size);
        void                        destroyState(ViewState& state);

        Renderer&                           renderer;
        RenderGraph                         graph;
        std::vector<View>                   graphViews;             // the list the graph was built from; keeps its capacity
        std::vector<glm::uvec2>             graphSizes;             // each view's size in pixels then
        std::vector<glm::uvec2>             sizes;                  // this frame's, compared with graphSizes
        std::vector<ViewState>              states;
        std::vector<RenderTargetHandle>     textures;               // the view textures added so far, which later views sample
        bool                                built       = false;
    };
}
// clang-format on
