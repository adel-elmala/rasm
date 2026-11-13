#include "rhi/textureStates.h"

#include "doctest/doctest.h"

#include <vector>

using namespace rasm;
using namespace rasm::rhi;

namespace
{
    struct Transition
    {
        TextureUsage oldUsage = TextureUsage::UNKNOWN;
        TextureUsage newUsage = TextureUsage::UNKNOWN;
    };

    class FakeCommandBuffer : public CommandBuffer
    {
    public:
        bool begin() override { return true; }
        bool end() override { return true; }
        bool transitionTexture(Texture *, TextureUsage oldUsage, TextureUsage newUsage) override
        {
            transitions.push_back({oldUsage, newUsage});
            return true;
        }
        void beginRendering(Texture *, Texture *, const glm::vec4 &, const AttachmentOps &) override {}
        void endRendering() override {}
        void setViewport(float, float, float, float, float, float) override {}
        void setScissor(int32_t, int32_t, uint32_t, uint32_t) override {}
        void setCullMode(CullMode) override {}
        void bindPipeline(Pipeline *) override {}
        void bindIndexBuffer(Buffer *, uint64_t, Format) override {}
        void bindDescriptorSet(Pipeline *, DescriptorSet *, uint32_t) override {}
        void pushConstants(Pipeline *, ShaderType, const void *, uint32_t, uint32_t) override {}
        void draw(uint32_t, uint32_t) override {}
        void drawIndexed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void drawIndexedIndirect(Buffer *, uint64_t, uint32_t, uint32_t) override {}
        void drawIndexedIndirectCount(Buffer *, uint64_t, Buffer *, uint64_t, uint32_t, uint32_t) override {}
        void dispatch(uint32_t, uint32_t, uint32_t) override {}
        void memoryBarrier(PipelineStage, PipelineStage) override {}
        void copyBuffers(Buffer *, const BufferCopy *, uint32_t) override {}
        void writeTimestamp(QueryPool *, uint32_t) override {}

        std::vector<Transition> transitions;
    };
}

TEST_CASE("texture states record transitions from the tracked state")
{
    FakeCommandBuffer commandBuffer;
    TextureStates states;
    Texture texture;

    SUBCASE("a transition starts from the last one")
    {
        REQUIRE(states.transition(commandBuffer, &texture, TextureUsage::COLOR_ATTACHMENT, false));
        REQUIRE(states.transition(commandBuffer, &texture, TextureUsage::SAMPLED, false));

        REQUIRE(commandBuffer.transitions.size() == 2);
        CHECK(commandBuffer.transitions[1].oldUsage == TextureUsage::COLOR_ATTACHMENT);
        CHECK(commandBuffer.transitions[1].newUsage == TextureUsage::SAMPLED);
        CHECK(texture.state == TextureUsage::SAMPLED);
    }

    SUBCASE("a read after a read records nothing")
    {
        texture.state = TextureUsage::SAMPLED;

        REQUIRE(states.transition(commandBuffer, &texture, TextureUsage::SAMPLED, false));
        CHECK(commandBuffer.transitions.empty());
    }

    SUBCASE("an attachment used again still gets a barrier")
    {
        texture.state = TextureUsage::COLOR_ATTACHMENT;

        REQUIRE(states.transition(commandBuffer, &texture, TextureUsage::COLOR_ATTACHMENT, false));
        REQUIRE(commandBuffer.transitions.size() == 1);
        CHECK(commandBuffer.transitions[0].oldUsage == TextureUsage::COLOR_ATTACHMENT);
    }

    SUBCASE("a discard moves out of UNKNOWN")
    {
        texture.state = TextureUsage::SAMPLED;

        REQUIRE(states.transition(commandBuffer, &texture, TextureUsage::COLOR_ATTACHMENT, true));
        REQUIRE(commandBuffer.transitions.size() == 1);
        CHECK(commandBuffer.transitions[0].oldUsage == TextureUsage::UNKNOWN);
        CHECK(texture.state == TextureUsage::COLOR_ATTACHMENT);
    }
}

TEST_CASE("texture states of commands that are never submitted are put back")
{
    FakeCommandBuffer commandBuffer;
    TextureStates states;
    Texture texture;
    texture.state = TextureUsage::SAMPLED;

    states.transition(commandBuffer, &texture, TextureUsage::COLOR_ATTACHMENT, true);
    states.transition(commandBuffer, &texture, TextureUsage::SAMPLED, false);
    states.transition(commandBuffer, &texture, TextureUsage::DEPTH_STENCIL_ATTACHMENT, false);

    SUBCASE("a discarded recording leaves the state from before it")
    {
        states.discardUnsubmitted();
        CHECK(texture.state == TextureUsage::SAMPLED);
    }

    SUBCASE("a submitted recording keeps its state")
    {
        states.submitted();
        states.discardUnsubmitted();
        CHECK(texture.state == TextureUsage::DEPTH_STENCIL_ATTACHMENT);
    }

    SUBCASE("a forgotten texture is not touched")
    {
        states.forget(&texture);
        states.discardUnsubmitted();
        CHECK(texture.state == TextureUsage::DEPTH_STENCIL_ATTACHMENT);
    }
}
