// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/readback.h"
#include "rasm/types.h"

#include <cstdint>
#include <vector>

namespace rasm
{

    // A copy asked for by Engine::readFrame or readTexture. endFrame records it into the frame's last graphics batch,
    // and it is done once the frame fence reaches fenceValue.
    struct PendingReadback
    {
        ReadbackHandle              handle;
        TextureHandle               texture;                                // invalid for readFrame: the frame's swapchain image of window
        WindowHandle                window;                                 // readFrame's; invalid: the main window
        uint32_t                    layer           = 0;
        uint32_t                    mip             = 0;
        ReadbackFormat              format          = ReadbackFormat::RGBA8_SRGB;
        Format                      sourceFormat    = Format::UNKNOWN;
        uint32_t                    width           = 0;
        uint32_t                    height          = 0;
        BufferHandle                buffer;
        uint64_t                    fenceValue      = 0;
        bool                        recorded        = false;
        bool                        submitted       = false;
        bool                        failed          = false;
    };

    struct Readbacks
    {
        std::vector<PendingReadback> pending;
    };

    // Bytes per texel of a format readbacks can convert, or 0 for one they can't (block-compressed, 3-channel, ...).
    uint32_t                        getReadbackTexelSize(Format format);
    bool                            isDepthFormat(Format format);
    // Converts width x height tightly packed texels of source into image. False for a format getReadbackTexelSize
    // refuses, or a depth source asked for as RGBA8_SRGB.
    bool                            convertReadback(Format source, const uint8_t* texels, uint32_t width, uint32_t height, ReadbackFormat format, ReadbackImage& image);

}
// clang-format on
