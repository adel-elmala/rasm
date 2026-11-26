# Contributing to RASM

Thank you for your interest in RASM. It is an early-stage project with one maintainer, so every bug report, test run and pull request makes a real difference.

Please read and follow the [Code of Conduct](CODE_OF_CONDUCT.md). For questions, use [GitHub Discussions](https://github.com/adel-elmala/rasm/discussions) rather than issues.

## Ways to help

You do not need to write engine code to help.

- **Run the examples on your machine and tell us how it went.** RASM is built and tested on Windows with an NVIDIA GPU, and on macOS through MoltenVK. Linux has never been tested, and neither have AMD or Intel GPUs. A report that says "it works on my Radeon on Fedora" is just as useful as one that says "it crashes". Use the [platform report](https://github.com/adel-elmala/rasm/issues/new?template=platform_report.yml) form.
- **Report bugs** with the [bug report](https://github.com/adel-elmala/rasm/issues/new?template=bug_report.yml) form. The validation layer output is the most useful part.
- **Improve the docs.** The guides and the API reference in [`docs/`](docs) are plain HTML. If something was hard to follow, a fix or an issue that says where you got lost helps the next reader.
- **Write bindings.** The C API ([`include/rasm/rasm_c.h`](include/rasm/rasm_c.h)) is a stable ABI made for other languages. [`tests/c_api/ctypes_smoke.py`](tests/c_api/ctypes_smoke.py) drives it from Python with no compiler, and is a good starting point for Python, C#, Rust or Zig bindings.
- **Pick an issue.** Issues labelled [`good first issue`](https://github.com/adel-elmala/rasm/labels/good%20first%20issue) are small and self-contained, and [`help wanted`](https://github.com/adel-elmala/rasm/labels/help%20wanted) marks bigger ones where outside help is welcome. Leave a comment before you start, so two people do not do the same work.

## Building

The short version, on Windows (use the `macOS` preset on macOS):

```sh
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

You need CMake 3.23 or newer, Ninja, Clang, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, and a Vulkan 1.4 driver. The [build guide](docs/build_guide.md) covers each platform, the build options, and the analysis builds (clang-tidy, sanitizers).

There is no Linux preset yet. If you want to try Linux, configure by hand with Ninja, `clang`/`clang++` and the vcpkg toolchain file, and please report what happened.

## Before you change code

RASM has a small set of written rules. Please read the ones that apply to your change instead of guessing:

| Read | For |
|---|---|
| [`CLAUDE.md`: Code rules](CLAUDE.md#code-rules) | Keep it simple: no templates, one `.h`/`.cpp` pair per part, file-local helpers in unnamed namespaces |
| [`CLAUDE.md`: Invariants](CLAUDE.md#invariants-that-are-easy-to-break) | The rules that are easy to break: CPU/GPU struct layout, double-precision positions, frame slots, the stable C ABI |
| [`docs/coding_style.md`](docs/coding_style.md) | Naming, headers (`// clang-format off` and hand-aligned columns), error handling, Vulkan and Slang rules |
| [`docs/architecture.md`](docs/architecture.md) | How the engine works today |
| [`docs/code_review.md`](docs/code_review.md) | Known bugs and design issues, each with an ID such as `B26` or `D44`. Check it before you "fix" odd behaviour: it may be known, with a reason |
| [`docs/development_plan.md`](docs/development_plan.md) | The roadmap and its open items |

`CLAUDE.md` is written for the AI coding assistant the maintainer uses, but its rules apply to every contributor.

### Discuss big changes first

Open an issue or a discussion before you start on a new feature, a new abstraction or a refactor that touches many files. The project prefers the least code that solves the problem in front of it, and a short conversation up front can save you a lot of rework. Small fixes, tests and docs changes can go straight to a pull request.

## Before you open a pull request

- [ ] The build is warning-free (every build has `RASM_WARNINGS` on).
- [ ] `clang-format -i` has been run on the `.cpp` files you changed. New headers are wrapped in `// clang-format off` / `// clang-format on` and aligned by hand.
- [ ] The unit tests pass: `ctest --preset x64-debug` (or `macOS`). New CPU-side code has tests in `tests/`.
- [ ] For rendering changes: the examples run with validation on and print no new validation messages. They can run without a window: `02_gltf --headless --frames 120 --out frame.png`.
- [ ] The documentation pages that describe the code you changed are updated, and `python .claude/skills/sync-docs/scripts/docsync.py check` passes. A new public function or type gets its entry in the API reference (`docs/api/`).
- [ ] If your change fixes or touches an item of `docs/code_review.md`, the item is updated and its ID is in the commit message.

CI builds and tests every pull request on Windows and macOS, and on Windows it also renders a set of scenes on a software GPU (lavapipe) and compares them with reference images. If your change alters one of those images on purpose, say why in the pull request and show the image before and after.

### If you use Claude Code

The repository ships [Claude Code](https://claude.com/claude-code) skills in [`.claude/skills/`](.claude/skills). The ones most useful to contributors:

- `verify-change` builds, runs the tests, runs every example headless with validation, and compares screenshots with a baseline taken before your change.
- `sync-docs` finds the doc pages your change made stale and checks them.
- `finish-task` runs the checks above and drafts a commit message.

They are optional: every check they run can also be run by hand.

## Commit messages

Look at `git log --oneline` for the style. A short subject with a prefix, in lower case:

```
feat: orthographic pan and zoom, turntable/arcball trackball, reference grids
fix: drape roads across their width, show labels whole by their anchor (B35, B36)
perf: staged pools in device-local-only memory, no first-frame stall (P35)
docs: weighted blended transparency guide
```

Use `feat:`, `fix:`, `perf:`, `refactor:`, `docs:`, `build:`, `test:` or `chore:`. Put the IDs of the `docs/code_review.md` items you fixed or touched at the end, in parentheses. Keep formatting-only changes in their own commit. Pull requests are squash-merged, so the pull request title becomes the commit subject on `main`.

## Review

RASM has one maintainer, who reviews in their spare time. You can usually expect a first response within a week. If you hear nothing after two weeks, a friendly ping on the pull request is welcome.

Review comments are about the code and the project's rules, never about you. A pull request may be closed if it goes against the design in a way that cannot be fixed easily; when that happens you will get the reason.

## License

RASM is released under the [MIT License](LICENSE). By contributing, you agree that your contribution is licensed under the same license (inbound = outbound). There is no Contributor License Agreement to sign.

Only submit code you have the right to contribute. Third-party code or assets need a license compatible with MIT, and must be named in the pull request.
