# RASM Development Plan

This plan turns RASM from a working GPU-driven prototype into an engine that can be embedded in indie games, visualization tools and map engines. Item IDs such as `B1` and `D4` refer to [code_review.md](code_review.md). The current design is described in [architecture.md](architecture.md).

## Guiding principles

1. **Examples drive features.** Every phase ends with at least one new or updated example that exercises it. (This keeps item 2 of the original plan.)
2. **GPU-driven by default.** The CPU uploads changes and records a handful of commands, and the GPU decides what to draw. CPU cost must not grow with the number of instances.
3. **Validation-clean is part of "done".** Every example runs without validation messages, in both Debug and Release.
4. **Portable by construction.** New shader and RHI code is written against the capability tiers below, so the Metal, D3D12 and WebGPU backends don't require rewrites.
5. **Embeddable.** The engine must work without owning the window, the event loop, the asset pipeline or the process working directory.

## Capability tiers (target for the RHI)

| Feature | Tier A: Vulkan 1.3+, D3D12 SM6.6, Metal 3 | Tier B: WebGPU (core) |
|---|---|---|
| Resource access | Bindless descriptor heaps or arrays | Bind groups per pass and per material, texture arrays or atlases |
| GPU data | Bindless buffers (BDA on Vulkan/Metal, descriptor-heap structured buffers on D3D12) | Storage buffers bound per pass |
| Indirect draws | Multi-draw indirect + draw count | `drawIndexedIndirect` per draw (or the multi-draw extension where present) |
| Per-draw constants | Push constants / root constants | Small uniform buffer + dynamic offset |
| Compute culling | Yes | Yes |
| Mesh shaders | Optional (Vulkan/D3D12/Metal 3) | No |

The engine's **data model** (GPU scene tables, instance and material indices, draw lists) is the same across tiers. Only the *binding* and *submission* layer differs. This is why `D12` must be solved before a second backend is started.

---

## Phase 0: Stabilize what exists

Goal: fix correctness bugs before building anything on top.

- [x] Remove desc-based dedup for buffers and textures. Keep it only for immutable objects keyed on their full state (`B1`, done 2026-10-26).
- [x] Zero-initialize `ResourceDesc` (`B18`, done 2026-10-26).
- [x] Set GLM and VMA configuration macros in CMake. Fix the depth range (`B2`, `C7`, done 2026-10-26).
- [x] Always enable `VK_EXT_debug_utils` when it is available, and guard the naming calls (`B3`).
- [x] Make acquire/present robust: handle out-of-date and suboptimal swapchains, reset the fence after a successful acquire, skip frames while minimized, and drop leaked depth textures and stale swapchain handles (`B4`, `B5`, `B17`, done 2026-10-27).
- [x] Cache glTF materials and textures per model. Add a default material (`B7`, `B8`, done 2026-10-27).
- [x] glTF geometry: generate indices for non-indexed primitives, skip non-triangle modes with a warning, and validate strides and buffer views (`B6`, done 2026-10-27).
- [ ] Delete or fix `UNLIT`, the `loadMaterials` declaration and the spot-light enum value (`B9`, `B14`, `B15`). Make empty scenes work (`B16`). Fix `submit` wait stages (`B11`) and the Slang options leak (`B12`).
- [x] Port `01_render_graph` to the GPU-driven path with a fixed render-target flow (`D10`, `B10`, done 2026-10-27).
- [ ] Remove the legacy path (`render`/`compileScene`/offscreen frame), which no example uses any more (`D10`).
- [ ] Call `validateRasmStructs` in debug builds (`C6`). Add Tracy frame marks and zones for frame, prepare, upload and submit (`D16`).

**Exit criteria:** all examples run validation-clean in Debug and Release (with `enableValidation = false`), survive resize and minimize, and a model with 100+ primitives sharing materials loads within the limits.

## Phase 1: Foundations

Goal: the structural changes that every later feature depends on. They get much harder to make later.

### 1.1 Project structure and public API
- [ ] Split public and private headers. `include/rasm/` must contain no Vulkan, VMA, vk-bootstrap, Slang, tinygltf or SDL headers. Then make `src/` a PRIVATE include directory (`D2`, review §1). *The files are already in their final places; what remains is removing the includes.*
- [x] Restructure the source tree into `core/`, `platform/`, `assets/`, `scene/`, `renderer/`, `rhi/vulkan/`, and shaders into `lib/` and `passes/` (review §1, done 2026-10-26).
- [x] CMake: `src/` and `examples/` subdirectories, the `rasm` target with alias `rasm::rasm`, the `RASM_BUILD_EXAMPLES` option, and the `rasm_add_example()` helper.
- [x] Put the `*_IMPLEMENTATION` defines in one translation unit, `src/thirdparty_impl.cpp` (`C10`).
- [ ] Install and export targets (`find_package(rasm)`). Add `RASM_BUILD_TESTS` together with the first tests.
- [ ] Locate assets and shaders through configuration: `EngineConfig::assetRoot`, a `RASM_SHADER_DIR` default, and shaders copied next to the binaries (`D11`).
- [ ] Commit a `.clang-format` and normalize formatting in a dedicated commit (`C1`, `C2`).

