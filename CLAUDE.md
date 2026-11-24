# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

RASM is an early-stage C++20 GPU-driven rendering engine (two static libs: `rasm::renderer`, the renderer alone, and `rasm::rasm`, which adds the Engine, the SDL platform and the AssetLoader; + example executables). Vulkan 1.4 is the only backend today; Metal, D3D12 and WebGPU are planned. Shaders are written in Slang. Target users: indie games, visualization tools, map engines.

## Docs — read the relevant one before changing code

- [docs/architecture.md](docs/architecture.md) — how the engine works today: handles/registries, `RenderContext` dedup caches, frame loop, the GPU-driven path (GPU scene tables, `syncGpuScene`, the scene passes of `addScenePasses`), CPU↔GPU struct contract, materials/bindless, asset import, render graph, limits.
- [docs/coding_style.md](docs/coding_style.md) — naming, headers, formatting (aligned `// clang-format off` headers), error handling, Vulkan and Slang rules.
- [docs/code_review.md](docs/code_review.md) — known bugs/design issues with IDs (`B1`, `D4`, `P1`, `C3`, ...). Check it before "fixing" odd behaviour; reference IDs in commits.
- [docs/development_plan.md](docs/development_plan.md) — phased roadmap; phase 0 (stabilization) comes first.
- [docs/build_guide.md](docs/build_guide.md) — toolchain setup (vcpkg, Vulkan SDK, Ninja, Clang).
- [docs/index.html](docs/index.html) — HTML docs for humans: an engine tour plus concept guides, one idea each, explained on its own and ending with an "In RASM" section (`docs/guides/`), and the API reference for application developers (`docs/api/`). Open in a browser; no build.

## Keep the HTML docs in sync with main

The guides and the API reference must always describe the code on `main`. Use the `sync-docs` skill:

- Every change to `include/`, `src/`, `shaders/`, `examples/` or `cmake/` updates the pages that describe it, in the same change (`finish-task` runs `sync-docs`). A new public function or type gets its API entry; changed behaviour gets its diagram fixed, not only its text.
- A change that brings a new concept, algorithm or technique into RASM (a shadow map, a new culling scheme, a new memory strategy, ...) also gets a new concept guide in `docs/guides/`, in the same change, without being asked. Write it when a contributor would need the idea explained to follow the code; skip it only for a small variation of a topic a guide already covers, and then extend that guide instead. Say in the report which guide you added, or why none was needed.
- At the start of a session, run `python .claude/skills/sync-docs/scripts/docsync.py status --target main`. If it lists changed code, tell the user the docs are behind main and offer to sync them.
- Guides have interactive pictures ("labs"): `figure.lab` in the page, code in `docs/assets/labs/<page>.js`, built on `docs/assets/lab.js`. A lab computes the real rule, so a code change that changes that rule or its numbers changes the lab too. `docsync.py labs` runs them all in headless Chrome and must pass.
- `docsync.py check` must pass before the docs are called done. `docs/assets/sync.js` (the stamp) is written only by `docsync.py stamp`.
- New pages are added to `SECTIONS` in `docs/assets/site.js`, the one list of pages, and follow the house style in the skill (visual first, plain short English, classes from `docs/assets/style.css` only).

## Build & run

```powershell
cmake --preset x64-debug            # or x64-release; macOS / macOS-release / macOS-Xcode on macOS
cmake --build --preset x64-debug
cmake --build --preset x64-debug --target 02_gltf     # single example
.\build\x64-debug\examples\02_gltf.exe                 # runs from any directory
ctest --preset x64-debug                               # unit tests
cmake --install build\x64-debug                        # into install/, for find_package(rasm)
```

