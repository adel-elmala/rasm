---
name: verify-change
description: Verify a RASM code or shader change end to end. Build, unit tests, then run every example for N frames with Vulkan validation, collect validation/engine errors, screenshot frame N and pixel-diff it against a baseline taken before the change. Use before calling any engine, renderer, RHI or shader change done, for refactors that must not change the image, and to check a visual change looks as intended.
---

# Verify change

One command checks everything CLAUDE.md asks for ("a change that adds validation messages is
not done") plus image regressions. No engine change is needed: the GFXReconstruct layer (Vulkan SDK)
quits each example after frame N, and its replay renders that frame to PNG.

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
| `--keep-captures` | Keep the `.gfxr` captures (≈100 MB each) for `frame-debug`. |

`diff <a.png> <b.png> [--out d.png]` compares any two PNGs.

## Output

```
build   OK  (0 unique warnings)
tests   OK  100% tests passed, 0 tests failed out of 1
examples at frame 60, preset x64-debug:
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
- Baselines depend on the machine, GPU and driver. They are local and never committed.
- Messages about `vkCmdPipelineBarrier()` (the original, not `...2`) come from GFXR's frame snapshot, not RASM, which only uses `vkCmdPipelineBarrier2`. The script counts them as "ignored". If a message looks like it came from the capture layer, run the exe once without the layer before blaming RASM (`timeout 10 <exe>` with the same `VK_LAYER_VALIDATE_*` variables).

## Determinism

Frame N is pixel-exact between runs (tested: max diff 0) because the examples rotate objects by
a fixed angle per frame, not by elapsed time, and the camera has no input. The camera eases toward its target by `deltaTime`, but snaps to it once it is within float precision (`B24`), so a camera at rest does not drift. A DIFF only on equal-depth edges, different from run to run, points at something time-based like that. If an example starts
animating by time, its diffs become noise. Make the animation per-frame, or exclude that example
with `--examples`. `05_city` animates by wall clock, but the script runs every example with `--fixed-step`, which makes it
advance a fixed 1/60 s per frame.
