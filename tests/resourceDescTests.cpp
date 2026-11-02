#include "rasm/render/resource.h"

#include "doctest/doctest.h"

namespace
{
    rasm::ResourceDesc makeShaderDesc(const char *source, size_t size)
    {
        rasm::ResourceDesc desc;
        desc.name = "shader";
        desc.type = rasm::ResourceType::SHADER;
        desc.shader.shaderType = rasm::ShaderType::VERTEX_FRAGMENT;
        desc.shader.sourceSize = size;
        desc.shader.source = source;
        return desc;
    }

    rasm::ResourceDesc makePipelineDesc()
    {
        rasm::ResourceDesc desc;
        desc.name = "pipeline";
        desc.type = rasm::ResourceType::GRAPHICS_PIPELINE;
        desc.pipeline.vertexShader = rasm::ShaderHandle{1, 1};
        desc.pipeline.fragmentShader = rasm::ShaderHandle{2, 1};
        desc.pipeline.descriptorSetLayout = rasm::DescriptorSetLayoutHandle{3, 1};
        desc.pipeline.topology = rasm::PrimitiveTopology::TRIANGLE_LIST;
        desc.pipeline.colorAttachmentFormat = rasm::Format::B8G8R8A8_SRGB;
        desc.pipeline.depthStencilAttachmentFormat = rasm::Format::D32_SFLOAT_S8_UINT;
        return desc;
    }

    rasm::ResourceDesc makeLayoutDesc()
    {
        rasm::ResourceDesc desc;
        desc.type = rasm::ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT;
        desc.bindlessDescriptorSetLayout.stage = rasm::ShaderType::FRAGMENT;
        desc.bindlessDescriptorSetLayout.count = 1024;
        desc.bindlessDescriptorSetLayout.type = rasm::ResourceType::TEXTURE;
        return desc;
    }

    rasm::ResourceDesc makeRenderTargetDesc()
    {
        rasm::ResourceDesc desc;
        desc.type = rasm::ResourceType::RENDER_TARGET;
        desc.renderTarget.width = 1280;
        desc.renderTarget.height = 720;
        desc.renderTarget.colorFormat = rasm::Format::B8G8R8A8_SRGB;
        desc.renderTarget.depthFormat = rasm::Format::D32_SFLOAT_S8_UINT;
        desc.renderTarget.clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        return desc;
    }

    void checkEqualAndSameHash(const rasm::ResourceDesc &a, const rasm::ResourceDesc &b)
    {
        CHECK(a == b);
        CHECK(rasm::ResourceDescHash{}(a) == rasm::ResourceDescHash{}(b));
    }
}

TEST_CASE("default-constructed descriptors compare and hash deterministically (B18)")
{
    rasm::ResourceDesc first;
    rasm::ResourceDesc second;
    first.type = rasm::ResourceType::RENDER_TARGET;
    second.type = rasm::ResourceType::RENDER_TARGET;

    checkEqualAndSameHash(first, second);
}

TEST_CASE("descriptors built from the same fields are equal and hash the same")
{
    checkEqualAndSameHash(makePipelineDesc(), makePipelineDesc());
    checkEqualAndSameHash(makeLayoutDesc(), makeLayoutDesc());
    checkEqualAndSameHash(makeRenderTargetDesc(), makeRenderTargetDesc());
}

TEST_CASE("a copied descriptor equals the original")
{
    auto original = makePipelineDesc();
    auto copy = original;

    checkEqualAndSameHash(original, copy);
}

TEST_CASE("descriptors of different types are not equal")
{
    rasm::ResourceDesc first;
    rasm::ResourceDesc second;
    first.type = rasm::ResourceType::TEXTURE;
    second.type = rasm::ResourceType::BUFFER;

    CHECK_FALSE(first == second);
}

TEST_CASE("shader descriptors compare the code, not the pointer")
{
    const char firstSource[] = "spirv";
    const char secondSource[] = "spirv";
    const char otherSource[] = "SPIRV";
    REQUIRE(firstSource != secondSource);

    checkEqualAndSameHash(makeShaderDesc(firstSource, sizeof(firstSource)), makeShaderDesc(secondSource, sizeof(secondSource)));
    CHECK_FALSE(makeShaderDesc(firstSource, sizeof(firstSource)) == makeShaderDesc(otherSource, sizeof(otherSource)));
    CHECK_FALSE(makeShaderDesc(firstSource, sizeof(firstSource)) == makeShaderDesc(firstSource, sizeof(firstSource) - 1));
}

TEST_CASE("every pipeline field takes part in equality")
{
    auto base = makePipelineDesc();

    auto vertexShader = base;
    vertexShader.pipeline.vertexShader.index = 9;
    CHECK_FALSE(base == vertexShader);

    auto fragmentShader = base;
    fragmentShader.pipeline.fragmentShader.index = 9;
    CHECK_FALSE(base == fragmentShader);

    auto layout = base;
    layout.pipeline.descriptorSetLayout.index = 9;
    CHECK_FALSE(base == layout);

    auto topology = base;
    topology.pipeline.topology = rasm::PrimitiveTopology::LINE_LIST;
    CHECK_FALSE(base == topology);

    auto colorFormat = base;
    colorFormat.pipeline.colorAttachmentFormat = rasm::Format::R8G8B8A8_UNORM;
    CHECK_FALSE(base == colorFormat);

    auto depthFormat = base;
    depthFormat.pipeline.depthStencilAttachmentFormat = rasm::Format::D24_UNORM_S8_UINT;
    CHECK_FALSE(base == depthFormat);

    auto name = base;
    name.name = "other";
    CHECK_FALSE(base == name);
}

TEST_CASE("every bindless layout field takes part in equality")
{
    auto base = makeLayoutDesc();

    auto stage = base;
    stage.bindlessDescriptorSetLayout.stage = rasm::ShaderType::VERTEX;
    CHECK_FALSE(base == stage);

    auto count = base;
    count.bindlessDescriptorSetLayout.count = 16;
    CHECK_FALSE(base == count);

    auto type = base;
    type.bindlessDescriptorSetLayout.type = rasm::ResourceType::BUFFER;
    CHECK_FALSE(base == type);
}

TEST_CASE("every render target field takes part in equality")
{
    auto base = makeRenderTargetDesc();

    auto width = base;
    width.renderTarget.width = 640;
    CHECK_FALSE(base == width);

    auto height = base;
    height.renderTarget.height = 360;
    CHECK_FALSE(base == height);

    auto colorFormat = base;
    colorFormat.renderTarget.colorFormat = rasm::Format::R8G8B8A8_UNORM;
    CHECK_FALSE(base == colorFormat);

    auto depthFormat = base;
    depthFormat.renderTarget.depthFormat = rasm::Format::D24_UNORM_S8_UINT;
    CHECK_FALSE(base == depthFormat);

    auto clearColor = base;
    clearColor.renderTarget.clearColor = glm::vec4(1.0f);
    CHECK_FALSE(base == clearColor);
}
