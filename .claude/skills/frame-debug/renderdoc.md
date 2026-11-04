# RenderDoc (Windows, Linux)

`scripts/rdc.py` runs with normal Python 3. It re-launches itself inside `qrenderdoc --python`
(the only place the `renderdoc` module exists), then prints the result. Every command opens the
capture again, which takes a few seconds for a 200 MB `.rdc`.

qrenderdoc is found via `RENDERDOC_DIR`, then `C:\Program Files\RenderDoc`, then `PATH`.
No engine change is needed: the script injects into the exe and queues the capture of frame N.

```sh
R=.claude/skills/frame-debug/scripts/rdc.py
python $R capture build/x64-debug/examples/02_gltf.exe --frame 60 --out <scratch>/f.rdc
```

The app is killed after the capture. Arguments after `--` go to the app.

## Commands

| Command | Shows |
|---|---|
| `actions <rdc> [--all]` | Event list: passes, draws (index/instance counts), dispatches, clears, outputs. Event IDs (`eid`) are used by the other commands. |
| `resources <rdc> [filter]` | Textures (size, mips, format) and buffers (size, **GPU address**), with debug names and ids. |
| `state <rdc> <eid>` | Shaders + entry points, used descriptors per stage, color/depth targets, cull/front face, depth test/write/func, blend, viewport, push constants (resolved to buffer + offset). |
| `targets <rdc> <eid> <outdir>` | Saves color targets + depth (depth range auto-stretched to gray) as PNG **after** that event. |
| `texture <rdc> <name\|id> <out.png> [--mip N] [--slice N]` | Saves any texture at end of frame. |
| `buffer <rdc> <name\|id\|0xADDR> [--offset B] [--size B] [--format F] [--rows N]` | Decodes a buffer. `F` = `u32` (default), `i32`, `f32`, `u64`, `hex`, or a struct row like `"u32,u32,u32,i32,u32"` / `"f32x4,u32"`. u64 values that are GPU addresses are printed as `= <buffer> + offset`. A `0x…` address selects the buffer and offset for you. |
| `pixel <rdc> <x> <y> [--eid E] [--target I]` | Pixel history up to E (default: last draw): each event that touched the pixel, PASS/FAIL with reason (depthTestFailed, backfaceCulled, scissorClipped, shaderDiscarded, ...), color before/shader/after, depth before→after. |
| `disasm <rdc> <eid> <vs\|ps\|cs\|ms\|ts>` | SPIR-V disassembly of the bound shader. |

## Recipes

- **Black / wrong screen**: `actions` → `targets` at each pass end → Read PNGs to find the first bad pass → `state` on its draw.
- **Object missing**: `buffer "Draw Info Buffer" --format "u32,u32,u32,i32,u32"`, then check indexCount/instanceCount are non-zero. `pixel` where it should be: no event means it never rasterized (check transforms in `Mega Model data Buffer`, the camera in `SceneData`); FAIL means a test rejected it.
- **Wrong material / texture**: `state <eid>` lists the bindless elements the pixel shader really read. `texture` saves one to compare.
- **Follow GPU pointers**: `state` → push constant address → `buffer 0x<addr> --offset 528 --format u64x3` (SceneData pointers) → `buffer <that buffer> --format ...`.
- **Struct mismatch**: dump the buffer with the C++ layout and check that values look sane (no NaN, no huge ints).
