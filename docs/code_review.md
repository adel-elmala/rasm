# RASM Code Review — October 2026

This review covers the whole repository as of `feat/pbr` @ `61d8028`. Every finding has an ID that the [development plan](development_plan.md) refers to.

| Prefix | Meaning |
|---|---|
| **B** | Bug: wrong behaviour, a crash, or undefined behaviour |
| **D** | Design or architecture issue |
| **P** | Performance |
| **C** | Code health and refactoring |

**Confidence.** *Confirmed* means the failure follows deterministically from the code as read. *Likely* means it follows from the code plus an assumption about driver or library behaviour; verify it at runtime before fixing.

---

## 1. Folder structure

This section describes the layout as of `6efdf48` and lists only the problems that remain.

### Current layout

```
CMakeLists.txt                  top level: options (RASM_ENABLE_AVX2, RASM_BUILD_EXAMPLES), add_subdirectory(...)
CMakePresets.json               x64-debug/release, macOS, macOS-release, macOS-Xcode
cmake/RasmExample.cmake         rasm_add_example()
include/rasm/                   public API (still includes third-party and private headers, see §1.1)
  rasm.h                        umbrella header
  engine.h scene.h entity.h transform.h camera.h light.h material.h mesh.h
  handle.h types.h window.h profiler.h
  render/                       low-level API: renderContext.h, renderGraph.h, resource.h
src/
  CMakeLists.txt                target `rasm`, alias `rasm::rasm`
  engine.cpp                    frame loop (not yet split into renderer/)
  core/                         handle, profiler, utils
  platform/                     window.cpp (SDL3 window + input)
  assets/                       material.cpp (incl. glTF material/texture import), mesh.cpp (glTF + OBJ)
  scene/                        scene (incl. prepareScene), entity, transform, camera, light
  renderer/                     renderGraph, shaderCompiler, uberMaterialTypes.h (shader ABI)
  rhi/                          renderContext.cpp (backend switch)
    vulkan/                     vulkanContext.{h,cpp}, vulkanTypes.h
  thirdparty_impl.cpp           the only *_IMPLEMENTATION defines
shaders/
  lib/                          core.slang (GPU structs), pbr.slang, unlit.slang
  passes/                       rasm.slang
examples/
  CMakeLists.txt
  00_hello_scene/ 01_render_graph/ 02_gltf/
tools/compile-shaders.bat       offline compile check only (output unused)
assets/models/{gltf,obj}/       sample models (binary, in plain git)
docs/
```

### Problems

1. **Public headers expose implementation details (`D2`).** *Fixed 2026-11-01:* public headers include nothing from `src/` and no third-party header except glm. `RenderContext` keeps its Vulkan state in a private `Impl`, and `Engine` holds `ShaderCompiler` through a forward declaration. `src/`, the backend libraries and the tinygltf/stb include paths are PRIVATE. The original finding follows.
   - `engine.h` includes the private `renderer/shaderCompiler.h` and `renderer/uberMaterialTypes.h`; `render/renderContext.h` includes `rhi/vulkan/vulkanTypes.h` and `rhi/vulkan/vulkanContext.h` (→ vk-bootstrap, VMA).
   - `types.h` and `mesh.h` include `tiny_gltf.h`, `window.h` includes `vulkan/vulkan.h`, and `render/renderGraph.h` includes `spdlog/spdlog.h`.
   - Because of this, `src/` is temporarily a PUBLIC include directory, and Vulkan, VMA, vk-bootstrap, Slang, glm, Tracy, spdlog and the tinygltf/stb include paths are all linked PUBLIC. Applications still compile against the whole backend.
   - This keeps compiles slow, makes the backend impossible to swap, and blocks a stable API/ABI.
2. **Some files still mix concerns.**
   - `scene/scene.cpp` holds `prepareScene` and pipeline creation (renderer logic);
   - `assets/material.cpp` holds both the material API and glTF texture import;
   - `engine.cpp` (frame loop) still sits at the root of `src/`.
3. **Shaders are found through the working directory (`D11`).** Paths are cwd-relative (`./shaders/passes/...` in `material.cpp`/`scene.cpp`, `cwd/shaders/lib` in `shaderCompiler.cpp`), so the examples only work when run from the repo root (not from the build directory, an IDE, or an installed app).
4. **No install/export.** There is no `install()`/`export()`, so `find_package(rasm)` does not work.
5. **No `tests/` and no CI** (plan phase 1.5).
6. **Binary assets live in plain git.** The glTF/OBJ sample models are committed directly, so the clone grows with every asset added.

### What is left of the target layout

Compared with the current layout, the target still needs:

```
include/rasm/                   PUBLIC API ONLY — no third-party or src/ headers (D2)
  config.h                      EngineConfig incl. assetRoot / shader dir
  render/rhi.h                  device-agnostic types replacing the Vk structs in RenderContext::Impl (D1)
src/
  renderer/                     + frame loop from engine.cpp, prepareScene/GPU scene from scene.cpp,
                                  passes (forward, culling, shadow, post)
  assets/gltf/ obj/ image/      split loaders out of material.cpp / mesh.cpp
  rhi/metal/ d3d12/ webgpu/     future backends
shaders/
  lib/                          + brdf, color, bindings modules as they are factored out
  passes/                       + cull, shadow, tonemap ...
  materials/                    material evaluation modules
tests/                          unit tests (handles, desc hashing, glTF import) + image tests
tools/                          + asset download script
assets/                         keep small; large models via Git LFS or a download script
```

