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
- [x] Implement `UNLIT` through a per-model material id in the uber shader. Delete the `loadMaterials` declaration and the spot-light enum value (`B9`, `B14`, `B15`). Make empty scenes work (`B16`). Fix `submit` wait stages (`B11`) and the Slang options leak (`B12`) (done 2026-10-31).
- [x] Remove the legacy path (`render`/`compileScene`/offscreen frame) and port `01_render_graph` to the GPU-driven path with a fixed render-target flow (`D10`, `B10`, done 2026-11-01).
- [x] Call `validateRasmStructs` in debug builds, checking the shader layouts behind the `pc` pointers (`C6`, done 2026-11-01).
- [x] Add Tracy frame marks and zones for frame, prepare, upload and submit, with the client in on-demand mode (`D16`, done 2026-11-01).

**Exit criteria:** all examples run validation-clean in Debug and Release (with `enableValidation = false`), survive resize and minimize, and a model with 100+ primitives sharing materials loads within the limits.

## Phase 1: Foundations

Goal: the structural changes that every later feature depends on. They get much harder to make later.

### 1.1 Project structure and public API
- [x] Split public and private headers. `include/rasm/` contains no Vulkan, VMA, vk-bootstrap, Slang, tinygltf, SDL or spdlog headers, and `src/` and the backend libraries are PRIVATE (`D2`, review §1, done 2026-11-01).
- [x] Restructure the source tree into `core/`, `platform/`, `assets/`, `scene/`, `renderer/`, `rhi/vulkan/`, and shaders into `lib/` and `passes/` (review §1, done 2026-10-26).
- [x] CMake: `src/` and `examples/` subdirectories, the `rasm` target with alias `rasm::rasm`, the `RASM_BUILD_EXAMPLES` option, and the `rasm_add_example()` helper.
- [x] Put the `*_IMPLEMENTATION` defines in one translation unit, `src/thirdparty_impl.cpp` (`C10`).
- [x] Install and export targets (`find_package(rasm)`). Add `RASM_BUILD_TESTS` together with the first tests (done 2026-11-01).
- [x] Locate assets and shaders through configuration: `EngineConfig::assetRoot`, `EngineConfig::shaderDir` with a `RASM_SHADER_DIR` default beside the executable (`D11`, done 2026-11-01).
- [x] Commit a `.clang-format` and normalize formatting in a dedicated commit (`C2`, done 2026-11-01).
- [x] Apply the naming fixes in their own commit: `registery`, `metallicRoughtness`, `compiledShader`, the snake_case `VulkanContext` members, the PascalCase GPU enums and `RenderGraph::CreateTexture` (`C1`, done 2026-11-01).

### 1.2 RHI
- [x] Define a backend-neutral device interface: device, buffer, texture, sampler, pipeline, command buffer, queue, swapchain, fence/timeline. Move all `Vk*` code behind it (`D1`, done 2026-11-02). The platform's Vulkan surface hook remains until `D7`.
- [x] Use typed descriptors (`BufferDesc`, `TextureDesc`, `PipelineDesc`, …) with defaulted equality and a real hash combine (`D5`, done 2026-11-02).
- [ ] Store objects in generational pools. Make `destroy*` APIs available for every object, with destruction deferred until the GPU has finished the frames that use them (`D4`, `P4`).
- [ ] Use a single shared pipeline layout (the bindless set + push-constant range), a `VkPipelineCache` persisted to disk, and an on-disk SPIR-V cache keyed by source hash (`P8`).
- [ ] Load Vulkan functions at run time with volk instead of linking `vulkan-1.dll`: a clear error when the loader is missing or older than Vulkan 1.4, device calls without the loader's trampoline, and unit tests that start without a Vulkan loader (`D24`).

### 1.3 Memory and uploads
- [x] Keep static data in device-local memory. Add an upload manager with a staging ring buffer, batched copies and timeline-semaphore completion (`P1`, done 2026-11-04).
- [ ] Optional dedicated transfer queue for uploads, with semaphore waits in `SubmitDesc` (done 2026-11-15) and queue-family ownership transfers. It pays off once loading is asynchronous (`D13`, `P6`).
- [ ] Texture upload: use host image copy when it is supported, otherwise staging. Generate mips on the GPU or with a worker thread. Use a small shared sampler set (done 2026-11-04) (`P2`).
- [x] Load assets asynchronously on worker threads, and release CPU-side asset data after upload (`P6`, done 2026-11-04).
- [ ] Custom allocators: per-frame linear allocators for transient CPU data, pool allocators for registries. (Original item 4; scope these to measured hot spots.)

