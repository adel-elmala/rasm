#include "c/cApi.h"

#include "rasm/render/renderContext.h"
#include "rasm/render/resource.h"
#include "rasm/renderer.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

using namespace rasm;

namespace
{
    static_assert(RASM_MAX_COLOR_ATTACHMENTS == MAX_COLOR_ATTACHMENTS);
    static_assert(RASM_PUSH_CONSTANTS_SIZE == PUSH_CONSTANTS_SIZE);
    static_assert(RASM_SCENE_COLOR_FORMAT == static_cast<uint32_t>(SCENE_COLOR_FORMAT));
    static_assert(RASM_MOTION_FORMAT == static_cast<uint32_t>(MOTION_FORMAT));
    static_assert(RASM_INVALID_INDEX == UINT32_MAX);
    static_assert(sizeof(rasm_render_target_desc::more_color_formats) / sizeof(rasm_format) == MAX_COLOR_ATTACHMENTS - 1);
    static_assert(RASM_BUFFER_USAGE_INDEX == static_cast<uint32_t>(BufferUsage::INDEX));
    static_assert(RASM_BUFFER_USAGE_INDIRECT == static_cast<uint32_t>(BufferUsage::INDIRECT));
    static_assert(RASM_BUFFER_USAGE_UNIFORM == static_cast<uint32_t>(BufferUsage::UNIFORM));
    static_assert(RASM_BUFFER_USAGE_STORAGE == static_cast<uint32_t>(BufferUsage::STORAGE));
    static_assert(RASM_BUFFER_USAGE_DEVICE_ADDRESS == static_cast<uint32_t>(BufferUsage::DEVICE_ADDRESS));
    static_assert(RASM_BUFFER_USAGE_TRANSFER == static_cast<uint32_t>(BufferUsage::TRANSFER));
    static_assert(RASM_BLEND_MODE_NONE == static_cast<uint32_t>(BlendMode::NONE));
    static_assert(RASM_BLEND_MODE_PREMULTIPLIED_ALPHA == static_cast<uint32_t>(BlendMode::PREMULTIPLIED_ALPHA));
    static_assert(RASM_BLEND_MODE_ADDITIVE == static_cast<uint32_t>(BlendMode::ADDITIVE));
    static_assert(RASM_BLEND_MODE_WEIGHTED == static_cast<uint32_t>(BlendMode::WEIGHTED));
    static_assert(RASM_BLEND_MODE_WEIGHTED_MOTION == static_cast<uint32_t>(BlendMode::WEIGHTED_MOTION));
    static_assert(RASM_BLEND_MODE_ADDITIVE_COLOR == static_cast<uint32_t>(BlendMode::ADDITIVE_COLOR));
    static_assert(RASM_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST == static_cast<uint32_t>(PrimitiveTopology::TRIANGLE_LIST));
    static_assert(RASM_PRIMITIVE_TOPOLOGY_LINE_LIST == static_cast<uint32_t>(PrimitiveTopology::LINE_LIST));
    static_assert(RASM_PRIMITIVE_TOPOLOGY_POINT_LIST == static_cast<uint32_t>(PrimitiveTopology::POINT_LIST));
    static_assert(RASM_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP == static_cast<uint32_t>(PrimitiveTopology::TRIANGLE_STRIP));
    static_assert(RASM_PRIMITIVE_TOPOLOGY_LINE_STRIP == static_cast<uint32_t>(PrimitiveTopology::LINE_STRIP));
    static_assert(RASM_CULL_MODE_NONE == static_cast<uint32_t>(CullMode::NONE));
    static_assert(RASM_CULL_MODE_FRONT == static_cast<uint32_t>(CullMode::FRONT));
    static_assert(RASM_CULL_MODE_BACK == static_cast<uint32_t>(CullMode::BACK));
    static_assert(RASM_SAMPLER_TYPE_LINEAR_REPEAT == static_cast<uint32_t>(SamplerType::LINEAR_REPEAT));
    static_assert(RASM_SAMPLER_TYPE_LINEAR_MIRRORED_REPEAT == static_cast<uint32_t>(SamplerType::LINEAR_MIRRORED_REPEAT));
    static_assert(RASM_SAMPLER_TYPE_LINEAR_CLAMP == static_cast<uint32_t>(SamplerType::LINEAR_CLAMP));
    static_assert(RASM_SAMPLER_TYPE_NEAREST_REPEAT == static_cast<uint32_t>(SamplerType::NEAREST_REPEAT));
    static_assert(RASM_SAMPLER_TYPE_NEAREST_MIRRORED_REPEAT == static_cast<uint32_t>(SamplerType::NEAREST_MIRRORED_REPEAT));
    static_assert(RASM_SAMPLER_TYPE_NEAREST_CLAMP == static_cast<uint32_t>(SamplerType::NEAREST_CLAMP));
    static_assert(RASM_SAMPLER_TYPE_NEAREST_CLAMP + 1 == static_cast<uint32_t>(SamplerType::COUNT));
    static_assert(RASM_PIPELINE_STAGE_COMPUTE == static_cast<uint32_t>(PipelineStage::COMPUTE));
    static_assert(RASM_PIPELINE_STAGE_GRAPHICS == static_cast<uint32_t>(PipelineStage::GRAPHICS));
    static_assert(RASM_PIPELINE_STAGE_HOST == static_cast<uint32_t>(PipelineStage::HOST));
}

