// clang-format off
#pragma once

#include <cstdint>

namespace rasm
{

    namespace gfx { struct SceneData; }

    struct GlyphCullLayout
    {
        uint64_t                        chunks                                  = 0;
        uint64_t                        batches                                 = 0;
        uint64_t                        commands                                = 0;
        uint64_t                        slots                                   = 0;
        uint64_t                        list                                    = 0;
        uint64_t                        size                                    = 0;
        uint32_t                        commandCapacity                         = 0;
    };

    GlyphCullLayout                     getGlyphCullLayout(uint32_t chunkCapacity, uint32_t batchCapacity);
    void                                writeGlyphCullPointers(gfx::SceneData& sceneData, uint64_t address, const GlyphCullLayout& layout, uint32_t chunkCapacity, uint32_t batchCapacity);

}
// clang-format on