Next steps, in order:
- Make the shader path configurable (`D11`): a `RASM_SHADER_DIR` compile definition and an `EngineConfig::assetRoot`, plus a post-build copy of `shaders/` next to the binaries.
- Keep moving code incrementally: move a file only while you are working on it, and change the CMake lists in the same commit.
- Add `install()`/`export()` and a `tests/` target alongside CI (phase 1.5).

---

## 2. Bugs

### High severity

**B1 — Creation dedup aliases resources that must be distinct.** *Fixed 2026-10-26:* buffer, texture, descriptor-pool and descriptor-set dedup has been removed. Shader and pipeline dedup is now keyed on full state; shader equality used to compare the SPIR-V *pointer*. glTF images are cached per model by image index and color space. Resize destroys the old depth textures. The original finding follows.
`RenderContext::create{Buffer,Texture,…}` returns an existing handle when it has seen an equal `ResourceDesc` before ([renderContext.cpp:136-140](../src/rhi/renderContext.cpp#L136-L140), [:171-175](../src/rhi/renderContext.cpp#L171-L175)). This bites in several places:
- `prepareScene` creates `megaModelDataBuffer[i]` and `sceneDataBuffer[i]` in a loop with an identical desc ([scene.cpp:204-208](../src/scene/scene.cpp#L204-L208), [:229-233](../src/scene/scene.cpp#L229-L233)). Both frame slots get the **same** buffer, so the CPU rewrites `SceneData`/`Modeldata` while the GPU may still be reading them for the previous frame. That undoes the frames-in-flight fix in `27bb89a`.
- Render-target attachments per frame slot alias each other ([renderContext.cpp:563-566](../src/rhi/renderContext.cpp#L563-L566)).
- Two OBJ meshes with the same vertex and index counts share one buffer, and the second upload overwrites the first ([mesh.cpp:179-191](../src/assets/mesh.cpp#L179-L191)).
- glTF textures are named by `image.uri`, which is empty for images embedded in `.glb` files ([material.cpp:182](../src/assets/material.cpp#L182)). Two embedded images of the same size and format therefore become one texture, and the last upload wins.
- Even a "correct" cache hit re-uploads the texture and takes a new bindless slot (see B7).

*Fix:* remove desc-based dedup from buffer and texture creation. Keep dedup only for immutable objects (samplers, pipeline layouts, pipelines), keyed on their *complete* state. Asset-level caching (by path, or by model + image index) belongs in the asset layer.

**B2 — The projection uses the OpenGL depth range [-1, 1].** *Fixed 2026-10-26:* `GLM_FORCE_DEPTH_ZERO_TO_ONE` is now a PUBLIC compile definition of the `rasm` target in [src/CMakeLists.txt](../src/CMakeLists.txt), and the define in `engine.cpp` was removed. The original finding follows.
- `#define GLM_FORCE_DEPTH_ZERO_TO_ONE` sits at [engine.cpp:9](../src/engine.cpp#L9), *after* `engine.h` has already included glm through `rasm/types.h`, so it has no effect. `camera.cpp` never sees it either.
- `glm::perspective`/`glm::ortho` therefore emit z in [-1, 1]. Vulkan clips z < 0, which moves the effective near plane to ≈ 2·n·f/(n+f) (≈ 0.2 for n = 0.1, f = 32) and wastes half the depth range.

*Fix:* set `GLM_FORCE_DEPTH_ZERO_TO_ONE` (and `GLM_FORCE_RADIANS` if wanted) with `target_compile_definitions(rasm PUBLIC ...)`, or call `glm::perspectiveRH_ZO` explicitly. After that, move to reversed-Z (see the plan, phase 1).

**B3 — Debug-name calls without the debug-utils extension.** *Fixed 2026-10-27:* `_init_instance` now enables `VK_EXT_debug_utils` whenever the instance reports it, independent of validation, and all naming goes through `VulkanContext::setObjectName`, which does nothing when the extension is not enabled. The original finding follows.
- `setDebugUtilsObjectNameEXT` is called for every buffer, image, view, shader and pipeline ([vulkanContext.cpp:232](../src/rhi/vulkan/vulkanContext.cpp#L232), [:425-426](../src/rhi/vulkan/vulkanContext.cpp#L425-L426), [:456](../src/rhi/vulkan/vulkanContext.cpp#L456), [:644](../src/rhi/vulkan/vulkanContext.cpp#L644)).
- `VK_EXT_debug_utils` is enabled only when validation is requested *and* available ([vulkanContext.cpp:1374-1380](../src/rhi/vulkan/vulkanContext.cpp#L1374-L1380)).
- With `enableValidation = false`, or on a machine without the SDK layers, the dispatch-table entry is most likely null, so the first `createBuffer` crashes. Every release build is affected.

*Fix:* always enable `VK_EXT_debug_utils` when it is available (it is cheap), and skip naming when it is not.

**B4 — Swapchain acquire and present results are ignored; the fence is reset before acquire.** *Fixed 2026-10-27:* acquire, present and recreation return `SwapchainStatus`. `beginFrame` waits on the fence, acquires, and resets the fence only after a successful acquire. It returns `false` to skip the frame on `OUT_OF_DATE`. `SUBOPTIMAL` from acquire and `SUBOPTIMAL`/`OUT_OF_DATE` from present set `swapchainOutOfDate`, and the swapchain is recreated at the start of the next frame. The original finding follows.
- `beginFrame` waits on and resets `inFlightFence`, then ignores the result of `acquireNextImage` ([engine.cpp:310-326](../src/engine.cpp#L310-L326)). `present` failures are only logged ([vulkanContext.cpp:1162-1168](../src/rhi/vulkan/vulkanContext.cpp#L1162-L1168)).
- On `VK_ERROR_OUT_OF_DATE_KHR` the image-acquired semaphore is never signaled, yet the frame still submits work that waits on it. That is invalid usage and can hang.
- `VK_SUBOPTIMAL_KHR` is treated as an error.

*Fix:* acquire first. On `OUT_OF_DATE`, recreate the swapchain and skip the frame. Reset the fence only after a successful acquire. Recreate on `SUBOPTIMAL` or `OUT_OF_DATE` from present.

**B5 — Resize and minimize can destroy the swapchain, and leak.** *Fixed 2026-10-27:* frames are skipped while the window is minimized or 0×0, and `pollEvents` blocks for events instead of spinning. `VulkanContext::recreateSwapchain` keeps the old swapchain when the surface extent is 0 (`OUT_OF_DATE`). When creation fails it destroys the retired swapchain and leaves none, so acquire reports `OUT_OF_DATE` and the engine retries. `get_images`/`get_image_views` are checked. `RenderContext::recreateSwapchain` drops the old swapchain image handles from `textureCache`. The depth textures were already destroyed on resize (`B1`). The original finding follows.
- Minimizing gives a 0×0 extent, so swapchain creation fails. `_recreate_swapchain` destroys the old swapchain anyway ([vulkanContext.cpp:1561-1568](../src/rhi/vulkan/vulkanContext.cpp#L1561-L1568)), `Engine::recreateSwapchain` ignores the failure ([engine.cpp:454](../src/engine.cpp#L454)), and the next acquire uses a destroyed swapchain.
- `get_images().value()` is called unchecked ([vulkanContext.cpp:862-863](../src/rhi/vulkan/vulkanContext.cpp#L862-L863)).
- Every resize leaks the previous depth textures ([engine.cpp:471-482](../src/engine.cpp#L471-L482)) and leaves stale swapchain `TextureVKHandle`s in `textureCache` ([renderContext.cpp:631-637](../src/rhi/renderContext.cpp#L631-L637)).

*Fix:* skip rendering while the extent is 0. Recreate only on success. Destroy the old depth textures and drop the old swapchain image handles.

### Medium severity

**B6 — Gaps in glTF geometry import.** *Fixed 2026-10-27:* `extractGLTFRaws` reads every attribute and index buffer through one accessor reader. It handles byte strides, accessors without a buffer view, sparse accessors, and normalized or integer components (`KHR_mesh_quantization`). It bounds-checks the accessor against its buffer view and the buffer view against its buffer. Non-indexed primitives get generated indices, which the smooth-normal path now shares. Triangle strips and fans become triangle lists. Points and lines are skipped with a warning. Primitives with a missing or invalid `POSITION`, invalid indices, or indices out of range are also skipped with a warning. An invalid optional attribute is ignored with a warning. `TANGENT` is imported and used for normal mapping (see `B19`). The original finding follows.
- Primitives without indices get an empty index list and are never drawn. Indices are filled only when `primitive.indices > -1` ([mesh.cpp:336-362](../src/assets/mesh.cpp#L336-L362)), and the smooth-normal path generates indices but does not keep them.
- `primitive.mode` is ignored, so lines, points and strips are drawn as triangle lists.
- Accessors are assumed to be tightly typed `float`. This breaks `KHR_mesh_quantization`, normalized integer UVs, and sparse accessors.
- `ByteStride()` returning -1 and `bufferView == -1` are not checked.
- The tangent attribute is ignored. That is acceptable for now because tangents are reconstructed in the shader.

**B7 — glTF materials and textures are duplicated for every primitive.** *Fixed 2026-10-27:* `loadMaterial` caches materials per model by material index, next to the per-model image cache from `B1`. Both live in a `GltfImportCache` that `processGLTF` creates for each import. A global texture cache is still open. The original finding follows.
- `extractGLTFRaws` calls `loadMaterial` for each primitive ([mesh.cpp:365-366](../src/assets/mesh.cpp#L365-L366)). Each call creates a new material and uploads every one of its textures again, taking new bindless slots ([material.cpp:154-193](../src/assets/material.cpp#L154-L193)).
- A model whose 50 primitives share one material consumes 50 materials and up to 250 bindless slots. The limits are 256 materials and 1024 slots, so medium-sized scenes fail.

*Fix:* add a per-model cache from material index to `MaterialHandle` and from image index to `TextureHandle` (and later a global texture cache).

**B8 — Missing materials crash or render black.** *Fixed 2026-10-27:* the engine creates `defaultMaterial` (the glTF default material) at startup, and `getMaterial` resolves invalid handles to it instead of throwing. Primitives without a material still prefer their entity's material. The original finding follows.
- `getMaterial` checks only `index >= size()` and throws ([material.cpp:229-236](../src/assets/material.cpp#L229-L236)). An OBJ entity without a material, in a program that never created a material, throws an uncaught `std::runtime_error` from `prepareScene` ([scene.cpp:282](../src/scene/scene.cpp#L282)).
- If other materials do exist, an invalid handle (index 0) returns the zeroed slot 0. A glTF primitive without a material then renders with base color 0 (black) instead of the glTF default material (white, metallic 1, roughness 1).

*Fix:* create a default material at startup and resolve invalid handles to it.

**B9 — `MaterialType::UNLIT` references a missing shader.** *Fixed 2026-10-31:* `gfx::Modeldata` now carries a `MaterialId` that `fragMain` branches on, so `UNLIT` is a path in the uber shader (`Unlit()` in `unlit.slang`). The legacy path uses `basic.slang` for it, which multiplies the first texture by the base color factor and handles a missing texture. glTF materials with `KHR_materials_unlit` import as `UNLIT` and upload only their base color texture. The original finding follows.
`createMaterial(UNLIT)` reads `./shaders/passes/unlit.slang` ([material.cpp:64](../src/assets/material.cpp#L64)), which does not exist. The file read fails and later compilation fails.

**B10 — The render-target path is broken.** *Fixed 2026-11-01:* `endPass` moves the color attachment to `SHADER_READ_ONLY_OPTIMAL` behind a barrier that waits for its color writes, depth attachments are no longer sampled or in the bindless array, and `beginPass` sizes the viewport from the render target. `B1` had already removed the aliasing. In `01_render_graph` the overlay reads the render target through this flow. `transitionImageLayout` now takes its source scope from the old usage instead of assuming render-target writes, so a target rendered again after being sampled waits for those reads, and usages it has no scope for are rejected. Validation does not guard the fix: on SDK 1.4.363, GPU-assisted validation with `VK_LAYER_GPUAV_IMAGE_LAYOUT=1` did not report the old layout either. The original finding follows.
- The attachments for each frame slot alias each other (B1).
- After rendering, the color attachment is never transitioned to `SHADER_READ_ONLY_OPTIMAL`: the barrier is commented out ([engine.cpp:416-423](../src/engine.cpp#L416-L423)). The bindless descriptor promises that layout ([vulkanContext.cpp:898](../src/rhi/vulkan/vulkanContext.cpp#L898)), so `01_render_graph` samples an image that is still in `COLOR_ATTACHMENT_OPTIMAL`. The default validation layers do not report this, because bindless (partially bound, update-after-bind) descriptors are only layout-checked with GPU-assisted validation.
- `beginPass` sets the viewport and scissor from the *window* size, not the render-target size ([engine.cpp:407-408](../src/engine.cpp#L407-L408)).
- The depth attachment of each RT also goes into the bindless array as a combined depth/stencil image.

**B18 — `ResourceDesc` is not zero-initialized.** *Fixed 2026-10-26:* the constructor now zeroes the whole union and `type` has a default initializer. The original finding follows.
- `ResourceDesc() {}` is a user-provided constructor ([resource.h:207](../include/rasm/render/resource.h#L207)), so `ResourceDesc desc{}` does **not** zero the union.
- Fields that a caller doesn't set (`buffer.offset`, `vertexIndexBuffer.*`, and texture fields when you use the buffer variant) are indeterminate. `operator==` and `ResourceDescHash` read them, so dedup (B1) also depends on stack garbage.

*Fix:* zero the storage in the constructor (`std::memset` the union, or give each variant default member initializers). Better, replace the union with `std::variant` or separate desc types (D5).

**B19 — The reconstructed tangent frame inverted the normal-map green channel.** *Fixed 2026-10-27:* `getNormalFromMap` now points the bitangent toward decreasing v (−dP/dv). glTF puts the UV origin at the image's top-left and normal maps store +Y as up in the image, so that is the direction of `cross(N, T) · w` for authored tangents. Before the fix, the derivative path aligned B with +dP/dv, so every normal map without `TANGENT` data was lit with Y flipped. This came to light when `TANGENT` import landed. On a test mesh with standard UVs, mirrored UVs and a mirroring node, and on the authored tangents of `coffee-room`, the two frames now agree (dot > 0.999). Before the fix they disagreed everywhere.

**B20 — Shader dedup keys pointed at freed SPIR-V.** *Fixed 2026-11-01:* `RenderContext::createShader` now keeps its own copy of the SPIR-V in `shaderSources`, and the cache key points at that copy. The bug came from `B1`, which made shader equality compare the code instead of the pointer. `shaderDescCache` still stored the caller's pointer into a temporary blob, so every later lookup that reached `operator==` ran `memcmp` on freed memory. This crashed intermittently with an access violation in any application that compiled the same shader twice, such as one that prepared a second scene. In a debug build, 5 of 30 runs crashed before the fix and 0 of 60 after it.

**B21 — Non-FIFO present modes crash `vkDestroyDevice` on NVIDIA.** *Worked around 2026-11-01; root cause unknown:* both swapchain builders now request `VK_PRESENT_MODE_FIFO_KHR`, so the frame rate is capped at the display's refresh rate. With vk-bootstrap's default of MAILBOX, closing a window that fills the screen raised an access violation inside `nvoglv64.dll` (driver 610.296, RTX 3060) during `vkDestroyDevice`. It depends on the window size, not on the example or on render targets: `00_hello_scene` crashes at 1920x1080 on a 1920x1080 display, and `01_render_graph` does not crash at 720x480. IMMEDIATE crashes too, and so does MAILBOX capped to about 60 fps, so it is not a frame-rate problem. Waiting 500 ms before teardown, destroying the swapchain before the semaphores, skipping the startup swapchain recreation and disabling validation do not help, and validation reports nothing. FIFO gave 0 crashes in 30 runs, against 40–100% per batch with the others. `vkcube` does not crash in any present mode at the same size, so the engine does something it does not, and that difference has not been found. The trigger is NVIDIA's overlay: with it turned off in the NVIDIA App, MAILBOX at 1920x1080 gave 0 crashes in 16 runs. `nvspcap64.dll` (NVIDIA's capture hook) stays loaded either way, but the overlay processes do not run. Keep FIFO while the overlay can be on, because it is on by default for many users, and revisit this before adding a way to turn vsync off.

### Low severity

**B11** — *Fixed 2026-10-31:* `submit` builds one wait stage per wait semaphore. The original finding follows. `VulkanContext::submit` passes one `waitStages` entry while `waitSemaphoreCount = waitSemaphores.size()` ([vulkanContext.cpp:1120-1138](../src/rhi/vulkan/vulkanContext.cpp#L1120-L1138)). Waiting on more than one semaphore reads past the array. The bug is latent today, because callers pass 0 or 1 semaphores.

**B12** — *Fixed 2026-10-31:* the SPIR-V options are now a static array. The original finding follows. `ShaderCompiler::getTargetDesc` allocates `new slang::CompilerOptionEntry[1]` and never frees it ([shaderCompiler.cpp:456-458](../src/renderer/shaderCompiler.cpp#L456-L458)).

**B13** — Handle indices are consumed before validation and are never reused.
- `loadMesh` takes a handle before parsing the file ([mesh.cpp:58](../src/assets/mesh.cpp#L58)), so failed loads burn slots.
- With `MAX_MESHES = 8` and indices starting at 1, only 7 meshes can be loaded in total.
- `MAX_LIGHTS = 8` and `MAX_CAMERAS = 8` are global, not per scene.

**B14** — *Fixed 2026-10-31:* the declaration and the `#if 0` definition are deleted. The original finding follows. `Engine::loadMaterials` is declared ([engine.h:56](../include/rasm/engine.h#L56)), but its definition is under `#if 0` ([material.cpp:81-103](../src/assets/material.cpp#L81-L103)), so calling it is a link error.

**B15** — *Fixed 2026-10-31:* `LightType::SPOT` is removed from the public enum. Real spot lights remain planned in the [development plan](development_plan.md). The original finding follows. `LightType::SPOT` is public ([light.h:15](../include/rasm/light.h#L15)), but the GPU light type has no spot variant. Spot lights are `static_cast` into value 2 and shaded as point lights.

**B16** — *Fixed 2026-10-31:* `prepareScene` returns an empty, uncached `PreparedScene` when there is no geometry, so entities added later still appear. `render2` presents the cleared frame when the prepared scene has no pipeline. *Extended 2026-11-01:* a failed preparation logs the failure, destroys what it created and retries on the next frame, and only a complete result is cached. A mesh without vertices or indices is skipped with a warning. The original finding follows. For an empty scene, `prepareScene` requests zero-sized buffers. Creation fails, the result is not cached, and every frame retries and logs errors. Then `render2` binds an invalid pipeline ([scene.cpp:199-233](../src/scene/scene.cpp#L199-L233)).

**B17** — *Fixed 2026-10-27:* `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` triggers recreation. `Window` has `pixelWidth`/`pixelHeight` (the swapchain's desired extent) next to the logical `width`/`height` used for mouse input. Depth textures, viewport, scissor and camera aspect come from the swapchain extent (`Swapchain::width`/`height`). The original finding follows. Resize uses `SDL_EVENT_WINDOW_RESIZED`, which reports logical size ([window.cpp:116-122](../src/platform/window.cpp#L116-L122)). The depth texture and viewport use `mainWindow` size, while the swapchain extent comes from the surface. On scaled or HiDPI displays they can disagree. Use `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` / `SDL_GetWindowSizeInPixels`, and size everything from the swapchain extent. *Likely.*

---

## 3. Design and architecture

**D1 — The backend abstraction does not scale.** Every `RenderContext` method is a `switch (backend)` with empty DX12/Metal branches (≈1,300 lines, mostly boilerplate). Replace it with an RHI interface:
- a small set of objects (`Device`, `Buffer`, `Texture`, `Sampler`, `Pipeline`, `CommandList`, `Queue`, `Swapchain`, `Fence`/timeline),
- either a virtual interface or a compile-time backend selection,
- a pool per object type with generational handles.

**D2 — The public API leaks its implementation.** *Mostly fixed 2026-11-01:* the header split is done (see §1.1). `createSurfaceVk` moved into the Vulkan backend, `convertToGfxMaterial` is a helper in `scene.cpp`, and the glTF import functions are private. Still open:
- `Engine` keeps a large public surface that mixes the high-level API with lower-level calls such as `updateUniforms`, `addBindlessTexture`, `createWindow` and `pollEvents`.
- `RenderGraph` and `RenderContext` are friends of `Engine`.

**D3 — `Engine` is a god object.** It owns the window, input, assets, scene registry, cameras, the frame loop and the renderer. Split it into subsystems that `Engine` composes: `Platform`, `AssetManager`, `Scene`, `Renderer`, `rhi::Device`. Then integrators can use only the pieces they need, for example the renderer with their own window and asset pipeline.

**D4 — There is no resource lifetime.**
- Nothing can be destroyed before shutdown: no `destroyMesh`, `destroyTexture`, `destroyEntity`, `removeEntityFromScene`.
- Handles are never invalidated (`generation` is always 1, [handle.cpp](../src/core/handle.cpp)), so use-after-free can't be detected.
- Streaming (map tiles, game levels) needs generational handles, deferred destruction tied to frame fences, and free-list reuse of registry slots and bindless slots.

**D5 — `ResourceDesc` is fragile.**
- It is a hand-maintained tagged union with a `std::string`, plus manual `==`/hash.
- Equality ignores fields that matter: pipeline equality ignores vertex layout, attachment formats and the descriptor layout, so two pipelines that differ only in color format alias each other.
- The hash XORs component hashes, so collisions are common (`a^b == b^a`, `x^x == 0`).
- Prefer distinct `BufferDesc`, `TextureDesc`, `PipelineDesc`, … structs with defaulted `operator==` and a proper hash combine.

**D6 — Scene preparation is all-or-nothing.** `prepareScene` is cached forever ([scene.cpp:129-132](../src/scene/scene.cpp#L129-L132)). Entities, meshes and materials added, removed or edited after the first frame are ignored; only transforms update. A GPU scene needs incremental updates:
- slot allocation inside persistent mega buffers,
- dirty tracking with per-frame upload of the changed ranges,
- material and instance tables separate from geometry.

**D7 — The engine owns the event loop and input.**
- `pollEvents` runs inside `beginFrame`. Escape quits. The camera controller is driven internally and the caller must pass a camera.
- Applications never see input events.
- You can't embed RASM in a host window (Qt, editor, map application) or run it headless.

Needed: a platform layer the application can replace, creation from a native surface or window handle, an offscreen/headless mode, and camera controllers as optional helpers driven by the application.

**D8 — Coordinate handedness is achieved by mirroring the world.**
- glTF geometry gets a `scale(1,-1,1)` root ([mesh.cpp:452](../src/assets/mesh.cpp#L452)), and OBJ vertices are negated per vertex ([mesh.cpp:127-131](../src/assets/mesh.cpp#L127-L131)).
- That mirrors the world: the determinant is negative and the triangle winding flips. Back-face culling is disabled to hide this ([vulkanContext.cpp:603-605](../src/rhi/vulkan/vulkanContext.cpp#L603-L605)).
- User-specified positions and directions live in a Y-down world.

Use a right-handed Y-up world (glTF's convention) and do the Vulkan flip with a negative-height viewport (core since 1.1) or in the projection. Then enable culling and honor glTF `doubleSided`.

**D9 — The render graph only sorts.**
- It does not track resource state, so barriers and layout transitions are left to the passes or missing (B10).
- It has no transient allocation or aliasing.
- Each pass starts its own rendering scope with `CLEAR` ([vulkanContext.cpp:1056](../src/rhi/vulkan/vulkanContext.cpp#L1056)), so two passes that target the swapchain overwrite each other.
- The camera is hardcoded to `scene.cameras[0]`, and `Engine::render` doesn't use the graph at all.

**D10 — There are two rendering paths.** *Fixed 2026-11-01:* the legacy path is gone, with `render2` renamed to `render`. `01_render_graph` draws through `Engine::drawScene` and `Engine::createPipeline`. The removal also took out what only the legacy path used: `basic.slang`, `MaterialType::BASIC` and `SHADER`, `Material::shader`, `Engine::createShader`, `uploadMesh`, the per-frame shader data buffer, the vertex-input reflection in `ShaderCompiler`, `VertexInputLayout`, `BufferUsage::VERTEX` and `VERTEXINDEX`, the vertex and index fields of the buffer desc, `bindVertexBuffer` and `SAMPLED_DEPTH_STENCIL_ATTACHMENT`. With `drawScene` public, each call has its own `SceneData` slot, fits the camera to the pass's aspect ratio, and refuses a render target with other formats than the scene's pipeline. A frame that never renders to the swapchain clears it in `endFrame`, because it is presented anyway. The original finding follows. `render`/`compileScene` (the legacy per-entity path) and `render2`/`prepareScene` (GPU-driven) coexist, together with `begin/endOffscreenFrame`, which is marked "TODO: delete". Pick the GPU-driven path, port `01_render_graph` to it, and delete the rest.

**D11 — Assets and shaders are located through the working directory** (§1.3).

**D12 — The shader code is Vulkan-only.**
- `spirv_asm` for `DrawIndex` ([core.slang:114-122](../shaders/lib/core.slang#L114-L122)) is SPIR-V specific.
- The data model relies on raw pointers (`SceneData*`, `VertexFull*`) through buffer device address.
- As far as I know, Slang pointer support is limited to SPIR-V, Metal and CUDA targets: HLSL/DXIL and WGSL have no raw pointers. Verify against the current Slang docs before you design around it.

Before you add D3D12 or WebGPU, put GPU data access behind a binding abstraction, either bindless `StructuredBuffer` indices or explicitly bound storage buffers, and route draw-ID access through a helper that each target implements.

**D13 — Queues and synchronization.** There is only one graphics queue: no transfer or async-compute queue. Timeline semaphores are supported by `createSemaphore` but unused. Uploads are synchronous on the main thread.

**D14 — Error handling is inconsistent.** Many `bool` results are ignored (`beginCommandBuffer`, `submit`, `fillBuffer`, `createTexture` in `loadTexture`). Some runtime errors are `assert`s.

**D15 — Hard-coded limits** are spread across headers (see [architecture.md §10](architecture.md#10-compile-time-limits)). Move them into `EngineConfig`, or grow the containers dynamically. Size GPU tables from the config.

**D16 — Profiling.** *Partly resolved:* the frame loop, `prepareScene`, uploads, submit and present now have Tracy zones and a frame mark, with the client in on-demand mode. Still open: Tracy is linked publicly, there are no GPU timestamps, nothing turns Tracy off (every build listens on TCP 8086 on all interfaces, which a shipped game must not do), and the VRAM check in `prepareScene` compares host-visible allocations against the device-local budget.

**D17 — No tests and no CI.** Pure-CPU pieces such as handle pools, desc hashing, glTF import, camera math and render-graph sorting are easy to unit-test. Rendering can be covered with headless golden-image tests once offscreen rendering exists.

---

## 4. Performance

**P1 — Every buffer is host-visible and mapped** (`HOST_ACCESS_SEQUENTIAL_WRITE | MAPPED`, [vulkanContext.cpp:205-208](../src/rhi/vulkan/vulkanContext.cpp#L205-L208)). On discrete GPUs without ReBAR, the mega vertex and index buffers end up in system RAM and every vertex fetch crosses PCIe. Static data should go to device-local memory through staging (or `ALLOW_TRANSFER_INSTEAD`). Keep host-visible memory for per-frame data only.

**P2 — Texture path.**
- Images are created with `HOST_ACCESS_*` allocation flags ([vulkanContext.cpp:347-351](../src/rhi/vulkan/vulkanContext.cpp#L347-L351)), which can push them into host-visible memory.
- Each image gets a dedicated allocation, and mips are generated on the CPU on the main thread.
- Each texture creates its own `VkSampler` and calls `vkGetPhysicalDeviceProperties` ([vulkanContext.cpp:381-409](../src/rhi/vulkan/vulkanContext.cpp#L381-L409)). Drivers often cap samplers at about 4000, so use a handful of shared samplers.
- Host image copy also requires the `hostImageCopy` feature, which narrows hardware support; keep a staging-buffer fallback.
- Longer term, support GPU-compressed textures (KTX2 + BCn/ASTC), which matters for map tiles and game content.

**P3 — No instancing.** Each entity copies its mesh into the mega buffer. `00_hello_scene` uploads the bunny three times ([scene.cpp:143-165](../src/scene/scene.cpp#L143-L165)). Separate *geometry* (uploaded once per mesh) from *instances* (transform + material + mesh-range index).

**P4 — Per-call overhead in `RenderContext`.** Every call hashes a handle and **copies** the backend struct, including a `ResourceDesc` with a `std::string` (e.g. `fillBuffer`, [renderContext.cpp:665](../src/rhi/renderContext.cpp#L665); `getBufferDeviceAddress`, [:740](../src/rhi/renderContext.cpp#L740)). Use index-based pools, return references, and cache device addresses.

**P5 — Large by-value copies in the scene code.**
- `prepareScene` no longer copies meshes: `processMesh` is gone, and each `SceneDraw` points into the registry. It returns a const reference to the cached entry, so `drawScene` copies nothing.
- `getMeshByteSize` copies a whole `Mesh`.
- `RenderGraph::execute` copies `Scene`, which holds 1,040 handles ([renderGraph.cpp:143](../src/renderer/renderGraph.cpp#L143)).

**P6 — CPU memory retention.** *Partly fixed 2026-11-01:* `GLTFRaw` no longer keeps a copy of the `tinygltf::Model`, so the decoded glTF images are freed after import. CPU vertex and index arrays and the `stbi` pixel data for every `loadTexture` still stay resident for the engine's lifetime.

**P7 — Per-frame CPU work grows with draw count.** `updateUniforms` recomputes and uploads every model and normal matrix every frame, with no dirty tracking. That is fine now, but it becomes the bottleneck at 10⁴–10⁵ instances. Plan for dirty ranges and, later, GPU-side transform computation.

**P8 — Shader and pipeline creation.** Shaders are compiled from source on every launch. There is no `VkPipelineCache` and no SPIR-V cache. A pipeline layout is created per pipeline even though all pipelines share the same set layout and push-constant range.

**P9 — Shader detail.** `fragMain` copies all 8 lights and the material into a `PBRInfo` local ([rasm.slang:90-91](../shaders/passes/rasm.slang#L90-L91)). Read through the pointer, or pass indices, to keep register pressure down.

---

## 5. Code health

- **C1 — Naming inconsistencies.** `registery`, `metallicRoughtness`, `compiledShader`, the snake_case `VulkanContext` members, PascalCase GPU enums, and `RenderGraph::CreateTexture` beside `PassBuilder::createTexture`. See the [coding style](coding_style.md#2-naming).
- **C2 — Formatting.** `scene.cpp` uses 2-space indentation while everything else uses 4, and there is no `.clang-format` in the repo.
- **C3 — Dead or placeholder code.**
  - `setUniform` (a no-op).
  - The empty mesh loop in `~Engine`, the glTF branch of `getMeshByteSize`.
  - `VertexFormat::PosOnly`, `MAX_DESCRIPTOR_TYPES`, `compile-shaders.bat`.
- **C4 — Duplication.**
  - `readFile` and `loadFileToString`.
  - The model-matrix composition appears twice, in `prepareScene` and `updateUniforms`; `Transform` should have a `toMatrix()` instead.
  - The glTF index-reading loop appears twice.
- **C5 — Copies instead of references.** `auto mesh = registery.meshes[...]`, `auto entity = ...`, `auto _scene = ...`.
- **C6 — An unused check.** *Resolved:* `validateRasmStructs` was never called because its sizes did not match. It asked `ProgramLayout::getTypeLayout` for the layout, and no `LayoutRules` value selects the layout of data behind a pointer, so it reported std140/std430 padding (`Modeldata` 240 bytes instead of 188). It now walks `pc` -> `scene` -> the pointer fields, which agrees with the `Offset` decorations in the SPIR-V, and asserts sizes and field offsets after linking any shader that uses `SceneData`.
- **C7 — A misplaced define.** *Resolved in the folder-layout change:* the ineffective `VMA_VULKAN_VERSION` define (it came after `vk_mem_alloc.h` was already included) was dropped. VMA's default, derived from the Vulkan headers, is the same 1004000 for every translation unit. If you ever need to pin it, set it in CMake.
- **C8 — Header-level objects.** `const static std::unordered_set<std::string>` lives in `types.h`, which makes one copy per translation unit and runs static initializers. Make it `inline constexpr` arrays or move it into the `.cpp`.
- **C9 — A fragile material layout.** `Material::params` is a flat `float[]` indexed by hand-maintained `PbrParam` offsets, and `name` is a `const char*`. Use a typed `PbrMaterialParams` struct instead.
- **C10 — Single-header implementations are spread out.** *Resolved in the folder-layout change:* all `*_IMPLEMENTATION` defines now live in [src/thirdparty_impl.cpp](../src/thirdparty_impl.cpp).

---

## 6. What is already good

These are worth keeping as the engine evolves:
- **Modern Vulkan baseline:** dynamic rendering, synchronization2, per-image render-finished semaphores (the correct modern pattern), VMA with budget tracking, vk-bootstrap.
- **A GPU-driven core:** one indirect draw, vertex pulling, bindless textures, and BDA through a single 8-byte push constant.
- **A careful CPU/GPU layout contract:** scalar layout with `static_assert`s and comments explaining the failure mode.
- **Correct color handling:** sRGB versus linear texture formats, sRGB-aware mip generation, and linear shading with sRGB output.
- **A solid PBR core:** a Filament-style BRDF, UV-derivative tangent frames that handle mirrored UVs, and per-texture UV sets.
- **A thoughtful camera:** frame-rate-independent easing, and a trackball that eases along the orbit sphere.
- **A clean layering intent:** handles everywhere and a single creation descriptor. The ideas are right; the implementation needs to catch up (D1, D5).
