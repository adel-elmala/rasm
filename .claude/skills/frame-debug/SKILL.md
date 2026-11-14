---
name: frame-debug
description: Capture and inspect a GPU frame of a RASM example to find rendering bugs — black/wrong output, missing objects, wrong materials, depth/blend/cull problems, bad indirect draws or buffer-device-address data. Uses RenderDoc (Windows/Linux) and GFXReconstruct (Windows/Linux/macOS, Vulkan) through scripts, so results come back as text and PNGs. Use when rendering looks wrong, after changing shaders/passes/GPU structs, or when the user asks to debug a frame.
---

# Frame debug

Capture one frame of an example, then read it with scripts: draws, pipeline state,
buffers, render targets as PNG (open them with Read), pixel history.

## Pick the tool

| Need | Tool | Platforms |
|---|---|---|
| Pipeline state, any buffer by name or GPU address, pixel history, shader disassembly | **RenderDoc** → [renderdoc.md](renderdoc.md) | Windows, Linux |
| Render targets before/after each draw, bound textures, frame as JSON, replay screenshot | **GFXReconstruct** → [gfxr.md](gfxr.md) | Windows, Linux, macOS (MoltenVK) |
| Metal (future backend) | Xcode GPU capture: the user opens it, scripts can't read it | macOS |

Default: RenderDoc on Windows/Linux, GFXR on macOS or when RenderDoc can't capture.
RenderDoc matters most for RASM, because scene data travels through buffer device
addresses, and GFXR only dumps buffers bound through descriptors.

## Workflow

```
1. build          cmake --build --preset x64-debug --target <example>
2. capture        frame 60 by default (async asset loads are done by then)
3. overview       rdc.py actions   /  gfxr.py frame
4. look           render targets as PNG → Read them
5. narrow down    state / buffer / pixel at the bad draw
6. fix → rebuild → capture again and compare
```

Put captures and dumps in the scratchpad directory, never in the repo (`.rdc` ≈ 200 MB, `.gfxr` ≈ 100 MB).

Before capturing, make sure the build is current: a stale exe against newer shaders
fails `validateRasmStructs` at startup (assert in `shaderCompiler.cpp`), before any frame.

## RASM facts that help reading a capture