namespace
{
    rasm_buffer_desc getBufferDefaults()
    {
        return {.size = sizeof(rasm_buffer_desc), .usage = RASM_BUFFER_USAGE_STORAGE};
    }

    rasm_pipeline_desc getPipelineDefaults()
    {
        return {.size = sizeof(rasm_pipeline_desc), .depth_test = RASM_TRUE, .depth_write = RASM_TRUE};
    }

    // One stage's shader: its entry point compiled from the desc's Slang source, or its own SPIR-V.
    ShaderHandle createStage(Renderer &renderer, const rasm_pipeline_desc &desc, const rasm_shader_stage &stage, ShaderType type, const char *defaultEntry, const std::string &name, const char *suffix,
                             const char *function)
    {
        // SPIR-V names its entry point main unless its compiler was told to keep the source's name.
        const std::string entry = stage.entry_point != nullptr ? stage.entry_point : desc.source != nullptr ? defaultEntry
                                                                                                            : "main";
        if (desc.source != nullptr)
            return renderer.createShader(name, desc.source, type, entry);
        if (stage.spirv == nullptr || stage.spirv_size == 0 || stage.spirv_size % 4 != 0)
        {
            c::reportError(function, ("desc has no source, so its " + entry + " stage needs SPIR-V: spirv set and spirv_size a multiple of 4 above 0.").c_str());
            return {};
        }
        const auto *bytes = static_cast<const uint8_t *>(stage.spirv);
        return renderer.getRenderContext().createShader({.name = name + suffix, .type = type, .entryPoint = entry, .code = std::vector<uint8_t>(bytes, bytes + stage.spirv_size)});
    }

