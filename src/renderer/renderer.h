// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/render/renderGraph.h"

#include "glm/glm.hpp"

namespace rasm
{
    class Engine;

    // The frame's background, linear. The scene's HDR target clears to it, and the tone-map pass maps it like the
    // scene's light, so exposure changes it. A frame with no pass on the swapchain shows it unmapped.
    constexpr glm::vec4             BACKGROUND_COLOR    = glm::vec4(0.018f, 0.018f, 0.018f, 1.0f);

    struct RenderView
    {
        SceneHandle                 scene;
        CameraHandle                camera;
    };

    class Renderer
    {
    public:
        explicit                    Renderer(Engine& owner);
        void                        render(const RenderView& view);
        // Lets go of its graph when it draws this scene or camera, so they can be destroyed.
        void                        release(SceneHandle scene, CameraHandle camera);
        bool                        draws(SceneHandle scene, CameraHandle camera) const;     // its graph holds the views of this scene and camera

    private:
        Engine&                     engine;
        RenderGraph                 graph;
        RenderView                  graphView   = {};
        bool                        built       = false;
    };
}
// clang-format on
