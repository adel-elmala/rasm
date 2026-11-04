# RASM Coding Style

These rules describe how RASM code is written. Most of them are what the code already does. Where the existing code is inconsistent, this guide picks one form: new code follows it, and old code is migrated when it is next touched (the cleanup items are tracked in [code_review.md](code_review.md#5-code-health)).

## 1. Language and toolchain

- **C++20**, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`). The reference compilers are Clang on Windows and Apple Clang on macOS, and the code must also build with MSVC and GCC.
- Use C++20 where it clarifies code: designated initializers for descriptors and config structs (already used widely), `constexpr`, `enum class`, `[[nodiscard]]`.
- No exceptions in engine code. Third-party exceptions (e.g. from tinygltf) are caught at the call site.
- No RTTI-dependent designs (`dynamic_cast`, `typeid`).
- No templates in engine code: no function, class, variable or lambda templates (including `auto` lambda parameters). Write concrete functions per type. Using std and third-party templates (`std::vector`, `std::unordered_map`) is fine, except `std::span`: pass a pointer and a count instead. The only exception is `Handle<Tag>` in `include/rasm/handle.h`.
- File-local helpers (functions, structs) go in one unnamed namespace at the top of the `.cpp` file: right after the includes, or as the first thing inside `namespace rasm` / `rasm::gfx` when the helpers use engine types. Never open a second unnamed namespace or define helpers between functions further down.
- Shaders are **Slang**. Prefer `import` modules over `#include` for new shader code.

## 2. Naming

| Kind | Style | Example |
|---|---|---|
| Namespaces | lower case | `rasm`, `rasm::gfx` |
| Types (class, struct, enum, alias) | PascalCase | `RenderContext`, `PreparedScene`, `MeshHandle` |
| Functions and methods | camelCase, verb first | `createBuffer`, `loadMesh`, `getBindlessTextureIndex` |
| Variables, parameters, members | camelCase, no prefixes | `frameCount`, `megaVertexBuffer` |
| Compile-time constants | UPPER_SNAKE_CASE | `MAX_FRAMES_IN_FLIGHT`, `INVALID_TEXTURE_INDEX` |
| `enum class` values | UPPER_SNAKE_CASE | `TextureUsage::COLOR_ATTACHMENT` |
| File names | camelCase, matching the main type | `renderGraph.h` / `renderGraph.cpp` |
| Macros | `RASM_` prefix, UPPER_SNAKE_CASE | `RASM_ENABLE_AVX2` |

Naming conventions for specific kinds of function:
- `create*` makes a new object and returns a handle.
- `load*` reads from disk.
- `get*` / `set*` are accessors.
- `update*` does a per-frame mutation.
- `destroy*` releases an object.

Deliberate exceptions:
- The BRDF terms in `pbr.slang` keep their names from the literature (`D_GGX`, `F_Schlick`, `V_SmithGGXCorrelated`, `Fd_Lambert`), so the shader can be checked against the references.
- `src/thirdparty_impl.cpp` keeps its name, which matches the `*_IMPLEMENTATION` macros it defines.
- vk-bootstrap's API is snake_case, such as `.physical_device`. Only names RASM declares follow this table.

## 3. Files and headers

- Every header starts with `#pragma once`.
- One main type per header. Small helper types that belong to it may live beside it.
- **Public versus private.** `include/rasm/` is the API that users compile against. Everything under `src/` is private, and `src/` is a PRIVATE include directory, so applications cannot reach it.
  - Public headers must not include backend or third-party implementation headers. That means no `vulkan.h`, `VkBootstrap.h`, `vk_mem_alloc.h`, `slang.h`, `tiny_gltf.h`, `spdlog`, Tracy or `SDL`. glm is the only third-party library in the public API.
  - Forward-declare instead, as `engine.h` does for `tinygltf::Model` and `ShaderCompiler`. A class that must own backend objects keeps them in a private `Impl`, as `RenderContext` does.
  - Backend and implementation headers go under `src/`.
- Include order, with a blank line between groups:
  1. the file's own header (in `.cpp` files),
  2. `rasm/...` headers,
  3. third-party headers,
  4. standard library headers.
- Include paths:
  - public headers as `"rasm/xyz.h"` or `"rasm/render/xyz.h"`,
  - private headers relative to `src/`, e.g. `"core/utils.h"`, `"renderer/shaderCompiler.h"`, `"rhi/vulkan/vulkanDevice.h"`.
- Where new code goes:

  | Kind of code | Directory |
  |---|---|
  | Handles, utilities, allocators | `src/core/` |
  | Window, input, OS | `src/platform/` |
  | File formats and importers | `src/assets/` |
  | Scene model | `src/scene/` |
  | Passes, render graph, shader compilation, GPU structs | `src/renderer/` |
  | Backend-neutral GPU layer | `src/rhi/` |
  | One backend | `src/rhi/<backend>/` |
  | Shared shader modules | `shaders/lib/` |
  | Pass and entry shaders | `shaders/passes/` |
  | A new example | `examples/NN_name/main.cpp`, plus one `rasm_add_example(NN_name)` line in `examples/CMakeLists.txt` |
  | Unit tests | `tests/<subject>Tests.cpp`, added to `rasm_tests` in `tests/CMakeLists.txt` |
- Single-header library implementations (`STB_IMAGE_IMPLEMENTATION`, `TINYGLTF_IMPLEMENTATION`, `VMA_IMPLEMENTATION`, …) are defined only in `src/thirdparty_impl.cpp`. Mind the include order documented there.
- Library configuration macros that change behaviour (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, `VMA_VULKAN_VERSION`) are set **once, in CMake**, with `target_compile_definitions`. Never set them in a `.cpp` file: a define placed after a header has already been included has no effect (see `B2`).

## 4. Formatting

- 4 spaces of indentation, no tabs. Allman braces, where the opening brace goes on its own line.
- **Aligned declarations in headers.** Every header, public or private and however small, wraps its whole content in `// clang-format off` / `// clang-format on`, starting on the first line, and aligns return types, names, member types and default values in columns (see `engine.h`, `renderContext.h`, `vulkanTypes.h`). Keep that alignment when you add lines. References and pointers sit on the type (`const std::string& path`, `Engine* owner`), and braces stay Allman: the formatter skips headers, so nothing fixes these for you.
- Run `clang-format -i <file>` on `.cpp` files you change. The rules are in [.clang-format](../.clang-format), and it leaves `// clang-format off` regions alone, so use those for hand-aligned blocks in `.cpp` files too (see `RenderContext::Impl`).
- The formatter has no column limit, so it keeps your line breaks. Keep lines readable at about 160 columns yourself. Long `spdlog` messages and designated initializers may exceed this.
- Put one field per line in a designated initializer when it doesn't fit comfortably on one line. Multi-line lambdas take their brace on the next line.

## 5. Types and data

- **Handles, not pointers, cross the public API.** Create through `Engine`, refer through `XxxHandle`, and check with `isValid()`. Never hand out raw pointers or references to registry storage: they are invalidated when the vector grows.
- **Descriptors.**
  - Resource creation takes a typed descriptor (`BufferDesc`, `TextureDesc`, `PipelineDesc`, …), filled with designated initializers or field assignment.
  - A descriptor is an aggregate with a default member initializer on every field that isn't a class type, and a defaulted `operator==`. No unions (`B18`, `D5`).
  - Put fields in a descriptor only when they affect the created object. A deduplicated descriptor's hash in `src/rhi/resource.cpp` must fold every field with `hashCombine`.
- **GPU-visible structs** (anything a shader reads through a pointer or a buffer):
  - live in `src/renderer/uberMaterialTypes.h` (or its successor) and are mirrored in `shaders/lib/core.slang`,
  - use only `uint32_t`, `int32_t`, `float`, `uint64_t`, `glm::vec2/3/4`, `glm::mat3/4` (scalar layout),
  - have a `static_assert` on `sizeof` and on every offset that a later field's correctness depends on,
  - are listed in `validateRasmStructs` in `src/renderer/shaderCompiler.cpp`, which checks them against Slang reflection in debug builds,
  - change on both sides in the same commit.
- Use fixed-width integer types (`uint32_t`, `uint64_t`) for anything stored, serialized or sent to the GPU. Use `size_t` for container sizes and byte counts on the CPU.
- Prefer `std::vector` + index over node-based containers in hot paths. `unordered_map` is fine for lookup caches that are off the per-draw path.
- Pass non-trivial objects by `const&`. Never copy `Mesh`, `GLTFRaw`, `Scene` or backend handle structs by value in a loop: they are large (`C5`).

## 6. Error handling and logging

- **Recoverable runtime failures** (missing file, unsupported format, allocation failure, Vulkan error) are logged with `spdlog::error` and the function returns an invalid handle, `false`, an empty `std::optional`, or a default value. The caller checks the result, and a `bool`/`optional` return must not be silently ignored (add `[[nodiscard]]`).
- **Programmer errors** (null data pointer, empty shader code, out-of-range index into an internal array):
  - At the public API (`RenderContext`, the layer every application call goes through), check them in every build: `checkArgument` in `renderContext.cpp` logs with `spdlog::error`, asserts so a debug build stops at the call, and the function returns an invalid handle or `false`.
  - Inside the engine (RHI backends, internal arrays) they use plain `assert`: only engine code reaches them, after the public layer validated its input.
- Log levels:
  - `error`: an operation failed.
  - `warn`: degraded but continuing (limit reached, fallback used).
  - `info`: lifecycle (device selected, engine started or shutting down).
  - `debug`: per-event detail.
- Nothing per frame may log above `debug`.
- Vulkan results always pass through `toVkResultString` (or its successor) in messages.

## 7. Comments

- Comment **why**, not what. Good existing examples are the scalar-layout warnings in `uberMaterialTypes.h`, the `SV_VertexID` lowering note in `rasm.slang`, and the color-space notes in `material.cpp`.
- Every GPU-layout constraint, synchronization decision, and coordinate-convention flip gets a comment at the site.
- `TODO:` comments name the missing piece concretely (`// TODO: support multiple instances`). Long-lived TODOs belong in [development_plan.md](development_plan.md).

## 8. Vulkan and backend code

- All Vulkan calls live under the backend (`src/rhi/vulkan/`). Only the backend and `src/rhi/device.cpp` include its headers: the rest of the engine, `RenderContext` included, talks to `rhi::Device` ([architecture.md §12](architecture.md#12-rhi)). The one exception is the Vulkan surface hook in `src/platform/`, until `D7`.
- New GPU functionality goes into `src/rhi/device.h` in terms that every planned backend (Vulkan, D3D12, Metal, WebGPU) can implement, never as a Vulkan-only path above the RHI.
- Use synchronization2 (`vkCmdPipelineBarrier2`, `VkDependencyInfo`) and dynamic rendering. Do not introduce render passes or framebuffers.
- Every created object gets a debug name, which is a no-op when debug utils is unavailable.
- Check every `VkResult`. Swapchain functions must handle `VK_SUBOPTIMAL_KHR` and `VK_ERROR_OUT_OF_DATE_KHR` explicitly.
- Test new features with the validation layers on (`EngineConfig::enableValidation = true`). A change that introduces validation messages is not done.

## 9. Shaders (Slang)

- Entry points are `vertMain`, `fragMain` and (future) `compMain`, with `[shader("...")]` attributes.
- One responsibility per module: data layout (`core.slang`), lighting and BRDF (`pbr.slang`), unlit shading (`unlit.slang`), pass or entry shaders (`rasm.slang`).
- Shaders output **linear** color. sRGB encoding is done by the attachment format.
- Texture access checks `hasTexture(index)` before sampling, and glTF default values apply when a texture is absent.
- Keep backend-specific code (`spirv_asm`, `vk::` attributes, raw pointers) behind small helper functions in one module, so that other targets can provide alternatives (`D12`).

## 10. Commits

- Short imperative subject. Use a prefix when it helps: `feat:`, `fix:`, `refactor:`, `docs:`, `build:`.
- Keep a change that alters a GPU struct, its Slang mirror and its asserts in one commit.
- Do not mix formatting-only changes with behaviour changes.
