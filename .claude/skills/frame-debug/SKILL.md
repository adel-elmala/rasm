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

Captures need a window. RenderDoc and GFXR cut frames at `vkQueuePresentKHR`, and a headless run (`--headless`) never
presents, so capture the example windowed (it opens a window until frame N). To only look at frame N, or to check
validation, run it headless instead: `<exe> --fixed-step --wait-for-loads --headless --frames 60 --out shot.png`.

Before capturing, make sure the build is current: a stale exe against newer shaders
fails `validateRasmStructs` at startup (assert in `shaderCompiler.cpp`), before any frame.

## RASM facts that help reading a capture

- **The scene is render graph passes** (`Renderer::addScenePasses`), all in the frame's one command buffer: `Cull 1` (compute: early `countMain`, a barrier, `writeMain`; `cull.slang`, 64 draws per group, each group inside one run), `Scene 1` (the early draws), and with occlusion culling `Hi-Z`, `Cull 2` (which also sorts the blend run, `Blend Sort` timer) and `Scene 2`; without it only `Cull` and `Scene`. A shaded view ends with `Transparent`, the `BLEND` draws back to front after the sky. The graph's barriers sit between the passes. A draw missing from the visible list was culled: compare `SceneData.drawCount` (@872) with the sum of the view's draw counts (the `MAX_DRAW_RUNS` u32s at the end of its `Visible Draw Buffer`, at `drawCountsPtr`).
- **Occlusion culling** (`EngineConfig::occlusionCulling`, on by default): after `Scene 1`, a depth → `SHADER_READ_ONLY` barrier, the `Hi-Z` pass (one `hizMain` dispatch per level, push constant `argument` = level), `Cull 2` (late `countMain` + `writeMain`, `argument` = 1), then `Scene 2`, which begins rendering with `LOAD` and records the same indirect draws again for the late draws. They reuse the `Visible Draw Buffer` (draw counts included), so a capture shows the late values at the end of the frame. A draw missing from both was frustum culled or occluded: look at its row in the view's `Visibility Buffer` (u32 per instance row, one buffer per scene view) and the view's `Hi-Z Buffer` (floats, level 0 = half the target's size, levels packed). To rule occlusion out, set `occlusionCulling = false` and compare.
- **A few indirect draws per scene**: one `vkCmdSetCullMode` + `vkCmdDrawIndexedIndirectCount(offset = firstDraw × 20, count = that run's u32 among the draw counts at the end of the Visible Draw Buffer, max = run length)` per run of draws with the same alpha mode and cull mode (`NONE` then `MASK`, each NONE, FRONT, BACK if used, then one `BLEND` run with cull NONE that only the `Transparent` pass draws in a shaded view), each run with its alpha mode's pipeline, stride 20 (`gfx::DrawInfo`: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance). RenderDoc expands each into `Indirect sub-command[i]` events, as many as were visible. **The draw's index in the call's visible list is `firstDraw + i`**, not `i`: use it for `DrawInfo` in `Visible Draw Buffer`, whose `firstInstance` is the draw's row in the instance table (`InstanceData`: matrices, `geometryIndex`, `materialIndex`). The visible draws keep the draw list's order.
- **Push constants (16 bytes) = `{SceneData address, firstDraw, argument}`** (`argument` is for compute: cull phase or Hi-Z level) for this frame slot. `rdc.py state` resolves the address to `Scene Data Buffer + offset`; `firstDraw` changes between the indirect calls.
- **`SceneData` layout** (scalar, `src/renderer/uberMaterialTypes.h`): projection mat4 @0, view mat4 @64, originHigh f32x3 @128 (the view's origin, its camera; render space = world − origin), shadowedLightCount u32 @140, vertsPtr @144, drawInfoPtr @152, instancesPtr @160, materialsPtr @168, geometryPtr @176, allDrawsPtr @184, drawCountsPtr @192, cullSlotsPtr @200, groupCountsPtr @208, cullStatsPtr @216, visibilityPtr @224, hizPtr @232, colorsPtr @240 (vertex color pool), sortKeysPtr @248, sortDrawsPtr @256 (blend sort scratch), lightsPtr @264 (the light table: `Light` × 68 B, row = handle index; each: position @0 (high part), intensity @12, color @16, range @28 (0 = global), direction @32, type u32 @44 (0 directional, 1 point, 2 spot), spotScale @48, spotOffset @52, positionLow @56), sceneLightsPtr @272 (u32 light rows: global ones, then local ones), clustersPtr @280 (0 = no clusters; layout in `shaders/lib/lightClusters.slang`), frustumPlanes f32x4 × 6 @288, drawCount u32 @384, runCount u32 @388, runFirst u32 × 7 @392, blendRun u32 @420 (7 = none), sortCapacity u32 @424, hizLevelCount u32 @428 (0 = no occlusion culling), depthTexture u32 @432 (bindless index), viewportWidth u32 @436, viewportHeight u32 @440, lodErrorPixels f32 @444, forcedLod i32 @448, shadowMaps[16] × 72 B @452 (each: matrix mat4 @0, texture u32 @64 (bindless index; a cube for a point light), split f32 @68 (a cascade's end as view depth)), lodPixelsPerUnit f32 @1604, lodDepthPlane f32x4 @1608 (0 = orthographic LOD camera), environmentSpecular u32 @1624 (bindless cube; 0xFFFFFFFF = no environment), environmentSpecularMips u32 @1628, environmentIrradiance u32 @1632, environmentSky u32 @1636 (0xFFFFFFFF = no sky), brdfLut u32 @1640, environmentIntensity f32 @1644, environmentRotation f32x2 @1648 (cos, sin), shadowedLights[4] × 12 B @1656 (each: light row u32 @0, firstMap u32 @4, mapCount u32 @8), globalLightCount u32 @1704, localLightCount u32 @1708, nearZ f32 @1712, depthSamples u32 @1716, previousViewProjection mat4 @1720 (unjittered, for motion), jitter f32x2 @1784 (NDC, 0 without TAA), textureLodBias f32 @1792, debugView u32 @1796 (`rasm::DebugView`, 0 = none), animatedTransformsPtr @1800 (this slot's `Animated Transform Buffer`), skinnedVertsPtr @1808 (this slot's `Skinned Vertex Buffer`), previousSkinnedVertsPtr @1816 (the other slot's), the meshlet fields @1824 to meshletDataPtr @2008, originLow f32x3 @2016, zero u32 @2028 (always 0), environmentOrientation f32x4 @2032 (quaternion x, y, z, w), labelsPtr @2048 (a shaded view's label slots' alpha, 0 = every label shown), grid GridData @2056 (168 B: the camera's reference grid, levelCount @2212 is 0 without one), labelProbesPtr @2224 (vec4 per slot: anchor pixel, depth, test), labelFadesPtr @2232 (the view's fades by depth), labelFadeStep f32 @2240, labelSlotCount u32 @2244 (0 = no Label Depth pass), size 2248. Positions in the GPU tables are split: an `InstanceData` row's translation is `world[i].w` + `translationLow`, in world space; subtract the view's origin to get what the shaders draw. The buffer holds several `SceneData`s back to back. Recheck this against the header if the struct changed.
- **Pointer chain**: `vertsPtr` → `Geometry Vertex Pool` (`VertexFull`, 64 B), `allDrawsPtr` → `Draw Info Buffer` (the scene's whole draw list, one per frame slot), `drawInfoPtr` → `Visible Draw Buffer` (the draws that passed culling, packed per run, one per frame slot and scene view; `cullSlotsPtr`, `groupCountsPtr` and `drawCountsPtr` point into it after the draws; the draw counts are its last 7 × u32, the visible draws per run), `instancesPtr` → `Instance Table 0`/`1` (`InstanceData`, 160 B: world, normal matrix, geometry and material index, previousWorld rows @108, animationRow i32 @156: -1, or the row in `Animated Transform Buffer 0`/`1` (`InstanceTransform`, 172 B: world @0, normal matrix @64, previousWorld rows @100, boundsCenter @148, boundsRadius @160 (-1: the geometry's), skinnedVertex i32 @164 (-1, or its first vertex in `Skinned Vertex Buffer 0`/`1`, which the `Skin` pass wrote, morphed and skinned), skinnedMotion u32 @168) that the `Animate` pass wrote and the shaders read instead), `materialsPtr` → `Material Table 0`/`1` (`MaterialData`, 220 B, row = material handle index: the material, its id, alpha mode, cutoff, flags and 5 texture transforms), `geometryPtr` → `Geometry Table` (`GeometryData`, 136 B: bounds, up to 8 LOD index ranges, `colorOffset`, `skinOffset` into `Geometry Skin Pool` (16 B `SkinVertex`es), `morphOffset` and `morphTargetCount` into `Geometry Morph Pool` (24 B `MorphDelta`s, target-major)), `colorsPtr` → `Geometry Color Pool` (u32 RGBA8 per vertex, for the sub-meshes with `COLOR_0`), `sortKeysPtr`/`sortDrawsPtr` → `Blend Sort Buffer` (keys, then the copied blend draws; a shaded view's, one per frame slot), `cullStatsPtr` → `Cull Stats Buffer 0`/`1` (13 × u32, `STATS_*` in `cull.slang`: visible draws; visible triangles low, high; the same at LOD 0 low, high; draws per LOD 0–7; added up by `countMain`). The table names end in the frame slot.
- **Resources have debug names** (`Draw Info Buffer`, `Scene Data Buffer`, `DepthTexture for Frame 0`, `glTF image N`, texture file paths), so look them up by name.
- **Bindless textures**: set 0 binding 0 array; the element index is the material's `*TextureIndex`.
- **Two frames in flight**: per-frame buffers exist twice. Use the one the push constant / `SceneData` pointers name.
- Depth is reversed-Z: `D32_SFLOAT`, `GreaterEqual`, cleared to 0 (far), near plane at 1, infinite far plane by default. Front face is counter-clockwise, in a Y-up clip space flipped by a negative-height viewport. Cull mode is dynamic: `NONE` for double-sided materials, `BACK`, or `FRONT` for a mirrored entity. A missing object with `backfaceCulled` in `pixel` history usually means wrong winding or a wrong `doubleSided`.

## Known quirks

- Machine-specific loader noise (e.g. Epic Games overlay layer JSON missing) is harmless.
- GFXR `dump --before` reports all-zero indirect params. Run without `--before` to read the indirect args.
- Don't capture with both tools at once. `gfxr.py` turns the GFXR layer on only for its own child process, so don't set `VK_INSTANCE_LAYERS` globally.