- Requires `VCPKG_ROOT`; presets use Ninja + `clang` / `clang++`. Options: `-DRASM_ENABLE_AVX2=ON` (x86 only), `-DRASM_IMGUI=OFF` (no Dear ImGui), `-DRASM_C_API=OFF` (no C API libraries), `-DRASM_BUILD_EXAMPLES=OFF`, `-DRASM_BUILD_TESTS=OFF`, `-DRASM_INSTALL=OFF`, `-DRASM_SHADER_DIR=<path>`.
- A new dependency linked to `rasm_renderer` or `rasm`, PUBLIC or PRIVATE, also needs a `find_dependency` line in `cmake/rasmConfig.cmake.in`, in that target's part (the `renderer` component, or `rasm`): both are static, so consumers link it too. `rasm_renderer` never links SDL3, tinygltf, tinyobjloader or libktx (`examples/21_renderer` links only it).
- Run `clang-format -i <file>` on `.cpp` files you change. It keeps line breaks (no column limit) and skips `// clang-format off` regions. Every header is wrapped in those guards and hand-aligned in columns, so new headers need them too (see `docs/coding_style.md` section 4).
- `rasm_tests` (doctest, no GPU) covers CPU-side code only. Verify rendering changes by running the examples with `EngineConfig::enableValidation = true` — a change that adds validation messages is not done. Run them headless (`--headless --frames N --out <png>`, which `verify-change` does), so no window opens.
- Analysis (local only, see [docs/build_guide.md](docs/build_guide.md#static-and-dynamic-analysis)): every build has `RASM_WARNINGS` on and must stay warning-free. `cmake --build --preset x64-tidy` runs clang-tidy (`.clang-tidy`) and `x64-asan` builds with ASan + UBSan (`macOS-asan`, `macOS-tsan` on macOS). Turn a check off only with a reason in `.clang-tidy`, or `// NOLINT(<check>): <reason>` at the site.
- Shaders are compiled at runtime from `EngineConfig::shaderDir`, by default a `shaders/` folder beside the executable. Examples set `shaderDir` to the repo's `shaders/` and `assetRoot` to its `assets/`, so a shader edit shows up the next time an example starts, with no build. `tools/compile-shaders.bat` is only an offline compile check.

## Adding files

| Adding | Where | Also |
|---|---|---|
| A part (`.h` + `.cpp`) | camelCase names, in its layer's folder (`src/scene/`, `src/rhi/vulkan/`, ...). Public header in `include/rasm/` only if applications use it. | Add the `.cpp` to `target_sources(rasm_renderer PRIVATE ...)` in `src/CMakeLists.txt`, next to its folder's files, or to `target_sources(rasm PRIVATE ...)` for the Engine, the SDL platform and the file loaders. Public headers need no entry: every `.h` under `include/` is picked up and installed. |
| Any header | — | Whole file inside `// clang-format off` ... `// clang-format on`, then `#pragma once`, with declarations hand-aligned in columns (`docs/coding_style.md` section 4). |
| An example | `examples/NN_name/main.cpp` | `rasm_add_example(NN_name)` in `examples/CMakeLists.txt`. Set `enableValidation = true`, `assetRoot = RASM_EXAMPLE_ASSET_ROOT`, `shaderDir = RASM_EXAMPLE_SHADER_DIR` like `02_gltf`. Parse the run flags with `parseExampleArgs` (`examples/exampleArgs.h`), call `applyExampleArgs(args, config)` before creating the engine, `waitForLoads` before the loop under `--wait-for-loads`, loop on `exampleRunning(engine, args)` instead of `engine.running()`, and `return exampleExit(args);` from `main`, so `--headless --frames N --out <png>` works and fails with exit code 1 when the frame was not written. |
| A test | `tests/<name>Tests.cpp` | Add it to the `rasm_tests` list in `tests/CMakeLists.txt`. |
| A shader | `shaders/passes/` for pass entry points, `shaders/lib/` for shared modules | No build step. GPU structs go through `core.slang` (see Invariants). |

Then run `clang-format -i` on the new `.cpp` files and build. CMake reconfigures by itself when a `CMakeLists.txt` changes.

## Layout

- `include/rasm/` is the public API, with the low-level API in `include/rasm/render/`.
- `src/` is private: `core/`, `platform/`, `assets/`, `scene/`, `renderer/`, `rhi/` (+ `rhi/vulkan/`), and `c/`, the C API (`include/rasm/rasm_c.h`) built into the shared libraries `rasm_c` and `rasm_c_renderer`. `src/thirdparty_impl.cpp` (the renderer's) and `src/thirdparty_loaders_impl.cpp` (the loaders') are the only files that define `*_IMPLEMENTATION` macros.
- `tests/` links `rasm::rasm` like any application. It also has `src/` on its include path, only to test private classes against a fake `rhi::Device` (see `tests/uploadManagerTests.cpp`).
- Private headers are included relative to `src/` (e.g. `"rhi/vulkan/vulkanDevice.h"`). `src/` is a PRIVATE include directory, so public headers can't include private ones; forward-declare, or use a private `Impl` like `RenderContext` does.

## Code rules

- Keep it simple and direct. Solve the problem in front of you with the least code that does it: no extra layers, helper abstractions or generality for cases that don't exist yet. When a refactor or a new abstraction seems worth it, propose it in a sentence and wait for a yes before writing it.
- One `.h`/`.cpp` pair per logical part (camera, mesh, entity, renderContext, ...), each with one job that doesn't leak into other files. New functionality that is its own part (e.g. a glTF importer, a texture loader) gets a new pair. Don't grow an unrelated file with it. Types shared between files go in headers.
- No templates in RASM code: no function, class, variable or lambda templates (`template <...>`, `auto` parameters in lambdas). Write concrete overloads or functions per type instead. Using third-party/std templates (`std::vector`, `std::unordered_map`) is fine. The only exception is `Handle<Tag>` in `include/rasm/handle.h`.
- File-local helpers (types used by only that `.cpp`, and functions) go in unnamed namespaces at the top of the `.cpp` file: right after the includes, or as the first thing inside `namespace rasm` / `rasm::gfx` when the helpers use engine types. Types come first. Prefer one unnamed namespace for types followed by a second one for helper functions. Never define helpers between functions further down.

## Invariants that are easy to break

- C++ structs in `src/renderer/uberMaterialTypes.h` must match `shaders/lib/core.slang` byte-for-byte (scalar layout); update both sides, the `static_assert`s and the `validateRasmStructs` tables in `src/renderer/shaderCompiler.cpp` together.
- `RenderContext::createBuffer`/`createTexture` always create a new resource (no dedup since `B1`) — cache at the asset level if you want sharing, and destroy what you replace. Shaders, pipelines and descriptor set layouts are still deduplicated: their descriptors (`ShaderDesc`, `PipelineDesc`, `BindlessDescriptorSetLayoutDesc`) compare every field through a defaulted `operator==`, but a new field must also be added to the matching hash in `src/rhi/resource.cpp`.
- `frameCount` is the frame-in-flight slot (0/1); per-frame mutable buffers are indexed by it.
- A load's fills (between `RenderContext::beginLoadUploads` and `endLoadUploads`) run on the transfer queue with no wait for the frames: they may only write new memory or ranges the release queue gave back, and their data is used only once `isLoadUploadDone` (a mesh `READY`, a texture out of `pendingTextures`). Overwrites of data a frame may read stay on the frames' uploads.
- GPU scene tables (instances, materials, draw lists) have one buffer per frame slot and are only written in `syncGpuScene`, after that slot's fence wait. Change rows through `GpuTable::setRow` (or mark a draw list for rebuild); never write a slot's buffer from anywhere else.
- Color/emissive textures use an sRGB format and normal/metallic-roughness/AO a UNORM one: `R8G8B8A8_SRGB` / `R8G8B8A8_UNORM` for images; for KTX2 the slot picks the sRGB or UNORM block format. Shaders output linear color.
- Library config macros (`GLM_FORCE_*`, `VMA_*`) belong in CMake, not in a `.cpp` after headers were included.
- `include/rasm/rasm_c.h` is a stable ABI: within a major version only add at the end (functions, enum constants, fields of a struct that starts with `size`), and bump `RASM_C_API_VERSION_MINOR`. A C++ enum value or config field that C shows changes its C constant (and `static_assert` in `src/c/cApi.cpp`), its `toC`/`toCpp` conversion and `tests/c_api/ctypes_smoke.py` together. Every C entry point is a function try block ending in `RASM_C_CATCH`.
