# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

RASM is an early-stage C++20 GPU-driven rendering engine (static lib target `rasm`, alias `rasm::rasm`, + example executables). Vulkan 1.4 is the only backend today; Metal, D3D12 and WebGPU are planned. Shaders are written in Slang. Target users: indie games, visualization tools, map engines.

## Docs — read the relevant one before changing code

- [docs/architecture.md](docs/architecture.md) — how the engine works today: handles/registries, `RenderContext` dedup caches, frame loop, the GPU-driven `prepareScene`/`render2` path, CPU↔GPU struct contract, materials/bindless, asset import, render graph, limits.
- [docs/coding_style.md](docs/coding_style.md) — naming, headers, formatting (aligned `// clang-format off` headers), error handling, Vulkan and Slang rules.
- [docs/code_review.md](docs/code_review.md) — known bugs/design issues with IDs (`B1`, `D4`, `P1`, `C3`, ...). Check it before "fixing" odd behaviour; reference IDs in commits.
- [docs/development_plan.md](docs/development_plan.md) — phased roadmap; phase 0 (stabilization) comes first.
- [docs/build_guide.md](docs/build_guide.md) — toolchain setup (vcpkg, Vulkan SDK, Ninja, Clang).

## Build & run

```powershell
cmake --preset x64-debug            # or x64-release; macOS / macOS-release / macOS-Xcode on macOS
cmake --build --preset x64-debug
cmake --build --preset x64-debug --target 02_gltf     # single example
.\build\x64-debug\examples\02_gltf.exe                 # must run from the repo root
```

- Requires `VCPKG_ROOT`; presets use Ninja + `clang++`. Options: `-DRASM_ENABLE_AVX2=ON` (x86 only), `-DRASM_BUILD_EXAMPLES=OFF`.
- New `.cpp` files must be added to `RASM_SRC` in `src/CMakeLists.txt`. New examples go in `examples/NN_name/main.cpp` plus a `rasm_add_example(NN_name)` line in `examples/CMakeLists.txt`.
- No test suite or lint config yet. Verify changes by running the examples with `EngineConfig::enableValidation = true` — a change that adds validation messages is not done.
- Shaders are compiled at runtime from `./shaders/passes` with includes resolved from `./shaders/lib` (cwd-relative), and examples load `assets/...` relative to cwd. `tools/compile-shaders.bat` is only an offline compile check.

## Layout

- `include/rasm/` is the public API, with the low-level API in `include/rasm/render/`.
- `src/` is private: `core/`, `platform/`, `assets/`, `scene/`, `renderer/`, `rhi/` (+ `rhi/vulkan/`). `src/thirdparty_impl.cpp` is the only file that defines `*_IMPLEMENTATION` macros.
- Private headers are included relative to `src/` (e.g. `"rhi/vulkan/vulkanContext.h"`). `src/` is temporarily a PUBLIC include directory until `D2`; don't add new public→private includes.

## Invariants that are easy to break

- C++ structs in `src/renderer/uberMaterialTypes.h` must match `shaders/lib/core.slang`/`pbr.slang` byte-for-byte (scalar layout); update both sides and the `static_assert`s together.
- `RenderContext::create*` returns an existing handle for an equal `ResourceDesc` (name + fields) — resources that must be distinct need distinct names until `B1` is fixed.
- `frameCount` is the frame-in-flight slot (0/1); per-frame mutable buffers are indexed by it.
- `prepareScene` is cached per scene after the first `render2`; only transforms update afterwards.
- Normal/metallic-roughness/AO textures must be `R8G8B8A8_UNORM`; color/emissive `R8G8B8A8_SRGB`. Shaders output linear color.
- Library config macros (`GLM_FORCE_*`, `VMA_*`) belong in CMake, not in a `.cpp` after headers were included.