### 1.2 RHI
- [ ] Define a backend-neutral device interface: device, buffer, texture, sampler, pipeline, command list, queue, swapchain, fence/timeline. Move all `Vk*` code behind it (`D1`).
- [ ] Use typed descriptors (`BufferDesc`, `TextureDesc`, `PipelineDesc`, …) with defaulted equality and a real hash combine (`D5`).
- [ ] Store objects in generational pools. Make `destroy*` APIs available for every object, with destruction deferred until the GPU has finished the frames that use them (`D4`, `P4`).
- [ ] Use a single shared pipeline layout (the bindless set + push-constant range), a `VkPipelineCache` persisted to disk, and an on-disk SPIR-V cache keyed by source hash (`P8`).

### 1.3 Memory and uploads
- [ ] Keep static data in device-local memory. Add an upload manager with a staging ring buffer, batched copies, an optional dedicated transfer queue, and timeline-semaphore completion (`P1`, `D13`).
- [ ] Texture upload: use host image copy when it is supported, otherwise staging. Generate mips on the GPU or with a worker thread. Use a small shared sampler set (`P2`).
- [ ] Load assets asynchronously on worker threads, and release CPU-side asset data after upload (`P6`).
- [ ] Custom allocators: per-frame linear allocators for transient CPU data, pool allocators for registries. (Original item 4; scope these to measured hot spots.)

### 1.4 Conventions
- [ ] Use a right-handed Y-up world. Flip Y with a negative-height viewport, stop mirroring geometry, enable back-face culling, and honor glTF `doubleSided` (`D8`).
- [ ] Use reversed-Z with an infinite far plane by default. The depth format becomes `D32_SFLOAT`, without stencil unless it is requested.

### 1.5 Quality
- [ ] Add a unit-test target covering handle pools, descriptor hashing, render-graph sorting, glTF import and camera math (`D17`).
- [ ] Set up CI: build Windows and macOS presets on every PR, and run unit tests. Add image tests once headless rendering exists (phase 4).

**Exit criteria:** examples build against `rasm::rasm` from an install tree, using only public headers. Core code contains no `Vk*` identifiers. Objects can be created and destroyed at runtime without leaks under validation.

## Phase 2: The GPU-driven scene

Goal: make RASM's core promise hold at scale.

- [ ] **GPU scene tables** (`D6`, `P3`, `P7`):
  - a persistent geometry pool (a sub-allocator inside mega vertex and index buffers, uploaded once per mesh),
  - an instance table (transform, mesh index, material index, bounds) and a material table,
  - per-frame upload of only the dirty ranges.
- [ ] Separate *mesh* from *instance*, so entities that share a mesh share its geometry.
- [ ] Compute pipelines and compute passes in the RHI.
- [ ] **GPU frustum culling**: a compute pass that writes a compacted draw list, followed by `vkCmdDrawIndexedIndirectCount`.
- [ ] **Occlusion culling**: a two-pass hierarchical-Z scheme (draw what was visible last frame, build Hi-Z, cull and draw the rest).
- [ ] LOD selection on the GPU, from per-mesh LOD ranges and screen-space error.
- [ ] Optional: meshlets with cluster culling, and a mesh-shader path where available (a Tier-A extra; keep the vertex-pulling path as the baseline).
- [ ] Stats overlay: frame time, GPU time per pass, triangles, instances drawn/culled, memory. (Original item 8.)

**Exit criteria:** a "city" example with ≥ 100k instances renders with CPU frame time independent of instance count. Culling statistics are visible on screen.

## Phase 3: A real render graph

Goal: passes describe what they use, and the graph does the synchronization and memory work (`D9`).

- [ ] Track resource state and generate barriers and layout transitions automatically.
- [ ] Imported resources (swapchain, history buffers) and transient resources with lifetime-based memory aliasing.
- [ ] Per-pass attachment load and store ops (no implicit clears), multiple color attachments, and resolve.
- [ ] Graph-driven frame: the forward/culling pipeline from phase 2 becomes a set of graph passes, and `render2` disappears behind `Renderer::render(view)`.
- [ ] Async compute: run culling and Hi-Z on a compute queue where available, with cross-queue synchronization generated by the graph.
- [ ] Graph debugging: dump the pass graph and resource lifetimes, and give every pass a GPU timestamp.

**Exit criteria:** a multi-pass example (shadow + forward + post) contains no hand-written barriers.

## Phase 4: Rendering features

Goal: production look and completeness for glTF content. The order is roughly by value to the target users.