### 1.4 Conventions
- [x] Use a right-handed Y-up world. Flip Y with a negative-height viewport, stop mirroring geometry, enable back-face culling, and honor glTF `doubleSided` (`D8`, done 2026-11-04).
- [x] Use reversed-Z with an infinite far plane by default. The depth format becomes `D32_SFLOAT`, without stencil unless it is requested (done 2026-11-04).

### 1.5 Quality
- [ ] Extend `rasm_tests` to glTF import (done 2026-11-04) and to the handle pools once they exist (`D4`). Handles, descriptor equality and hashing, render-graph sorting, transforms, camera math and glTF import are covered (`D17`).
- [ ] Set up CI: build Windows and macOS presets on every PR, and run unit tests (`D17`, done 2026-11-04). Add image tests once headless rendering exists (phase 4).

**Exit criteria:** examples build against `rasm::rasm` from an install tree, using only public headers. Core code contains no `Vk*` identifiers. Objects can be created and destroyed at runtime without leaks under validation.

## Phase 2: The GPU-driven scene

Goal: make RASM's core promise hold at scale.

- [ ] **GPU scene tables** (`D6`, `P3`, `P7`):
  - a persistent geometry pool (a sub-allocator inside mega vertex and index buffers, uploaded once per mesh). The pool and a geometry table with per-sub-mesh bounds are done (2026-11-07). They are append-only: freeing and reusing ranges waits for the destroy APIs of `D4`.
  - an instance table (transform, mesh index, material index, bounds) and a material table (done 2026-11-07; the bounds are in the geometry table),
  - per-frame upload of only the dirty ranges (done 2026-11-07, example `03_instances`).
- [x] Separate *mesh* from *instance*, so entities that share a mesh share its geometry (`P3`, done 2026-11-07).
- [x] Compute pipelines and compute passes in the RHI (done 2026-11-07, example `04_compute`). Compute writes buffers only, and samples textures through the bindless set (done 2026-11-07). Storage images (done 2026-11-21, for the environment bake, outside the render graph); render graph compute passes that write textures (done 2026-11-21: storage render targets, `RenderTargetDesc::storage`, and storage textures, moved to `STORAGE` and back to `SAMPLED` by the graph).
- [x] **GPU frustum culling**: a compute pass that writes a compacted draw list, followed by `vkCmdDrawIndexedIndirectCount` (done 2026-11-07).
- [x] **Occlusion culling**: a two-pass hierarchical-Z scheme (draw what was visible last frame, build Hi-Z, cull and draw the rest) (done 2026-11-07, `EngineConfig::occlusionCulling`). The Hi-Z is a buffer pyramid built by compute from the sampled depth, so it needs no storage images.
- [x] LOD selection on the GPU, from per-mesh LOD ranges and screen-space error (done 2026-11-07, `EngineConfig::meshLods` and `lodErrorPixels`). LODs are built with meshoptimizer as meshes load and share the mesh's vertices. Reordering the vertices for coarse-LOD fetch locality, and streaming LODs in and out of the pool (after `D4`), are still open.
- [ ] Optional: meshlets with cluster culling, and a mesh-shader path where available (a Tier-A extra; keep the vertex-pulling path as the baseline).
- [x] Stats overlay: frame time, GPU time per pass, triangles, instances drawn/culled, memory. (Original item 8; done 2026-11-07, `EngineConfig::showStats` or F1, on in example `03_instances`.)

**Exit criteria:** a "city" example with ≥ 100k instances renders with CPU frame time independent of instance count. Culling statistics are visible on screen. (Example `05_city`, done 2026-11-07: 145.6k instances, about 1 ms of CPU per frame in Release, GPU-bound.)

## Phase 3: A real render graph

Goal: passes describe what they use, and the graph does the synchronization and memory work (`D9`).

