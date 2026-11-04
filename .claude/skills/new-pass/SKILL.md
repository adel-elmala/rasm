---
name: new-pass
description: Add a render pass to RASM — a full-screen/post-process pass (tone map, blur, vignette, debug view, overlay), a scene pass into an offscreen render target, or a pass with its own shader and parameters — through the render graph. Covers the shader file, pipeline creation, pass ordering, reading earlier targets, passing parameters, the current RHI limits (8-byte push constants, fixed pipeline state, no compute yet), and verification. Use whenever a new pass, post effect or shader-driven draw is added.
---

# New pass

Background: `docs/architecture.md` §9 (render graph) and §4 (GPU-driven path); `examples/01_render_graph`
is the working reference (forward pass into a render target, then an overlay pass that samples it).

## 1. Decide what kind of pass

| Kind | How today | Notes |
|---|---|---|
| **Full-screen / post** (tone map, blur, debug view) | own pipeline, `ctx.draw(cmd, 3)`, samples an earlier render target | the common case. Template below. |
| **Scene into a render target** | `engine.drawScene(scene, camera)` in the pass | uses the uber shader. The target must have the swapchain color format + `D32_SFLOAT_S8_UINT`, else `drawScene` logs and skips. |
| **Scene with its own shader** (shadow map, normals view, ID buffer) | ✗ not possible from the app | the custom shader can't get the `SceneData` address. Needs an engine change: propose it first. |
| **Compute** (culling, Hi-Z, particles) | ✗ no compute pipelines in the RHI yet | development plan phase 2 ("Compute pipelines and compute passes in the RHI"). Propose the RHI work first, don't fake it. |
| **Pass built into the engine** (every app gets it) | ✗ `Engine::render` records one fixed pass | phase 3 ("graph-driven frame"). Propose first. |

## 2. Current limits (check before designing)

| Limit | Value | Where |
|---|---|---|
| Push constants | **8 bytes total**, `ShaderType::VERTEX_FRAGMENT` | range = `sizeof(VkDeviceAddress)`, `VulkanDevice::createGraphicsPipeline` (`C12`) |
| Entry points | `vertMain` + `fragMain` in **one source string** | `Engine::createPipeline` |
| Attachments | 1 color (swapchain format) + `D32_SFLOAT_S8_UINT` | `Engine::createPipeline` |
| Fixed state | depth test + write `LESS_OR_EQUAL`, no blending, cull none, triangle list, no vertex input | `createGraphicsPipeline` |
| Every pass **clears** its target | two passes can't accumulate into one target | `VulkanCommandBuffer::beginRendering`, review §3 |
| Readable outputs | only the **color** of an earlier render target, through its bindless index. Depth isn't stored. | `Engine::endPass` |
| Ordering | from `builder.read`/`write` only, topologically sorted. No automatic barriers beyond the render target begin/end transitions | `RenderGraph::compile` |
| Camera | `execute()` uses the current scene's first camera for `beginFrame` | `RenderGraph::execute` |

If a design needs to go past a limit (blending, more push constants, depth reads, MRT), that's an
RHI/engine change: propose it in a sentence and wait (CLAUDE.md).

## 3. Template: full-screen pass

Shader in `shaders/passes/<name>.slang` (picked up at startup, no build):

```slang
struct PushConstants { uint sourceIndex; float strength; };   // ≤ 8 bytes
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;
[[vk::binding(0, 0)]] Sampler2D gTextures[];

struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID)
{
    VSOutput o;
    o.uv = float2((vertexID << 1) & 2, vertexID & 2);   // one triangle covers the screen
    o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    float3 color = gTextures[pc.sourceIndex].Sample(input.uv).rgb;
    return float4(color, 1.0);   // linear color, like every RASM shader
}
```

C++ (application side, e.g. an example's `main.cpp`):

```cpp
std::ifstream file(std::string(RASM_EXAMPLE_SHADER_DIR) + "/passes/<name>.slang");
std::stringstream source;
source << file.rdbuf();
rasm::PipelineHandle pipeline = engine.createPipeline("<name>", source.str());
if (!pipeline.isValid())
    return 1;

graph.addPass(
    "<Name>",
    [&](rasm::PassBuilder &builder)
    {
        builder.read(sceneTarget);          // the render target an earlier pass wrote
        // builder.write(outputTarget);     // omit to draw to the swapchain
    },
    [&](rasm::RenderContext &ctx)
    {
        auto commandBuffer = engine.getCurrentFrameResources().commandBuffer;
        struct { uint32_t sourceIndex; float strength; } push = {
            engine.getBindlessTextureIndex(engine.getRenderTargetTexture(sceneTarget)), 0.8f};
        ctx.bindPipeline(commandBuffer, pipeline);
        ctx.bindDescriptorSet(commandBuffer, pipeline, engine.getBindlessDescriptorSet(), 0);
        ctx.pushConstants(commandBuffer, pipeline, rasm::ShaderType::VERTEX_FRAGMENT, &push, sizeof(push), 0);
        ctx.draw(commandBuffer, 3);
    });
```

- Look up the bindless index **inside** `execute`: each frame slot has its own attachment.
- The scene target needs `clearColor` and the swapchain format. Copy `examples/01_render_graph`.
- **More than 8 bytes of parameters**: put them in a `BufferDesc{.usage = STORAGE, .hostVisible = true}` buffer, `ctx.fillBuffer(...)`, push `ctx.getBufferDeviceAddress(buffer)` (8 bytes), and read it through a pointer in Slang (`MyParams* params;`), like `SceneData`. Parameters that change every frame need one buffer per frame slot, but the app can't see the slot index yet, so fill once or propose exposing it.
- Shader structs shared with C++ through a pointer follow the `gpu-struct-sync` layout rules.

## 4. Where the code goes

- A new example for a new kind of pass: `examples/NN_name/` (see CLAUDE.md "Adding files"), with validation on.
- Engine code (only after the user agreed to an engine change): the pass gets its own `.h`/`.cpp` pair under `src/renderer/` (CLAUDE.md "one file pair per part"), and is added to `RASM_SRC`.

## 5. Verify

1. `verify-change` skill: baseline before, `run` after, with `--sync` (passes add barriers and layout transitions). New example → its screenshot has no baseline yet, so look at it (Read) and judge.
2. Wrong output → `frame-debug` skill: `rdc.py actions` shows each pass's begin/draw, and `targets <eid>` shows what each pass wrote.
3. Cost → `tracy-profile` (CPU only. GPU pass timing isn't instrumented yet: `D16`).
4. `finish-task` skill: architecture.md §9 if graph behavior changed, the plan item if it closes one.
