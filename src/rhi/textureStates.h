// clang-format off
#pragma once

#include "rhi/device.h"

#include <vector>

namespace rasm::rhi
{
    class TextureStates
    {
    public:
        bool                                            transition(CommandBuffer& commandBuffer, Texture* texture, TextureUsage newUsage, bool discard);
        void                                            submitted(CommandBuffer* const* commandBuffers, uint32_t commandBufferCount);
        void                                            discardUnsubmitted();
        void                                            forget(Texture* texture);

    private:
        struct Change
        {
            Texture*                                    texture             = nullptr;
            const CommandBuffer*                        commandBuffer       = nullptr;
            TextureUsage                                before              = TextureUsage::UNKNOWN;
        };

        std::vector<Change>                             unsubmitted;        // in the order they were recorded
    };
}
// clang-format on
