#include "rasm/render/resource.h"

#include "doctest/doctest.h"

namespace
{
    rasm::ShaderDesc makeShaderDesc(std::vector<uint8_t> code)
    {
        return {.name = "shader", .type = rasm::ShaderType::VERTEX, .entryPoint = "vertMain", .code = std::move(code)};
    }

    rasm::PipelineDesc makePipelineDesc()
    {
        return {.name = "pipeline",
                .vertexShader = rasm::ShaderHandle{1, 1},
                .fragmentShader = rasm::ShaderHandle{2, 1},
                .descriptorSetLayout = rasm::DescriptorSetLayoutHandle{3, 1},
                .topology = rasm::PrimitiveTopology::TRIANGLE_LIST,
                .colorAttachmentFormat = rasm::Format::B8G8R8A8_SRGB,
                .depthStencilAttachmentFormat = rasm::Format::D32_SFLOAT_S8_UINT};
    }

    rasm::BindlessDescriptorSetLayoutDesc makeLayoutDesc()
    {
        return {.name = "layout", .stage = rasm::ShaderType::FRAGMENT, .count = 1024, .type = rasm::DescriptorType::COMBINED_IMAGE_SAMPLER};
    }

    rasm::RenderTargetDesc makeRenderTargetDesc()
    {
        return {.name = "target",
                .width = 1280,
                .height = 720,
                .colorFormat = rasm::Format::B8G8R8A8_SRGB,
                .depthFormat = rasm::Format::D32_SFLOAT_S8_UINT,
                .clearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    }
}

TEST_CASE("default-constructed descriptors are equal (B18)")
{
    CHECK(rasm::BufferDesc{} == rasm::BufferDesc{});
    CHECK(rasm::TextureDesc{} == rasm::TextureDesc{});
    CHECK(rasm::ShaderDesc{} == rasm::ShaderDesc{});
    CHECK(rasm::PipelineDesc{} == rasm::PipelineDesc{});
    CHECK(rasm::RenderTargetDesc{} == rasm::RenderTargetDesc{});
    CHECK(rasm::BindlessDescriptorSetLayoutDesc{} == rasm::BindlessDescriptorSetLayoutDesc{});
    CHECK(rasm::DescriptorPoolDesc{} == rasm::DescriptorPoolDesc{});
    CHECK(rasm::PipelineDescHash{}(rasm::PipelineDesc{}) == rasm::PipelineDescHash{}(rasm::PipelineDesc{}));
}

TEST_CASE("equal descriptors hash the same")
{
    CHECK(makePipelineDesc() == makePipelineDesc());
    CHECK(rasm::PipelineDescHash{}(makePipelineDesc()) == rasm::PipelineDescHash{}(makePipelineDesc()));

    CHECK(makeLayoutDesc() == makeLayoutDesc());
    CHECK(rasm::BindlessDescriptorSetLayoutDescHash{}(makeLayoutDesc()) == rasm::BindlessDescriptorSetLayoutDescHash{}(makeLayoutDesc()));

    CHECK(makeShaderDesc({1, 2, 3, 4}) == makeShaderDesc({1, 2, 3, 4}));
    CHECK(rasm::ShaderDescHash{}(makeShaderDesc({1, 2, 3, 4})) == rasm::ShaderDescHash{}(makeShaderDesc({1, 2, 3, 4})));
}

TEST_CASE("shader descriptors compare the code")
{
    CHECK_FALSE(makeShaderDesc({1, 2, 3, 4}) == makeShaderDesc({1, 2, 3, 5}));
    CHECK_FALSE(makeShaderDesc({1, 2, 3, 4}) == makeShaderDesc({1, 2, 3}));

    auto fragment = makeShaderDesc({1, 2, 3, 4});
    fragment.type = rasm::ShaderType::FRAGMENT;
    CHECK_FALSE(makeShaderDesc({1, 2, 3, 4}) == fragment);

    auto otherEntryPoint = makeShaderDesc({1, 2, 3, 4});
    otherEntryPoint.entryPoint = "otherMain";
    CHECK_FALSE(makeShaderDesc({1, 2, 3, 4}) == otherEntryPoint);
}

TEST_CASE("the hash is order-sensitive, unlike an XOR of the fields (D5)")
{
    auto swapped = makePipelineDesc();
    std::swap(swapped.vertexShader, swapped.fragmentShader);
    CHECK_FALSE(makePipelineDesc() == swapped);
    CHECK(rasm::PipelineDescHash{}(makePipelineDesc()) != rasm::PipelineDescHash{}(swapped));

    CHECK(rasm::HandleHash{}(rasm::ShaderHandle{1, 2}) != rasm::HandleHash{}(rasm::ShaderHandle{2, 1}));
    CHECK(rasm::HandleHash{}(rasm::ShaderHandle{1, 1}) != rasm::HandleHash{}(rasm::ShaderHandle{2, 2}));
}

TEST_CASE("every pipeline field takes part in equality")
{
    auto base = makePipelineDesc();

    auto vertexShader = base;
    vertexShader.vertexShader.index = 9;
    CHECK_FALSE(base == vertexShader);

    auto fragmentShader = base;
    fragmentShader.fragmentShader.index = 9;
    CHECK_FALSE(base == fragmentShader);

    auto computeShader = base;
    computeShader.computeShader = rasm::ShaderHandle{9, 1};
    CHECK_FALSE(base == computeShader);
    CHECK(rasm::PipelineDescHash{}(base) != rasm::PipelineDescHash{}(computeShader));

    auto layout = base;
    layout.descriptorSetLayout.index = 9;
    CHECK_FALSE(base == layout);

    auto topology = base;
    topology.topology = rasm::PrimitiveTopology::LINE_LIST;
    CHECK_FALSE(base == topology);

    auto colorFormat = base;
    colorFormat.colorAttachmentFormat = rasm::Format::R8G8B8A8_UNORM;
    CHECK_FALSE(base == colorFormat);

    auto depthFormat = base;
    depthFormat.depthStencilAttachmentFormat = rasm::Format::D24_UNORM_S8_UINT;
    CHECK_FALSE(base == depthFormat);

    auto name = base;
    name.name = "other";
    CHECK_FALSE(base == name);
}

TEST_CASE("every bindless layout field takes part in equality")
{
    auto base = makeLayoutDesc();

    auto stage = base;
    stage.stage = rasm::ShaderType::VERTEX;
    CHECK_FALSE(base == stage);

    auto count = base;
    count.count = 16;
    CHECK_FALSE(base == count);

    auto type = base;
    type.type = rasm::DescriptorType::UNIFORM_BUFFER;
    CHECK_FALSE(base == type);
}

TEST_CASE("every render target field takes part in equality")
{
    auto base = makeRenderTargetDesc();

    auto width = base;
    width.width = 640;
    CHECK_FALSE(base == width);

    auto height = base;
    height.height = 360;
    CHECK_FALSE(base == height);

    auto colorFormat = base;
    colorFormat.colorFormat = rasm::Format::R8G8B8A8_UNORM;
    CHECK_FALSE(base == colorFormat);

    auto depthFormat = base;
    depthFormat.depthFormat = rasm::Format::D24_UNORM_S8_UINT;
    CHECK_FALSE(base == depthFormat);

    auto clearColor = base;
    clearColor.clearColor = glm::vec4(1.0f);
    CHECK_FALSE(base == clearColor);
}
