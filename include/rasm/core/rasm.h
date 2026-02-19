#pragma once

#include <string>

#include "rasm/core/material.h"
#include "rasm/core/scene.h"

namespace rasm
{

    struct EngineConfig
    {
        std::string appName;
        int windowWidth;
        int windowHeight;
        bool enableValidation;
    };

    typedef uint32_t MeshHandle;
    // typedef Material MaterialHandle;
    typedef Scene SceneHandle;

    std::string rasm_hello();

}