- **The scene is render graph passes** (`Engine::addScenePasses`), all in the frame's one command buffer: `Cull 1` (compute: early `countMain`, a barrier, `writeMain`; `cull.slang`, 64 draws per group, each group inside one run), `Scene 1` (the early draws), and with occlusion culling `Hi-Z`, `Cull 2` and `Scene 2`; without it only `Cull` and `Scene`. The graph's barriers sit between the passes. A draw missing from the visible list was culled: compare `SceneData.drawCount` (@720) with the sum of the view's draw counts (the `MAX_DRAW_RUNS` u32s at the end of its `Visible Draw Buffer`, at `drawCountsPtr`).
- **Occlusion culling** (`EngineConfig::occlusionCulling`, on by default): after `Scene 1`, a depth → `SHADER_READ_ONLY` barrier, the `Hi-Z` pass (one `hizMain` dispatch per level, push constant `argument` = level), `Cull 2` (late `countMain` + `writeMain`, `argument` = 1), then `Scene 2`, which begins rendering with `LOAD` and records the same indirect draws again for the late draws. They reuse the `Visible Draw Buffer` (draw counts included), so a capture shows the late values at the end of the frame. A draw missing from both was frustum culled or occluded: look at its row in the view's `Visibility Buffer` (u32 per instance row, one buffer per scene view) and the view's `Hi-Z Buffer` (floats, level 0 = half the target's size, levels packed). To rule occlusion out, set `occlusionCulling = false` and compare.
- **A few indirect draws per scene**: one `vkCmdSetCullMode` + `vkCmdDrawIndexedIndirectCount(offset = firstDraw × 20, count = that run's u32 among the draw counts at the end of the Visible Draw Buffer, max = run length)` per run of draws with the same cull mode (usually NONE, FRONT, BACK, each only if used), stride 20 (`gfx::DrawInfo`: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance). RenderDoc expands each into `Indirect sub-command[i]` events, as many as were visible. **The draw's index in the call's visible list is `firstDraw + i`**, not `i`: use it for `DrawInfo` in `Visible Draw Buffer`, whose `firstInstance` is the draw's row in the instance table (`InstanceData`: matrices, `geometryIndex`, `materialIndex`). The visible draws keep the draw list's order.
- **Push constants (16 bytes) = `{SceneData address, firstDraw, argument}`** (`argument` is for compute: cull phase or Hi-Z level) for this frame slot. `rdc.py state` resolves the address to `Scene Data Buffer + offset`; `firstDraw` changes between the indirect calls.
- **`SceneData` layout** (scalar, `src/renderer/uberMaterialTypes.h`): projection mat4 @0, view mat4 @64, cameraWorldPosition f32x3 @128, numLights u32 @140, lights[MAX_LIGHTS=8] × 48 B @144, vertsPtr @528, drawInfoPtr @536, instancesPtr @544, materialsPtr @552, geometryPtr @560, allDrawsPtr @568, drawCountsPtr @576, cullSlotsPtr @584, groupCountsPtr @592, cullStatsPtr @600, visibilityPtr @608, hizPtr @616, frustumPlanes f32x4 × 6 @624, drawCount u32 @720, runCount u32 @724, runFirst u32 × 3 @728, hizLevelCount u32 @740 (0 = no occlusion culling), depthTexture u32 @744 (bindless index), viewportWidth u32 @748, viewportHeight u32 @752, lodErrorPixels f32 @756, forcedLod i32 @760, padding u32 @764, size 768. The buffer holds several `SceneData`s back to back. Recheck this against the header if the struct changed.
- **Pointer chain**: `vertsPtr` → `Geometry Vertex Pool` (`VertexFull`, 64 B), `allDrawsPtr` → `Draw Info Buffer` (the scene's whole draw list, one per frame slot), `drawInfoPtr` → `Visible Draw Buffer` (the draws that passed culling, packed per run, one per frame slot and scene view; `cullSlotsPtr`, `groupCountsPtr` and `drawCountsPtr` point into it after the draws; the draw counts are its last 3 × u32, the visible draws per run), `instancesPtr` → `Instance Table 0`/`1` (`InstanceData`, 108 B), `materialsPtr` → `Material Table 0`/`1` (`MaterialData`, 84 B, row = material handle index), `geometryPtr` → `Geometry Table` (`GeometryData`, 120 B: bounds and up to 8 LOD index ranges), `cullStatsPtr` → `Cull Stats Buffer 0`/`1` (13 × u32, `STATS_*` in `cull.slang`: visible draws; visible triangles low, high; the same at LOD 0 low, high; draws per LOD 0–7; added up by `countMain`). The table names end in the frame slot.
- **Resources have debug names** (`Draw Info Buffer`, `Scene Data Buffer`, `DepthTexture for Frame 0`, `glTF image N`, texture file paths), so look them up by name.
- **Bindless textures**: set 0 binding 0 array; the element index is the material's `*TextureIndex`.
- **Two frames in flight**: per-frame buffers exist twice. Use the one the push constant / `SceneData` pointers name.
- Depth is reversed-Z: `D32_SFLOAT`, `GreaterEqual`, cleared to 0 (far), near plane at 1, infinite far plane by default. Front face is counter-clockwise, in a Y-up clip space flipped by a negative-height viewport. Cull mode is dynamic: `NONE` for double-sided materials, `BACK`, or `FRONT` for a mirrored entity. A missing object with `backfaceCulled` in `pixel` history usually means wrong winding or a wrong `doubleSided`.

## Known quirks

- Machine-specific loader noise (e.g. Epic Games overlay layer JSON missing) is harmless.
- GFXR `dump --before` reports all-zero indirect params. Run without `--before` to read the indirect args.
- Don't capture with both tools at once. `gfxr.py` turns the GFXR layer on only for its own child process, so don't set `VK_INSTANCE_LAYERS` globally.