    // Also what it draws into, for rasm_cmd_bind_pipeline's check against the pass.
    PipelineHandle createPipeline(Renderer &renderer, const rasm_pipeline_desc &values, c::PipelineInfo &info, const char *function)
    {
        RenderContext &ctx = renderer.getRenderContext();
        const std::string name = values.name != nullptr ? values.name : "";
        PipelineDesc desc = {.name = "Pipeline for " + name, .descriptorSetLayout = ctx.getBindlessDescriptorSetLayout()};
        if (values.type == RASM_PIPELINE_TYPE_COMPUTE)
        {
            desc.computeShader = createStage(renderer, values, values.compute, ShaderType::COMPUTE, "compMain", name, ".comp", function);
            // An invalid handle would turn the desc into a graphics pipeline with no shaders.
            return desc.computeShader.isValid() ? ctx.createPipeline(desc) : PipelineHandle{};
        }

        if (values.target.id != 0)
        {
            const RenderTargetHandle target = c::toCpp(values.target);
            if (!ctx.isAlive(target))
            {
                c::reportError(function, "desc->target is not a live render target.");
                return {};
            }
            const RenderTarget &rt = ctx.getRenderTarget(target);
            std::copy(rt.colorFormats, rt.colorFormats + MAX_COLOR_ATTACHMENTS, std::begin(desc.colorAttachmentFormats));
            desc.depthStencilAttachmentFormat = rt.depthFormat;
            desc.samples = rt.samples;
        }
        else
            desc.colorAttachmentFormats[0] = ctx.getSwapchainImageFormat(c::toCpp(values.window));
        if (desc.colorAttachmentFormats[0] == Format::UNKNOWN && desc.depthStencilAttachmentFormat == Format::UNKNOWN)
        {
            c::reportError(function, "the pipeline would draw into nothing: desc->window has no swapchain.");
            return {};
        }
        desc.vertexShader = createStage(renderer, values, values.vertex, ShaderType::VERTEX, "vertMain", name, ".vert", function);
        desc.fragmentShader = createStage(renderer, values, values.fragment, ShaderType::FRAGMENT, "fragMain", name, ".frag", function);
        if (!desc.vertexShader.isValid() || !desc.fragmentShader.isValid())
            return {};
        desc.topology = static_cast<PrimitiveTopology>(values.topology);
        desc.blendMode = static_cast<BlendMode>(values.blend_mode);
        desc.depthTest = values.depth_test != 0;
        desc.depthWrite = values.depth_write != 0;
        std::copy(std::begin(desc.colorAttachmentFormats), std::end(desc.colorAttachmentFormats), std::begin(info.colors));
        info.depth = desc.depthStencilAttachmentFormat;
        info.samples = desc.samples;
        return ctx.createPipeline(desc);
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_buffer_desc_init(rasm_buffer_desc *desc, uint32_t size)
try
{
    const rasm_buffer_desc defaults = getBufferDefaults();
    return c::initSized(desc, size, c::BUFFER_DESC_SIZE_1_17, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_buffer RASM_CALL rasm_create_buffer(rasm_renderer *renderer, const rasm_buffer_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_buffer_desc values = getBufferDefaults();
    if (r == nullptr || !c::readSized(desc, c::BUFFER_DESC_SIZE_1_17, &values, sizeof(values), __func__, "desc") ||
        !c::checkEnum(values.usage, RASM_BUFFER_USAGE_TRANSFER + 1, __func__, "desc->usage"))
        return {};
    if (values.byte_size == 0)
    {
        c::reportError(__func__, "desc->byte_size must be above 0.");
        return {};
    }
    const BufferHandle buffer = r->createBuffer({.name = values.name != nullptr ? values.name : "",
                                                 .usage = static_cast<BufferUsage>(values.usage),
                                                 .size = values.byte_size,
                                                 .hostVisible = values.host_visible != 0 || values.host_readable != 0,
                                                 .hostReadable = values.host_readable != 0,
                                                 .staged = values.staged != 0});
    if (buffer.isValid())
    {
        auto &buffers = renderer->buffers;
        if (buffer.index >= buffers.size())
            buffers.resize(buffer.index + 1);
        buffers[buffer.index] = {.generation = buffer.generation, .usage = static_cast<BufferUsage>(values.usage), .size = values.byte_size};
    }
    return c::toC(buffer);
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_destroy_buffer(rasm_renderer *renderer, rasm_buffer buffer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(buffer.id, __func__, "buffer"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (!c::checkNotRecording(renderer, __func__))
        return RASM_ERROR_FAILED;
    const c::CallErrors errors;
    const BufferHandle handle = c::toCpp(buffer);
    r->destroyBuffer(handle);
    if (handle.index < renderer->buffers.size() && renderer->buffers[handle.index].generation == handle.generation)
        renderer->buffers[handle.index] = {};
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_write_buffer(rasm_renderer *renderer, rasm_buffer buffer, const void *data, uint64_t size, uint64_t offset)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(buffer.id, __func__, "buffer") || !c::checkPointer(data, __func__, "data"))
        return RASM_ERROR_INVALID_ARGUMENT;
    return r->getRenderContext().fillBuffer(c::toCpp(buffer), data, size, offset) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_read_buffer(rasm_renderer *renderer, rasm_buffer buffer, void *data, uint64_t size, uint64_t offset)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(buffer.id, __func__, "buffer") || !c::checkPointer(data, __func__, "data"))
        return RASM_ERROR_INVALID_ARGUMENT;
    return r->getRenderContext().readBuffer(c::toCpp(buffer), data, size, offset) ? RASM_OK : RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT

uint64_t RASM_CALL rasm_get_buffer_device_address(rasm_renderer *renderer, rasm_buffer buffer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(buffer.id, __func__, "buffer"))
        return 0;
    return r->getRenderContext().getBufferDeviceAddress(c::toCpp(buffer));
}
RASM_C_CATCH(0)

rasm_result RASM_CALL rasm_pipeline_desc_init(rasm_pipeline_desc *desc, uint32_t size)
try
{
    const rasm_pipeline_desc defaults = getPipelineDefaults();
    return c::initSized(desc, size, c::PIPELINE_DESC_SIZE_1_17, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

// A pipeline is never destroyed, as in C++: the same desc gives the same one back. Its type and formats are kept by its
// index, so rasm_cmd_bind_pipeline can tell a pass that cannot run it.
rasm_pipeline RASM_CALL rasm_create_pipeline(rasm_renderer *renderer, const rasm_pipeline_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_pipeline_desc values = getPipelineDefaults();
    if (r == nullptr || !c::readSized(desc, c::PIPELINE_DESC_SIZE_1_17, &values, sizeof(values), __func__, "desc") ||
        !c::checkEnum(values.type, RASM_PIPELINE_TYPE_COMPUTE + 1, __func__, "desc->type") || !c::checkEnum(values.blend_mode, RASM_BLEND_MODE_ADDITIVE_COLOR + 1, __func__, "desc->blend_mode") ||
        !c::checkEnum(values.topology, RASM_PRIMITIVE_TOPOLOGY_LINE_STRIP + 1, __func__, "desc->topology"))
        return {};
    c::PipelineInfo info = {.type = static_cast<uint8_t>(values.type + 1)};
    const PipelineHandle pipeline = createPipeline(*r, values, info, __func__);
    if (!pipeline.isValid())
        return {};
    auto &pipelines = renderer->pipelines;
    if (pipeline.index >= pipelines.size())
        pipelines.resize(pipeline.index + 1);
    pipelines[pipeline.index] = info;
    return c::toC(pipeline);
}
RASM_C_CATCH({})

uint32_t RASM_CALL rasm_get_bindless_texture_index(rasm_renderer *renderer, rasm_texture texture)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(texture.id, __func__, "texture"))
        return RASM_INVALID_INDEX;
    return r->getBindlessTextureIndex(c::toCpp(texture));
}
RASM_C_CATCH(RASM_INVALID_INDEX)

uint32_t RASM_CALL rasm_add_bindless_texture(rasm_renderer *renderer, rasm_texture texture, rasm_sampler_type sampler)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(texture.id, __func__, "texture") || !c::checkEnum(sampler, RASM_SAMPLER_TYPE_NEAREST_CLAMP + 1, __func__, "sampler"))
        return RASM_INVALID_INDEX;
    return r->addBindlessTexture(r->getBindlessDescriptorSet(), c::toCpp(texture), static_cast<SamplerType>(sampler));
}
RASM_C_CATCH(RASM_INVALID_INDEX)

rasm_texture RASM_CALL rasm_get_render_target_color(rasm_renderer *renderer, rasm_render_target target, uint32_t attachment)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return {};
    return c::toC(r->getRenderTargetTexture(c::toCpp(target), attachment));
}
RASM_C_CATCH({})

rasm_texture RASM_CALL rasm_get_previous_render_target_color(rasm_renderer *renderer, rasm_render_target target, uint32_t attachment)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return {};
    return c::toC(r->getPreviousRenderTargetTexture(c::toCpp(target), attachment));
}
RASM_C_CATCH({})

