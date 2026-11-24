// clang-format off
#pragma once

#include "rasm/renderer.h"

namespace rasm
{

    // The all-in-one class: a Renderer whose windows come from EngineConfig::platform. SDL opens them and reads their
    // events itself; NATIVE draws into the application's windows and HEADLESS into offscreen images, as a Renderer does.
    // Everything else is the Renderer's.
    class Engine final : public Renderer
    {
    public:
        explicit                    Engine(const EngineConfig& config);
    };

}
// clang-format on
