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

- **A few indirect draws per scene**: one `vkCmdSetCullMode` + `vkCmdDrawIndexedIndirect(offset = firstDraw × 20, drawCount = run length)` per run of draws with the same cull mode (usually NONE, FRONT, BACK, each only if used), stride 20 (`gfx::DrawInfo`: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance). RenderDoc expands each into `Indirect sub-command[i]` events. **The draw's index in the scene's draw list is `firstDraw + i`**, not `i`: use it for `DrawInfo`, whose `firstInstance` is the draw's row in the instance table (`InstanceData`: matrices, `geometryIndex`, `materialIndex`).
- **Push constants (16 bytes) = `{SceneData address, firstDraw, padding}`** for this frame slot. `rdc.py state` resolves the address to `Scene Data Buffer + offset`; `firstDraw` changes between the indirect calls.
- **`SceneData` layout** (scalar, `src/renderer/uberMaterialTypes.h`): projection mat4 @0, view mat4 @64, cameraWorldPosition f32x3 @128, numLights u32 @140, lights[MAX_LIGHTS=8] × 48 B @144, vertsPtr @528, drawInfoPtr @536, instancesPtr @544, materialsPtr @552, geometryPtr @560, size 568. The buffer holds several `SceneData`s back to back. Recheck this against the header if the struct changed.
- **Pointer chain**: `vertsPtr` → `Geometry Vertex Pool` (`VertexFull`, 64 B), `drawInfoPtr` → `Draw Info Buffer` (the scene's draw list, one per frame slot), `instancesPtr` → `Instance Table 0`/`1` (`InstanceData`, 108 B), `materialsPtr` → `Material Table 0`/`1` (`MaterialData`, 84 B, row = material handle index), `geometryPtr` → `Geometry Table` (`GeometryData`, 28 B). The table names end in the frame slot.
- **Resources have debug names** (`Draw Info Buffer`, `Scene Data Buffer`, `DepthTexture for Frame 0`, `glTF image N`, texture file paths), so look them up by name.
- **Bindless textures**: set 0 binding 0 array; the element index is the material's `*TextureIndex`.
- **Two frames in flight**: per-frame buffers exist twice. Use the one the push constant / `SceneData` pointers name.
- Depth is reversed-Z: `D32_SFLOAT`, `GreaterEqual`, cleared to 0 (far), near plane at 1, infinite far plane by default. Front face is counter-clockwise, in a Y-up clip space flipped by a negative-height viewport. Cull mode is dynamic: `NONE` for double-sided materials, `BACK`, or `FRONT` for a mirrored entity. A missing object with `backfaceCulled` in `pixel` history usually means wrong winding or a wrong `doubleSided`.

## Known quirks

- Machine-specific loader noise (e.g. Epic Games overlay layer JSON missing) is harmless.
- GFXR `dump --before` reports all-zero indirect params. Run without `--before` to read the indirect args.
- Don't capture with both tools at once. `gfxr.py` turns the GFXR layer on only for its own child process, so don't set `VK_INSTANCE_LAYERS` globally.
