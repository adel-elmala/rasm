# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

RASM is an early-stage C++20 GPU-driven rendering engine (static lib target `rasm`, alias `rasm::rasm`, + example executables). Vulkan 1.4 is the only backend today; Metal, D3D12 and WebGPU are planned. Shaders are written in Slang. Target users: indie games, visualization tools, map engines.

## Docs — read the relevant one before changing code

- [docs/architecture.md](docs/architecture.md) — how the engine works today: handles/registries, `RenderContext` dedup caches, frame loop, the GPU-driven path (GPU scene tables, `syncGpuScene`, `drawScene`), CPU↔GPU struct contract, materials/bindless, asset import, render graph, limits.
- [docs/coding_style.md](docs/coding_style.md) — naming, headers, formatting (aligned `// clang-format off` headers), error handling, Vulkan and Slang rules.
- [docs/code_review.md](docs/code_review.md) — known bugs/design issues with IDs (`B1`, `D4`, `P1`, `C3`, ...). Check it before "fixing" odd behaviour; reference IDs in commits.
- [docs/development_plan.md](docs/development_plan.md) — phased roadmap; phase 0 (stabilization) comes first.
- [docs/build_guide.md](docs/build_guide.md) — toolchain setup (vcpkg, Vulkan SDK, Ninja, Clang).

## Build & run

```powershell
cmake --preset x64-debug            # or x64-release; macOS / macOS-release / macOS-Xcode on macOS
cmake --build --preset x64-debug
cmake --build --preset x64-debug --target 02_gltf     # single example
.\build\x64-debug\examples\02_gltf.exe                 # runs from any directory
ctest --preset x64-debug                               # unit tests
cmake --install build\x64-debug                        # into install/, for find_package(rasm)
```

- Requires `VCPKG_ROOT`; presets use Ninja + `clang++`. Options: `-DRASM_ENABLE_AVX2=ON` (x86 only), `-DRASM_BUILD_EXAMPLES=OFF`, `-DRASM_BUILD_TESTS=OFF`, `-DRASM_INSTALL=OFF`, `-DRASM_SHADER_DIR=<path>`.
- A new dependency linked to `rasm`, PUBLIC or PRIVATE, also needs a `find_dependency` line in `cmake/rasmConfig.cmake.in`: `rasm` is static, so consumers link it too.
- Run `clang-format -i <file>` on `.cpp` files you change. It keeps line breaks (no column limit) and skips `// clang-format off` regions. Every header is wrapped in those guards and hand-aligned in columns, so new headers need them too (see `docs/coding_style.md` section 4).
- `rasm_tests` (doctest, no GPU) covers CPU-side code only, and there is no lint config yet. Verify rendering changes by running the examples with `EngineConfig::enableValidation = true` — a change that adds validation messages is not done.
- Shaders are compiled at runtime from `EngineConfig::shaderDir`, by default a `shaders/` folder beside the executable. Examples set `shaderDir` to the repo's `shaders/` and `assetRoot` to its `assets/`, so a shader edit shows up the next time an example starts, with no build. `tools/compile-shaders.bat` is only an offline compile check.

## Adding files

| Adding | Where | Also |
|---|---|---|
| A part (`.h` + `.cpp`) | camelCase names, in its layer's folder (`src/scene/`, `src/rhi/vulkan/`, ...). Public header in `include/rasm/` only if applications use it. | Add the `.cpp` to `RASM_SRC` in `src/CMakeLists.txt`, next to its folder's files. |
| Any header | — | Whole file inside `// clang-format off` ... `// clang-format on`, then `#pragma once`, with declarations hand-aligned in columns (`docs/coding_style.md` section 4). |
| An example | `examples/NN_name/main.cpp` | `rasm_add_example(NN_name)` in `examples/CMakeLists.txt`. Set `enableValidation = true`, `assetRoot = RASM_EXAMPLE_ASSET_ROOT`, `shaderDir = RASM_EXAMPLE_SHADER_DIR` like `02_gltf`. |
| A test | `tests/<name>Tests.cpp` | Add it to the `rasm_tests` list in `tests/CMakeLists.txt`. |
| A shader | `shaders/passes/` for pass entry points, `shaders/lib/` for shared modules | No build step. GPU structs go through `core.slang` (see Invariants). |

Then run `clang-format -i` on the new `.cpp` files and build. CMake reconfigures by itself when a `CMakeLists.txt` changes.

## Layout

- `include/rasm/` is the public API, with the low-level API in `include/rasm/render/`.
- `src/` is private: `core/`, `platform/`, `assets/`, `scene/`, `renderer/`, `rhi/` (+ `rhi/vulkan/`). `src/thirdparty_impl.cpp` is the only file that defines `*_IMPLEMENTATION` macros.
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
- GPU scene tables (instances, materials, draw lists) have one buffer per frame slot and are only written in `syncGpuScene`, after that slot's fence wait. Change rows through `GpuTable::setRow` (or mark a draw list for rebuild); never write a slot's buffer from anywhere else.
- Normal/metallic-roughness/AO textures must be `R8G8B8A8_UNORM`; color/emissive `R8G8B8A8_SRGB`. Shaders output linear color.
- Library config macros (`GLM_FORCE_*`, `VMA_*`) belong in CMake, not in a `.cpp` after headers were included.
