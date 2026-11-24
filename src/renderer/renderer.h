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
    class Engine;
    struct TaaState;

    // The frame's background, linear. The scene's HDR target clears to it, and the tone-map pass maps it like the
    // scene's light, so exposure changes it. A frame with no pass on the swapchain shows it unmapped.
    constexpr glm::vec4             BACKGROUND_COLOR    = glm::vec4(0.018f, 0.018f, 0.018f, 1.0f);

    // What a view keeps from one graph to the next. Its camera and output target say which view it is, not its place in
    // the list, so adding, removing or moving another view leaves it as it is.
    struct ViewState
    {
        CameraHandle                camera;
        RenderTargetHandle          output;                         // ViewOutput::target; invalid: the swapchain
        SceneHandle                 scene;
        AntiAliasing                antiAliasing        = AntiAliasing::NONE;   // the one its targets were made for
        RenderTargetHandle          sceneTarget;                    // HDR color (and motion with TAA) and depth, the scene view's target
        RenderTargetHandle          taaHistory;
        std::shared_ptr<TaaState>   taa;
        bool                        used                = false;    // in the list the graph was built from
    };

    class Renderer
    {
    public:
        explicit                    Renderer(Engine& owner);
        void                        render(const View* views, uint32_t viewCount);
        // Lets go of its graph and of the views of this scene or camera, so they can be destroyed.
        void                        release(SceneHandle scene, CameraHandle camera);
        bool                        draws(SceneHandle scene, CameraHandle camera) const;     // its graph holds a view of this scene and camera

    private:
        void                        build(const View* views, uint32_t viewCount);
        bool                        addView(RenderGraph& graph, const View& view, glm::uvec2 size);
        void                        destroyState(ViewState& state);

        Engine&                     engine;
        RenderGraph                 graph;
        std::vector<View>           graphViews;                     // the list the graph was built from; keeps its capacity
        std::vector<glm::uvec2>     graphSizes;                     // each view's size in pixels then
        std::vector<glm::uvec2>     sizes;                          // this frame's, compared with graphSizes
        std::vector<ViewState>      states;
        std::vector<RenderTargetHandle> textures;                   // the view textures added so far, which later views sample
        bool                        built               = false;
    };
}
// clang-format on
