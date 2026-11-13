// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/render/renderGraph.h"

namespace rasm
{
    class Engine;

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

    private:
        Engine&                     engine;
        RenderGraph                 graph;
        RenderView                  graphView   = {};
        bool                        built       = false;
    };
}
// clang-format on