- [x] Track resource state and generate barriers and layout transitions automatically (done 2026-11-07). Textures keep their state in `RenderContext`, and the graph orders each declared buffer by its last write and the reads since, across frames. A pass must declare every buffer it uses: an undeclared one gets no barrier.
- [x] Imported resources (swapchain, history buffers) and transient resources with lifetime-based memory aliasing (`D9`, done 2026-11-13). `PassBuilder::writeSwapchain`, history render targets read with `readPrevious`, transient render targets placed in shared heaps by their lifetimes (`rhi::packAliased`), and targets sized from the window (`RenderTargetDesc::swapchainScale`).
- [x] Per-pass attachment load and store ops (no implicit clears) and multiple color attachments (done 2026-11-13). `AttachmentOps`, set by the graph so that later passes on a target load what earlier ones drew; up to `MAX_COLOR_ATTACHMENTS` colors per render target, with pipelines from `Engine::createPipeline(name, source, renderTarget)`. Resolve moved to the MSAA item in phase 4, since it only serves multisampled targets.
- [x] Graph-driven frame: the forward/culling pipeline from phase 2 becomes a set of graph passes, and `Engine::render` moves behind `Renderer::render(view)` (`D9`, `D18`, done 2026-11-13). `Engine::addScenePasses` adds a scene view's culling, draw, Hi-Z, late culling and late draw passes to any graph; the barriers inside the Hi-Z build and between the count and write dispatches of culling stay inside their passes.
- [x] Async compute: run culling and Hi-Z on a compute queue where available, with cross-queue synchronization generated by the graph (`D13`, done 2026-11-15). `RenderGraph::addAsyncComputePass`, `EngineConfig::asyncCompute`. Off by default: on the scene passes alone the handoffs between the queues cost a little more than the overlap saves (`P11`).
- [x] Graph debugging: dump the pass graph and resource lifetimes, and give every pass a GPU timestamp (done 2026-11-07, shown in the stats overlay). `RenderGraph::dump` lists the passes in the order they run, with their reads, writes, attachment ops and the passes that wait for them, then a lifetime table with a column per pass; F3 logs it for the graph `Engine::render` runs (done 2026-11-15).

**Exit criteria:** a multi-pass example (shadow + forward + post) contains no hand-written barriers. (Example `06_shadows`, done 2026-11-15: a depth-only shadow map drawn by `addScenePasses` from a light camera, the forward view that samples it through `SceneShadow`, and a post pass; validation-clean with synchronization validation.)

## Phase 4: Rendering features

Goal: production look and completeness for glTF content. The order is roughly by value to the target users.