- [ ] An HDR color target plus a separate tone-map pass (ACES or AgX) with exposure. Remove Reinhard from the material shader.
- [ ] **Shadows**: cascaded shadow maps for directional lights, and spot and point light shadows. Add real spot lights (`B15`).
- [ ] **Image-based lighting**: an environment map, prefiltered specular, irradiance and a BRDF LUT.
- [ ] **glTF completeness**:
  - alpha modes (`MASK` with alpha-to-coverage, and `BLEND` as a sorted transparent pass),
  - vertex colors (tangents done 2026-10-27),
  - `KHR_texture_transform`, `KHR_materials_emissive_strength`, `KHR_mesh_quantization`,
  - KTX2/Basis textures.
- [ ] **Many lights**: clustered forward lighting. Remove the fixed `MAX_LIGHTS = 8`.
- [ ] Anti-aliasing (MSAA option and TAA), bloom.
- [ ] Skinning and animation, morph targets.
- [ ] Dear ImGui integration (behind an option) with debug views: normals, UVs, overdraw, culling, buffers. (Original item 9; replace the stale submodule entry.)
- [ ] Headless/offscreen rendering with readback, which enables golden-image tests in CI.

**Exit criteria:** the Khronos glTF sample models (DamagedHelmet, Sponza, AlphaBlendModeTest, …) render correctly under IBL with shadows, and image tests guard regressions.

## Phase 5: Integration for the target audiences

### Embedding (all audiences)
- [ ] A platform layer the application can replace: create the engine from a native window or surface handle, or headless. The application owns the event loop and feeds input (`D7`).
- [ ] Optional camera controller helpers that the application drives instead of the engine.
- [ ] Multiple views and windows, viewports and render-to-texture as first-class features.
- [ ] Split subsystems so integrators can use only the renderer: `Platform`, `AssetManager`, `Scene`, `Renderer`, `rhi::Device` (`D3`).
- [ ] **A C API** (`rasm_c.h`), a stable C ABI facade over the C++ core, for bindings in other languages. (Original item 5. I recommend this rather than rewriting the engine in C without the STL: it gives the same portability and keeps C++ productivity inside the engine.)

### Map engines
- [ ] Large-world precision: double-precision transforms on the CPU, camera-relative rendering (subtract the camera origin on the CPU, float offsets on the GPU), and reversed-Z (from phase 1).
- [ ] Tile streaming: asynchronous load and destroy of meshes and textures under a memory budget (builds on `D4`, phase 1.3).
- [ ] Vector rendering: anti-aliased wide lines, polygons, points and SDF text and labels. Terrain from heightmap tiles.
- [ ] Picking: an ID buffer pass with readback, giving the entity and the world position.

### Visualization tools
- [ ] Point clouds and instanced glyphs, lines and grids, and orthographic views (already partly supported).
- [ ] Order-independent transparency (weighted blended) for dense data.
- [ ] Screenshot and offscreen export at arbitrary resolution.

### Indie games
- [ ] Scene serialization and an asset cooking step (pre-processed geometry, KTX2 textures, compiled shaders).
- [ ] Animation playback API, particles (GPU compute), decals.

## Phase 6: Additional backends

Prerequisites: phase 1.2 (RHI), the `D12` shader binding abstraction, and image tests from phase 4 that compare every backend against Vulkan.

1. **Metal** (native, Metal 3: argument buffers, GPU addresses, ICBs). macOS is already a target, and this replaces the MoltenVK dependency and its feature gaps.
2. **D3D12**: SM 6.6 `ResourceDescriptorHeap` bindless, `ExecuteIndirect`, enhanced barriers. Slang → DXIL.
3. **WebGPU** (Dawn or wgpu-native, plus Emscripten for the browser): Tier B. Bind-group materials, per-draw indirect, uniform-buffer constants, Slang → WGSL. Expect the most backend-specific work here. Every other backend gets bindless and BDA, and WebGPU does not.

For each backend: device and swapchain bring-up first, then the unit and image tests, then the examples in order.

---

## Original plan, mapped

| Original item | Where it lives now |
|---|---|
| 1. Simplify the low-level API | Phase 1.1–1.2 (public/private split, RHI, typed descriptors), phase 3 (graph-driven passes) |
| 2. Example-driven development | Guiding principle 1, and every phase's exit criteria |
| 3. GPU resource management (MDI, bindless, mega buffers, uber shader) | Done for the basics. Continued in phase 2 (GPU scene tables, instancing) and phase 4 (material system on the uber shader) |
| 4. Custom CPU allocators | Phase 1.3, scoped to measured hot spots |
| 5. Move to C / no STL | Phase 5: a C ABI facade instead of a rewrite (see the rationale there) |
| 6. More examples and custom passes | Every phase. Phase 3 makes custom passes practical |
| 7. GPU frustum culling | Phase 2 |
| 8. Engine statistics | Phase 0 (Tracy), phase 2 (stats overlay) |
| 9. GUI | Phase 4 (Dear ImGui integration) |
| 10. Open-source release | After phase 1: install/export, CI, docs, examples. Alpha after phase 2 |
| 11. Metal, D3D12, WebGPU backends | Phase 6, with groundwork in phase 1.2 and `D12` |
