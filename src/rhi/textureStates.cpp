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

        unsubmitted.push_back({.texture = texture, .commandBuffer = &commandBuffer, .before = texture->state});
        texture->state = newUsage;
        return true;
    }

    // A frame may go out in several submits, and only the changes recorded into these command buffers are on the
    // GPU now. A texture's changes in a later submit start from the state these leave.
    void TextureStates::submitted(CommandBuffer *const *commandBuffers, uint32_t commandBufferCount)
    {
        std::erase_if(unsubmitted, [commandBuffers, commandBufferCount](const Change &change)
                      { return std::find(commandBuffers, commandBuffers + commandBufferCount, change.commandBuffer) != commandBuffers + commandBufferCount; });
    }

    // Backwards, so a texture ends at the state from before its first change that was not submitted.
    void TextureStates::discardUnsubmitted()
    {
        for (auto it = unsubmitted.rbegin(); it != unsubmitted.rend(); ++it)
            it->texture->state = it->before;
        unsubmitted.clear();
    }

    void TextureStates::forget(Texture *texture)
    {
        std::erase_if(unsubmitted, [texture](const Change &change)
                      { return change.texture == texture; });
    }
}
