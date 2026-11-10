// clang-format off
#pragma once

#include "rhi/device.h"

#include <utility>
#include <vector>

namespace rasm::rhi
{
    class TextureStates
    {
    public:
        bool                                            transition(CommandBuffer& commandBuffer, Texture* texture, TextureUsage newUsage, bool discard);
        void                                            submitted();
        void                                            discardUnsubmitted();
        void                                            forget(Texture* texture);

    private:
        std::vector<std::pair<Texture*, TextureUsage>>  unsubmitted;
    };
}
// clang-format on