rasm_texture RASM_CALL rasm_get_render_target_depth(rasm_renderer *renderer, rasm_render_target target)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return {};
    return c::toC(r->getRenderTargetDepthTexture(c::toCpp(target)));
}
RASM_C_CATCH({})

uint32_t RASM_CALL rasm_get_render_target_storage_index(rasm_renderer *renderer, rasm_render_target target, uint32_t attachment)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(target.id, __func__, "target"))
        return RASM_INVALID_INDEX;
    return r->getRenderTargetStorageIndex(c::toCpp(target), attachment);
}
RASM_C_CATCH(RASM_INVALID_INDEX)

uint32_t RASM_CALL rasm_get_gpu_timer_count(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? r->getFrameStats().gpuTimerCount : 0;
}
RASM_C_CATCH(0)

uint32_t RASM_CALL rasm_get_gpu_timer_name(rasm_renderer *renderer, uint32_t index, char *buffer, uint32_t capacity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return 0;
    const FrameStats &stats = r->getFrameStats();
    return index < stats.gpuTimerCount ? c::copyString(stats.gpuTimers[index].name, buffer, capacity) : 0;
}
RASM_C_CATCH(0)

float RASM_CALL rasm_get_gpu_timer_ms(rasm_renderer *renderer, uint32_t index)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return 0.0f;
    const FrameStats &stats = r->getFrameStats();
    return index < stats.gpuTimerCount ? stats.gpuTimers[index].milliseconds : 0.0f;
}
RASM_C_CATCH(0.0f)
// NOLINTEND(readability-identifier-naming)
