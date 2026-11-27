# Changelog

All notable changes to RASM are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and RASM follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before 1.0.0 the C++ API can change in any minor release. The C API (`include/rasm/rasm_c.h`) has its own version (`RASM_C_API_VERSION`) and stays compatible within a major version: a minor version only adds to it.

## [Unreleased]

### Added

- Offscreen export at any size: `Renderer::exportView` draws a still of a view (an 8K, a 16K, a poster larger than the device's biggest image) in tiles with an off-center projection, with no seams at tile borders (guard bands, labels and transparency ranges decided once for the whole image, a frozen streaming cut), anti-aliased by averaging jittered samples, and streams it to a PNG (8 or 16 bits) or an EXR (half or float) file, with a transparent background and scene-linear HDR as options; `getExportProgress` polls it without blocking. C API 1.16: `rasm_export_view` and its calls. zlib is a new dependency of `rasm::renderer`.
- The render graph from C (C API 1.17): a C program or a binding builds its own frame with `rasm_render_graph`, RASM's passes (shadows, scene, MSAA resolve, TAA, bloom, tone map, UI) and passes of its own, each a C function the graph calls with a `rasm_commands` that records draws, dispatches (indirect too), push constants, viewports, scissors, barriers and GPU timers, valid only during the call. Also buffers, pipelines from Slang source or SPIR-V (with a blend mode, depth and a topology), bindless slots, a render target's textures and the GPU timers. Calls that would start a frame or free what it uses are refused inside a pass callback, and destroying a renderer detaches its graphs. Example `33_c_render_graph`; a Python ctypes pass in the CI smoke test. C++: `Renderer::createShader` compiles one Slang entry point for a `PipelineDesc` of your own.

- Shadows per view: `View::shadows` (up to `MAX_VIEW_SHADOWS`) makes `render(views, count)` draw each view's shadows fitted to its own camera, so split screen, a map and editor viewports have shadows without a graph of their own. Views of different cameras draw one set of maps per light, size and count in turn, so the memory does not grow with the views (C API 1.18: `shadow_count` and `shadows` at the end of `rasm_view`). `examples/20_views` and `examples/22_c_api` show them; image test `views_shadows`.
- Debug views and the buffer inspector from C (C API 1.18): `rasm_set_debug_view`, `rasm_get_debug_view`, `rasm_set_culling_frozen`, `rasm_set_meshlet_path`, `rasm_get_meshlet_path`, `rasm_set_inspection` and `rasm_get_inspection`; `Renderer::getDebugView` in C++. `examples/33_c_render_graph --debug-view normals`.

### Changed

- `MAX_GPU_TIMERS` is 256 (was 64), since a frame with shadowed views has more passes (`D21`).

### Fixed

- A point cloud drawn after other scenes could lose the device (a page fault on an RTX 3060, a crash on lavapipe): the point resolve's empty pixels, which `discard` leaves running as helpers, read the point of instance row 0, a mesh there, and followed it out of the point buffer (`B39`).
- A scene with no light crashed on lavapipe: its light loops indexed a null pointer, which lavapipe loads even for a loop of 0 lights (`B38`).

## [0.1.0] - YYYY-MM-DD (draft, not released yet)

The first release. Vulkan 1.4 is the only backend. Windows x64 is built and tested in CI; macOS (through MoltenVK) is built and unit-tested in CI; Linux is not tested yet.

### GPU-driven core

- The scene lives in GPU tables (instances, materials, geometry, draw lists), one buffer per frame in flight, and a frame uploads only the rows that changed (#33).
- A persistent geometry pool: meshes share mega vertex and index buffers, and entities that share a mesh share its geometry.
- GPU frustum culling and two-pass hierarchical-Z occlusion culling into compacted indirect draw lists (#35, #38).
- Automatic mesh LODs built with meshoptimizer as meshes load, chosen on the GPU by screen-space error (#39).
- Meshlets with meshlet culling, drawn with mesh shaders where the device has `VK_EXT_mesh_shader`, and by vertex pulling otherwise (#85, #86).
- Bindless textures and buffer device addresses: a handful of indirect draws per frame, not one per object.
- A render graph with automatic barriers and layout transitions, imported and transient resources with memory aliasing, per-pass attachment load and store ops, async compute, a graph dump and a GPU timestamp per pass (#41, #45, #46, #47, #48).
- A backend-neutral RHI with typed descriptors, generational handle pools and deferred destruction, and a destroy call for every object (#26, #78, #79).
- Vulkan is loaded at run time with volk: a clear error when the loader or a Vulkan 1.4 device is missing (#76).
- Shaders in Slang, compiled at run time on worker threads at start-up, with SPIR-V and pipeline caches on disk (#80, #108).
- Zero heap allocations in a steady Release frame (#84).

### Rendering

- Physically based materials (glTF metallic-roughness), with alpha modes (opaque, cut-out, sorted blending), vertex colors, `KHR_texture_transform`, `KHR_materials_emissive_strength` and `KHR_mesh_quantization` (#10, #59).
- Image-based lighting baked on the GPU from an equirectangular HDR environment (#56).
- Cascaded shadow maps for directional lights, and shadows for spot and point lights (#54, #55).
- Clustered forward lighting, with 16,384 lights by default (#62).
- An HDR color target with AgX tone mapping and exposure, bloom, MSAA with alpha-to-coverage, and TAA with motion vectors (#52, #64, #65, #66).
- glTF node animation, skinning and morph targets, all evaluated on the GPU (#67, #68, #69).
- Reversed-Z with an infinite far plane by default (#31).
- Optional Dear ImGui integration (`RASM_IMGUI`), a stats overlay, debug views (normals, overdraw, LODs, culling, light counts), frozen culling and a GPU buffer inspector (#36, #70, #71, #72).

### Assets and streaming

- glTF 2.0 and GLB, OBJ, KTX2 and Basis textures (transcoded to the block formats the device samples), Radiance HDR environments, and LAS and LAZ point clouds (#6, #61, #106).
- Background loading on one bounded worker pool with a memory budget, and a job per glTF image (#82).
- Texture mips generated on the workers, sub-allocated textures and optional host image copy (#81).
- Load uploads on a dedicated transfer queue where the GPU has one, with queue ownership transfers (#83).
- Tile streaming under a GPU memory budget, showing a parent tile while a finer one loads (`TileStreamer`) (#97).

### Large worlds and maps

- Double-precision world positions on the CPU and camera-relative rendering on the GPU: a scene 6,370 km from the origin renders without jitter (#95, #96).
- Terrain from height-map tiles, with seamless edges, normals and draped imagery (#98).
- Anti-aliased vector lines, polygons and points, draped on the terrain, with meshes built off the main thread (#99, #103).
- SDF text labels placed per view, with halos, priorities and decluttering (#100).
- Picking: the entity and vector feature under a pixel or a rectangle, with its world position in double precision (#101).

### Visualization

- Orthographic views with pan and zoom at the cursor, turntable and arcball trackballs, and anti-aliased reference grids (#102).
- Colormaps (viridis, turbo, magma, inferno, plasma, cividis, gray) and point clouds in GPU memory (#104).
- A compute rasterizer for small points, used up to the point size where it stops paying off (#105, #107).
- Point-cloud octree streaming of LAS and LAZ files larger than the GPU memory budget (#106).
- Instanced glyphs (arrows, spheres or any mesh), culled per instance on the GPU, and data lines (#110).

### Embedding

- `rasm::renderer`, the renderer alone, with no SDL and no file loaders; `rasm::rasm` adds the `Engine`, the SDL platform and the `AssetLoader` (#91, #92).
- Native window handles (Win32, Metal layer, X11, XCB, Wayland) and one input event stream; only Win32 and Metal are tested so far (#87).
- Several windows, several views per window, viewports and render-to-texture (#89, #90).
- Headless rendering with frame and texture readback (#73).
- Camera controller helpers that the application drives (#88).
- Install and export: `find_package(rasm)` with the `renderer` component.

### C API 1.14

- `include/rasm/rasm_c.h` and the shared libraries `rasm_c` (`rasm::c`) and `rasm_c_renderer` (`rasm::c_renderer`): a stable C ABI over the renderer, the `Engine` and the `AssetLoader`, for bindings in other languages (#93, #94).
- Each feature above that applications use from C was added in a minor version, up to 1.14: double-precision positions, tile streaming, terrain, vector data, labels, picking, orthographic views and grids, colormaps, point clouds and their streaming, and glyphs.
- Not in C yet: the render graph and custom passes, `RenderContext`, shadows in `render(views)`, Dear ImGui and the debug tools.

### Tooling, docs and CI

- CI on Windows and macOS for every pull request: Debug and Release builds and unit tests, golden-image tests on lavapipe (Windows), a check that nothing links the Vulkan loader, and an install check with C++ and C consumers and a Python (ctypes) smoke test of the C API (#32, #57, #74).
- About 430 unit tests (doctest, no GPU) and 47 golden images.
- Every example runs headless: `--headless --frames N --out frame.png` (#75).
- Warning-free builds, clang-tidy and ASan/UBSan presets (#44).
- Tracy profiling zones and GPU timers.
- HTML documentation in `docs/`: an API reference, and 79 concept guides with interactive figures, checked against the code by `docsync.py`.
- 31 examples, from `00_hello_scene` to `30_glyphs`.

### Known limits

- Vulkan 1.4 only; Metal, Direct3D 12 and WebGPU backends are planned.
- Only FIFO (vsync) present mode, as a workaround for a crash on NVIDIA (`B21`).
- Point clouds and glyphs cast no shadows (`D50`, `D52`); the environment light is not shadowed (`D23`).
- Text is laid out without shaping, so scripts such as Arabic do not join (`D46`).
- See [`docs/code_review.md`](docs/code_review.md) for the full list.

[Unreleased]: https://github.com/adel-elmala/rasm/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/adel-elmala/rasm/releases/tag/v0.1.0
