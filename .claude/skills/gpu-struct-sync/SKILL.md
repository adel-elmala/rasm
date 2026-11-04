---
name: gpu-struct-sync
description: Change a CPU↔GPU struct in RASM safely — add, remove, reorder or retype a field in SceneData, Modeldata, Material*, Light, DrawInfo, VertexFull, PushConstants, or add a new GPU struct or shared enum/constant. Lists every place that must change together (C++ struct, Slang mirror, static_asserts, validateRasmStructs table, writers, readers, docs), the scalar-layout rules, and how to verify. Use whenever uberMaterialTypes.h or core.slang structs change.
---

# GPU struct sync

Rules: `docs/coding_style.md` ("GPU-visible structs") and `docs/architecture.md` ("CPU/GPU struct contract").
This skill is the procedure. A mismatch corrupts GPU reads silently, or becomes a device lost when a
pointer field moves.

## Every place to touch

| # | Place | What |
|---|---|---|
| 1 | `src/renderer/uberMaterialTypes.h` | The C++ struct (aligned header columns). |
| 2 | `shaders/lib/core.slang` | The Slang mirror: **same field order, matching types** (table below). Names may differ (C++ `Material::pbr` = Slang `metallicRoughness`). |
| 3 | `static_assert`s at the end of `uberMaterialTypes.h` | `sizeof` and every offset after the change point that matters: arrays, pointers, the last field. |
| 4 | `validateRasmStructs` in `src/renderer/shaderCompiler.cpp` | One row per field: `{"<slang name>", offsetof(gfx::X, <c++ name>)}`. A **new struct** needs its own `validateStruct(...)` call, reached through `fieldStruct(...)` from `pc → scene → pointer/field`. |
| 5 | Shared constants and enums | `MAX_LIGHTS` (`include/rasm/light.h`), `MAX_VERTEX_UV_SETS` (`include/rasm/mesh.h`), `VertexFormat`, `MaterialId`, `LightType`: same values in `core.slang`. |
| 6 | `VertexFull` only | `rasm::Vertex` in `include/rasm/mesh.h` and the `static_assert`s in `src/renderer/gpuScene.cpp`. |
| 7 | `PushConstants` only | Its size must stay `PUSH_CONSTANTS_SIZE` (`include/rasm/types.h`), the range of every pipeline layout and the limit `RenderContext::pushConstants` checks. Growing it means raising that constant, and the `new-pass` skill's limits table. |
| 8 | CPU writers | All in `src/renderer/gpuScene.cpp`: `drawScene` (`SceneData`, `Light`), `convertToGfxMaterial` (`Material`), `Modeldata` and `DrawInfo` filling in the upload loop. Fill the new field. Zero-init (`{}`) hides a forgotten one. |
| 9 | GPU readers | `shaders/passes/rasm.slang`, `shaders/lib/pbr.slang`, `shaders/lib/unlit.slang`. |
| 10 | Docs that quote numbers | `docs/architecture.md` (sizes in the struct contract section), `.claude/skills/frame-debug/SKILL.md` (`SceneData` offsets). |

Find more users with: `grep -rn "gfx::<Struct>\|<Struct>\b" src shaders`.

## Type mapping (scalar layout)

Slang is compiled with scalar layout and column-major matrices (`shaderCompiler.cpp`), and glm uses its default (unaligned) packing.
In scalar layout, every type aligns to its largest scalar.

| C++ | Slang | Size | Align |
|---|---|---|---|
| `float` / `uint32_t` / `int32_t` | `float` / `uint32_t` / `int32_t` | 4 | 4 |
| `glm::vec2` / `vec3` / `vec4` | `float2` / `float3` / `float4` | 8 / 12 / 16 | 4 |
| `glm::mat3` / `mat4` | `float3x3` / `float4x4` | 36 / 64 | 4 |
| `uint64_t` (device address) | `uint64_t` or `T*` | 8 | **8** |
| `enum class E : uint32_t` | `enum class E : uint32_t` | 4 | 4 |
| `T a[N]` | `T a[N]` or `T[N] a` | N × size | align of T |
| nested struct | nested struct | its size | largest member align |

- **Never** use `bool` (1 byte in C++, 4 in Slang), `size_t`, `int`/`long`, `double`, `glm::uvec*`/`ivec*` unless both sides agree, or glm's aligned types.
- **64-bit fields are 8-aligned on both sides**, so both add the same hidden padding before them, and a struct containing one rounds its size up to a multiple of 8. Example: `lights` ending at 532 puts `vertsPtr` at 536. Keep 64-bit fields at 8-aligned offsets so the padding is visible in the code.
- **The check compares sizes and offsets, not types (`C11`).** `uint32_t` vs `float`, or swapping two same-size fields, passes every check and still reads garbage. Compare the two definitions field by field by eye.

## Verify

1. **Debug build.** `validateRasmStructs` runs inside `assert(...)`, so Release builds (NDEBUG) skip it completely (`B23`). Once `B23`, `C11` or `C12` is fixed, update this skill.
2. Compile errors from step 3 come first. A failed `static_assert` means your expected offsets are wrong. Recompute them, don't just copy the compiler's numbers.
3. Run the `verify-change` skill. A missed side fails at startup with exact numbers:
   ```
   02_gltf  FAIL  | app exited (code 3) before frame 60
      [error] Size mismatch for SceneData: C++ 552, shader 560
      [error] Offset mismatch for SceneData.lights: C++ 144, shader 148
      [error] Offset mismatch for SceneData.vertsPtr: C++ 528, shader 536
      Assertion failed: validateRasmStructs(...)
   ```
4. If the new field changes rendering, check the image (DIFF) and read the field back with the `frame-debug` skill: `rdc.py buffer "Scene Data Buffer" --offset <off> --format ...`.

Tested on 2026-11-04 by adding `float exposure` after `SceneData::numLights`. The Slang-only change failed as shown above. Steps 1–4 then passed with an identical image.