- [x] An HDR color target plus a separate tone-map pass (ACES or AgX) with exposure. Remove Reinhard from the material shader. (Done 2026-11-20: the scene passes draw into a `SCENE_COLOR_FORMAT` (`R16G16B16A16_SFLOAT`) target, and `Engine::addToneMapPass` maps it to the swapchain with AgX and `Camera::exposure`.)
- [x] **Shadows**: cascaded shadow maps for directional lights, and spot and point light shadows. Add real spot lights (`B15`). Done 2026-11-20: cascades for a directional light (`Engine::addShadowPasses`, `ShadowDesc`, example `06_shadows`), up to `MAX_SHADOW_CASCADES` (4) depth-only maps fitted to slices of the camera's view, which replace the single map from a light camera the application placed (done 2026-11-15). `LightType::SPOT` with glTF cone angles; a spot light's shadow is one perspective map, and a point light's is a cube texture (`RenderTargetDesc::cube`) drawn by one scene view per face. A view reads up to `MAX_VIEW_SHADOWS` (4) shadowed lights (`addScenePasses` takes a list), and `MAX_SCENE_VIEWS` is 16 (example `07_lights`).
- [x] **Image-based lighting**: an environment map, prefiltered specular, irradiance and a BRDF LUT. Done 2026-11-21: `Engine::loadEnvironment` takes an equirectangular `.hdr`, and the first frame after it decodes bakes, on the GPU before the graph, a cube with mips, a GGX specular cube of 6 roughness mips (filtered importance sampling) and a 32² irradiance cube; a 128² BRDF LUT is baked once per engine. `setSceneEnvironment` (`SceneEnvironment{environment, intensity, rotation, sky}`) lights a scene with the split sum in `shadePBR` and draws it as the sky. The RHI got storage images for it (`TextureUsage::SAMPLED_STORAGE`, the `STORAGE` state, binding 1 of the bindless set). Example `08_ibl`. Open: destroying an environment (`D22`, after `D4`), shadowed environment light and specular occlusion (`D23`).
- [x] **glTF completeness** (done 2026-11-21):
  - alpha modes (`MASK` as a cut-out, and `BLEND` as a sorted transparent pass; done 2026-11-21: `Material::alphaMode` and `alphaCutoff`, a pipeline per alpha mode, the `Transparent` pass after the sky with the blend run sorted back to front by a GPU bitonic sort, and hashed alpha for `BLEND` in shadow maps. Alpha-to-coverage moved to the MSAA item; cut-outs still thin out in the far mips, `D25`),
  - vertex colors (tangents done 2026-10-27; colors done 2026-11-21: `COLOR_0` and `createMesh(vertices, indices, colors)` into an optional color stream of the geometry pool, `packVertexColor`),
  - `KHR_texture_transform`, `KHR_materials_emissive_strength`, `KHR_mesh_quantization` (done 2026-11-21, with a check of `extensionsRequired`; a rotated normal map keeps the authored tangent frame, `D26`), example `09_gltf_materials`,
  - KTX2/Basis textures (done 2026-11-21: `.ktx2` through `loadTexture` and glTF `KHR_texture_basisu`, transcoded on the worker with libktx to BC7/BC5/BC4, ASTC 4×4 or ETC2/EAC, whichever the device samples, RGBA8 as the fallback, with the file's mips; block formats in `Format`, `supportsTextureFormat`, `fillTexture` with every level, a staging path for formats without host image copy (`P2`), two-channel normal maps (`MATERIAL_NORMAL_XY`), example `10_ktx2_textures`: the Flight Helmet in 68.6 MiB instead of 272.6 MiB).
- [x] **Many lights**: clustered forward lighting. Remove the fixed `MAX_LIGHTS = 8`. (Done 2026-11-21: lights live in a GPU light table of `EngineConfig::maxLights` rows (16384 by default), written only when a light changes (`updateLight`, `getLight`). Each shaded view sorts its scene's local lights (point and spot lights with a range) into 16 × 9 × 24 clusters on the GPU in its first cull pass, in two levels (per screen tile, then per cluster) and in a fixed order, and `shadePBR` shades the view's shadowed lights, the global lights (directional, or without a range) and its pixel's cluster, at most `MAX_CLUSTER_LIGHTS` (256). The clusters need no depth buffer, so the `Transparent` pass and a later MSAA target use them as they are. `EngineConfig::clusteredLighting` = false shades every light at every pixel, with the same image. Example `11_many_lights`: 4096 lights take 0.31 ms of assignment and 4.5 ms of draw passes at 1080p (RTX 3060), against 231 ms with every light at every pixel. A light beyond a view's `MAX_VIEW_SHADOWS` shadowed ones has no shadow. Open: far clusters crowd with many short lights (`P15`).)
- [x] Anti-aliasing (MSAA option, with the resolve in a pass of the graph (done 2026-11-21: `RenderTargetDesc::samples`, pipelines per sample count, the Hi-Z from every depth sample, `Engine::addMsaaResolvePass` weighting each sample by 1 / (1 + luminance) so bright edges stay smooth, `EngineConfig::antiAliasing`, examples `09_gltf_materials` and `02_gltf` with `--aa`), and TAA (done 2026-11-21: `InstanceData::previousWorld` written in the sync, a `MOTION_FORMAT` motion color, Halton (2, 3) jitter per view, `Engine::addTaaPass` with nearest-depth motion, Catmull-Rom history, YCoCg variance clipping and brightness weights, a −1 texture mip bias, `resetCameraHistory` and a reset on camera jumps, deterministic; `AntiAliasing::TAA`; 0.37 ms at 1080p on an RTX 3060; `D27`)), bloom (done 2026-11-21: `Engine::addBloomPasses`, 11 compute passes of Jimenez's down and up chain into transient storage render targets, 1/2 to 1/64 of the source, stopping at the last level of at least 2 x 2; `addToneMapPass` mixes it in by `Camera::bloomStrength`; `EngineConfig::bloom`, off by default; example `09_gltf_materials`: 0.37 ms at 1080p on an RTX 3060). With MSAA, glTF `MASK` turns on alpha-to-coverage: the mask pipeline outputs a sharpened alpha, `(alpha - cutoff) / fwidth(alpha) + 0.5`, instead of discarding, so cut-out edges are smooth and keep the material's cutoff (done 2026-11-21).
- [ ] Skinning and animation, morph targets. Node animation is done (2026-11-21): glTF clips (STEP, LINEAR with slerp, CUBICSPLINE) on the nodes that move a mesh, posed on the GPU by an `Animate` pass, one group per playing entity, which every view and shadow map reads through `getWorldMatrix`; `setAnimationTime` (the clock is the application's; the GPU adds only the seconds since a base that moves every 64 s, within 0.1 ms after a day), `playAnimation`, `stopAnimation`, `getAnimations`, `findAnimation`, `ALL_ANIMATION_CLIPS`; motion vectors from the other slot's transforms; example `12_animation`. `Animate` takes 0.17 ms for 10000 playing entities (30k nodes, RTX 3060), and the CPU cost does not grow with them. Open: skinning (next), morph targets, attachments to nodes (`D28`), skipping still or unseen entities (`P18`).
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
