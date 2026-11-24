---
name: verify-change
description: Verify a RASM code or shader change end to end. Build, unit tests, then run every example for N frames with Vulkan validation, collect validation/engine errors, screenshot frame N and pixel-diff it against a baseline taken before the change. Use before calling any engine, renderer, RHI or shader change done, for refactors that must not change the image, and to check a visual change looks as intended.
---

# Verify change

One command checks everything CLAUDE.md asks for ("a change that adds validation messages is
not done") plus image regressions. Every example runs **headless**, so no window opens: it gets
`--headless --frames N --out <png>` (`examples/exampleArgs.h`), renders N frames into an offscreen swapchain with
validation on, writes frame N through `Renderer::readFrame` and exits. The UI is drawn headless too (with no input),
so the RASM window and an example's own ImGui windows are in the screenshot.

`--windowed` runs the examples in a window under the GFXReconstruct layer (Vulkan SDK) instead: the layer quits each
one after frame N, and its replay renders that frame to PNG. Use it only for captures (`--keep-captures`, for
`frame-debug`); it opens a window per example. `16_headless` (`HEADLESS_EXAMPLES` in the script) runs headless
either way.

```sh
V=.claude/skills/verify-change/scripts/verify.py
python $V baseline      # BEFORE editing: build, tests, examples → saves screenshots as baseline
# ... make the change ...
python $V run           # AFTER: same checks + pixel diff against the baseline
```

## Options

| Option | Use |
|---|---|
| `--preset x64-debug` | Build preset (also `x64-release`, `macOS`, `macOS-release`, and the sanitizer presets `x64-asan`, `macOS-asan`, `macOS-tsan`). Each preset has its own baseline. |
| `--examples 00_hello_scene,02_gltf` | Only these (default: every `rasm_add_example` in `examples/CMakeLists.txt`). |
| `--frame 60` | Frame to screenshot. Keep it the same for baseline and run. |
| `--tolerance 2` | Max per-channel difference (0-255) still counted as "same". |
| `--no-build`, `--no-tests` | Skip steps you just ran. |
| `--sync` | Synchronization validation: barriers, hazards. Use it for any barrier, layout, pass-order or render-graph change. |
| `--best-practices` | Best Practices warnings. Noisy, so compare the counts against a baseline run with the same flag. |
| `--gpu-av` | GPU-assisted validation: out-of-bounds bindless indices and buffer device address reads in shaders. Slow. |
| `--args "--debug-view normals"` | Extra arguments for every example, after `--fixed-step --wait-for-loads` (an example ignores the ones it does not know). Compare such a run against a baseline taken with the same arguments. |
| `--windowed` | Run in a window under GFXR instead of headless (opens a window per example). |
| `--keep-captures` | With `--windowed`: keep the `.gfxr` captures (≈100 MB each) for `frame-debug`. |
| `--images` | Also run the golden-image tests (`tools/image_tests.py --lavapipe --preset x64-release --build`, `run` only, whatever `--preset` is). **It downloads (once) and runs a third-party Mesa build, lavapipe**, so pass it only when the user asked for it. The goldens are lavapipe's; this machine's GPU is checked by the baseline diff above, not by them. |

`diff <a.png> <b.png> [--out d.png]` compares any two PNGs.

## Output

```
build   OK  (0 unique warnings)
tests   OK  100% tests passed, 0 tests failed out of 1
examples at frame 60, preset x64-debug, headless:
  00_hello_scene   OK    validation/engine: 0 errors, 0 warnings  | image same (max diff 0)
  02_gltf          DIFF  validation/engine: 0 errors, 0 warnings  | image CHANGED 1.443% px, max diff 139, bbox (89, 190, 624, 336)
      [WARNING Validation] x9 BestPractices-...   (grouped by VUID / message id, with count)
```

| Verdict | Meaning | Exit code |
|---|---|---|
| OK | no errors or warnings, image same (or no baseline) | 0 |
| WARN | warnings only | 0 |
| DIFF | image differs from the baseline | 1 |
| FAIL | validation/engine error, assert, crash, never reached frame N, or no screenshot | 1 |

Files in `build/verify/<preset>/` (gitignored): `screens/`, `diffs/<name>_diff.png`
(gray = same, orange = small change, red = big change), `logs/<name>.log` (full app output), `baseline/`.
Look at the diff and the new screenshot with Read.

## Rules

- **Take the baseline first**, on the unchanged code. If you forgot and the work is uncommitted, ask the user before `git stash` to make one.
- **DIFF is not automatically bad.** For a refactor it is a bug. For a visual change, look at the screenshot and the diff and judge it. Tell the user what changed and where (bbox), then run `baseline` again once they accept it.
- **Any new error or warning means not done.** If a warning was already in the baseline, say so instead of hiding it.
- Unexpected DIFF or FAIL with no obvious cause: use the `frame-debug` skill (`--keep-captures` gives you the capture).
- Baselines depend on the machine, GPU and driver. They are local and never committed. The golden images in
  `tests/golden/lavapipe/` are the committed ones: they belong to lavapipe, and CI checks them on every PR
  (`docs/guides/image-tests.html`).
- With `--windowed`, messages about `vkCmdPipelineBarrier()` (the original, not `...2`) come from GFXR's frame snapshot, not RASM, which only uses `vkCmdPipelineBarrier2`. The script counts them as "ignored". If a message looks like it came from the capture layer, run the same example headless before blaming RASM.
- Run one example by hand the same way: `build/x64-debug/examples/<name>.exe --fixed-step --wait-for-loads --headless --frames 60 --out shot.png`.

## Determinism

Frame N is pixel-exact between runs (tested: max diff 0) because the examples rotate objects by
a fixed angle per frame, not by elapsed time, the camera has no input, `--fixed-step` sets
`EngineConfig::fixedDeltaTime` to 1/60 s (the cameras' easing, the UI and the stats' refresh step by it, so the RASM
window and the stats overlay renew their numbers on the same frames: `C15`), and with `--wait-for-loads`
(`examples/exampleArgs.h`) every example that loads calls `Renderer::waitForLoads` before its first frame, so
frame N does not depend on how fast the loads ran (`C16`). The camera eases toward its target by `deltaTime`, but snaps to it once it is within float precision (`B24`), so a camera at rest does not drift. A DIFF only on equal-depth edges, different from run to run, points at something time-based like that. If an example starts
animating by time, its diffs become noise. Make the animation per-frame, or exclude that example
with `--examples`. `05_city` animates by wall clock, but the script runs every example with `--fixed-step`, which makes it
advance a fixed 1/60 s per frame. The stats' timings (ms, fps, VRAM) still change from run to run: a DIFF only in the
numbers of the RASM window or the stats overlay is that.
