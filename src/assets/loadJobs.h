// clang-format off
#pragma once

#include "rasm/mesh.h"
#include "rasm/types.h"
#include "assets/meshImport.h"
#include "core/workerPool.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace rasm
{
    // What a load's jobs write and the main thread reads once ticket.state is DONE. The jobs hold it by shared_ptr, so
    // a load the main thread dropped lives until its last job ends.
    struct MeshLoadJob
    {
        LoadTicket                      ticket;
        GLTFRaw                         raw;
        std::vector<GltfImageWork>      images;
        std::atomic<uint32_t>           remaining       = 0;    // child jobs still running: the last one finishes the load
    };

    struct TextureLoadJob
    {
        LoadTicket                      ticket;
        TextureRaw                      raw;
    };

    struct EnvironmentLoadJob
    {
        LoadTicket                      ticket;
        EnvironmentRaw                  raw;
    };
}
// clang-format on
