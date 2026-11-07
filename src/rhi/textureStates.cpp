#include "rhi/textureStates.h"

#include <algorithm>

namespace rasm::rhi
{
    // texture->state is what the recorded commands leave the texture in. Commands that are never submitted
    // don't change the GPU, so discardUnsubmitted puts back the states from before them.
    bool TextureStates::transition(CommandBuffer &commandBuffer, Texture *texture, TextureUsage newUsage, bool discard)
    {
        // Reads after reads need no barrier. An attachment used again still gets one, since it is written.
        if (!discard && texture->state == newUsage && newUsage == TextureUsage::SAMPLED)
            return true;
        // Moving out of UNKNOWN drops the contents, and still waits for every earlier use.
        if (!commandBuffer.transitionTexture(texture, discard ? TextureUsage::UNKNOWN : texture->state, newUsage))
            return false;

        const bool known = std::any_of(unsubmitted.begin(), unsubmitted.end(), [texture](const std::pair<Texture *, TextureUsage> &entry)
                                       { return entry.first == texture; });
        if (!known)
            unsubmitted.push_back({texture, texture->state});
        texture->state = newUsage;
        return true;
    }

    void TextureStates::submitted()
    {
        unsubmitted.clear();
    }

    void TextureStates::discardUnsubmitted()
    {
        for (const auto &[texture, state] : unsubmitted)
            texture->state = state;
        unsubmitted.clear();
    }

    void TextureStates::forget(Texture *texture)
    {
        std::erase_if(unsubmitted, [texture](const std::pair<Texture *, TextureUsage> &entry)
                      { return entry.first == texture; });
    }
}
