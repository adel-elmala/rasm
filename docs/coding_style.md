# RASM Coding Style

These rules describe how RASM code is written. Most of them are what the code already does. Where the existing code is inconsistent, this guide picks one form: new code follows it, and old code is migrated when it is next touched (the cleanup items are tracked in [code_review.md](code_review.md#5-code-health)).

## 1. Language and toolchain

- **C++20**, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`). The reference compilers are Clang on Windows and Apple Clang on macOS, and the code must also build with MSVC and GCC.
- Use C++20 where it clarifies code: designated initializers for descriptors and config structs (already used widely), `std::span`, `constexpr`, `enum class`, `[[nodiscard]]`.
- No exceptions in engine code. Third-party exceptions (e.g. from tinygltf) are caught at the call site.
- No RTTI-dependent designs (`dynamic_cast`, `typeid`).
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

Known deviations to migrate, rather than copy:
- `VulkanContext` uses snake_case members (`physical_device`, `graphics_queue`) and `_init_*` free functions.
- The GPU enums `gfx::VertexFormat` and `gfx::LightType` use unscoped PascalCase values. Make them `enum class ... : uint32_t` with UPPER_SNAKE values.
- `ShaderCompiler::compiledShader` should be `CompiledShader`.
- `registery` should be `registry`, and `metallicRoughtness` should be `metallicRoughness`.
- `RenderGraph::CreateTexture` / `Read` / `Write` should become camelCase.

## 3. Files and headers

- Every header starts with `#pragma once`.
- One main type per header. Small helper types that belong to it may live beside it.
- **Public versus private.** `include/rasm/` is the API that users compile against. Everything under `src/` is private. `src/` is temporarily also a public include directory until `D2` is done; don't add new uses of that.
  - Public headers must not include backend or third-party implementation headers. That means no `vulkan.h`, `VkBootstrap.h`, `vk_mem_alloc.h`, `slang.h`, `tiny_gltf.h` or `SDL`.
  - Forward-declare instead, as `engine.h` already does for `tinygltf::Model`.
  - Backend and implementation headers go under `src/`.
- Include order, with a blank line between groups:
  1. the file's own header (in `.cpp` files),
  2. `rasm/...` headers,
  3. third-party headers,
  4. standard library headers.
- Include paths:
  - public headers as `"rasm/xyz.h"` or `"rasm/render/xyz.h"`,
  - private headers relative to `src/`, e.g. `"core/utils.h"`, `"renderer/shaderCompiler.h"`, `"rhi/vulkan/vulkanContext.h"`.
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
- Single-header library implementations (`STB_IMAGE_IMPLEMENTATION`, `TINYGLTF_IMPLEMENTATION`, `VMA_IMPLEMENTATION`, …) are defined only in `src/thirdparty_impl.cpp`. Mind the include order documented there.
- Library configuration macros that change behaviour (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, `VMA_VULKAN_VERSION`) are set **once, in CMake**, with `target_compile_definitions`. Never set them in a `.cpp` file: a define placed after a header has already been included has no effect (see `B2`).

## 4. Formatting

- 4 spaces of indentation, no tabs. Allman braces, where the opening brace goes on its own line.
- **Aligned declarations in headers.** Headers that declare large APIs or structs wrap their body in `// clang-format off` / `// clang-format on` and align return types, names, member types and default values in columns (see `engine.h`, `renderContext.h`, `vulkanTypes.h`). Keep that alignment when you add lines.
- `.cpp` files are formatted with clang-format. The repo has no `.clang-format` yet. Until one is committed, match the surrounding code. A starting point that matches the current `.cpp` style:

  ```yaml
  BasedOnStyle: Microsoft      # 4-space indent, Allman braces
  ColumnLimit: 160
  AccessModifierOffset: -4
  NamespaceIndentation: All
  PointerAlignment: Right
  SortIncludes: Never          # include order is semantic, see §3
  AllowShortFunctionsOnASingleLine: Inline
  ```
- Keep lines readable at about 160 columns. Long `spdlog` messages and designated initializers may exceed this.

## 5. Types and data

- **Handles, not pointers, cross the public API.** Create through `Engine`, refer through `XxxHandle`, and check with `isValid()`. Never hand out raw pointers or references to registry storage: they are invalidated when the vector grows.
- **Descriptors.**
  - Resource creation takes a descriptor struct (`ResourceDesc`) that is zero-initialized and filled with designated initializers or field assignment.
  - Descriptor structs must be value-initialized to zero. Unions must not be left indeterminate (`B18`).
  - Put fields in a descriptor only when they are compared and hashed. Anything that affects the created object must take part in equality.
- **GPU-visible structs** (anything a shader reads through a pointer or a buffer):
  - live in `src/renderer/uberMaterialTypes.h` (or its successor) and are mirrored in `shaders/lib/core.slang`,
  - use only `uint32_t`, `int32_t`, `float`, `uint64_t`, `glm::vec2/3/4`, `glm::mat3/4` (scalar layout),
  - have a `static_assert` on `sizeof` and on every offset that a later field's correctness depends on,
  - change on both sides in the same commit.
- Use fixed-width integer types (`uint32_t`, `uint64_t`) for anything stored, serialized or sent to the GPU. Use `size_t` for container sizes and byte counts on the CPU.
- Prefer `std::vector` + index over node-based containers in hot paths. `unordered_map` is fine for lookup caches that are off the per-draw path.
- Pass non-trivial objects by `const&`. Never copy `Mesh`, `GLTFRaw`, `Scene` or backend handle structs by value in a loop: they are large (`C5`).

## 6. Error handling and logging

- **Recoverable runtime failures** (missing file, unsupported format, allocation failure, Vulkan error) are logged with `spdlog::error` and the function returns an invalid handle, `false`, an empty `std::optional`, or a default value. The caller checks the result, and a `bool`/`optional` return must not be silently ignored (add `[[nodiscard]]`).
- **Programmer errors** (wrong `ResourceType` passed, out-of-range index into an internal array) use `assert`.
- Log levels:
  - `error`: an operation failed.
  - `warn`: degraded but continuing (limit reached, fallback used).
  - `info`: lifecycle (device selected, engine started or shutting down).
  - `debug`: per-event detail.
- Nothing per frame may log above `debug`.
- Vulkan results always pass through `_to_vk_result_string` (or its successor) in messages.

## 7. Comments

- Comment **why**, not what. Good existing examples are the scalar-layout warnings in `uberMaterialTypes.h`, the `SV_VertexID` lowering note in `rasm.slang`, and the color-space notes in `material.cpp`.
- Every GPU-layout constraint, synchronization decision, and coordinate-convention flip gets a comment at the site.
- `TODO:` comments name the missing piece concretely (`// TODO: support multiple instances`). Long-lived TODOs belong in [development_plan.md](development_plan.md).

## 8. Vulkan and backend code

- All Vulkan calls live under the backend (`src/rhi/vulkan/`). Code outside `src/rhi/` never includes `vulkan.h` or uses `Vk*` types. (`window.cpp` and `renderContext.h` still do today: `D1`/`D2`.)
- Use synchronization2 (`vkCmdPipelineBarrier2`, `VkDependencyInfo`) and dynamic rendering. Do not introduce render passes or framebuffers.
- Every created object gets a debug name, which is a no-op when debug utils is unavailable.
- Check every `VkResult`. Swapchain functions must handle `VK_SUBOPTIMAL_KHR` and `VK_ERROR_OUT_OF_DATE_KHR` explicitly.
- Test new features with the validation layers on (`EngineConfig::enableValidation = true`). A change that introduces validation messages is not done.

## 9. Shaders (Slang)

- Entry points are `vertMain`, `fragMain` and (future) `compMain`, with `[shader("...")]` attributes.
- One responsibility per module: data layout (`core.slang`), lighting and BRDF (`pbr.slang`), pass or entry shaders (`rasm.slang`).
- Shaders output **linear** color. sRGB encoding is done by the attachment format.
- Texture access checks `hasTexture(index)` before sampling, and glTF default values apply when a texture is absent.
- Keep backend-specific code (`spirv_asm`, `vk::` attributes, raw pointers) behind small helper functions in one module, so that other targets can provide alternatives (`D12`).

## 10. Commits

- Short imperative subject. Use a prefix when it helps: `feat:`, `fix:`, `refactor:`, `docs:`, `build:`.
- Keep a change that alters a GPU struct, its Slang mirror and its asserts in one commit.
- Do not mix formatting-only changes with behaviour changes.
