## What

<!-- What does this change do? One or two sentences, plus pictures for anything visual. -->

## Why

<!-- The problem it solves. Link the issue: "Fixes #123". -->

## Review items

<!-- The IDs of the docs/code_review.md items this fixes or touches (B26, D44, ...), and the development_plan.md item if any. Write "none" if none. -->

## How it was tested

<!-- Platform, GPU, preset, which examples you ran. For a rendering change, an image before and after. -->

## Checklist

- [ ] The build is warning-free.
- [ ] `clang-format -i` was run on the changed `.cpp` files; new headers have the `// clang-format off` guards and aligned columns.
- [ ] `ctest` passes, and new CPU-side code has tests.
- [ ] For rendering changes: the examples run with validation on and add no validation messages (`--headless --frames N --out frame.png` runs one without a window).
- [ ] The doc pages that describe the changed code are updated, and `python .claude/skills/sync-docs/scripts/docsync.py check` passes.
- [ ] `docs/code_review.md` and `docs/development_plan.md` are updated for the items this changes.
- [ ] A change to `include/rasm/rasm_c.h` only adds at the end and bumps `RASM_C_API_VERSION_MINOR`.
- [ ] If a golden image changed on purpose, the reason and the image before and after are above.

See [CONTRIBUTING.md](https://github.com/adel-elmala/rasm/blob/main/CONTRIBUTING.md) for details.
