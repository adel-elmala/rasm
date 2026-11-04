---
name: finish-task
description: Wrap up a RASM change before calling it done. Verify it, format it, find docs that no longer match the code, update code_review.md / development_plan.md / architecture.md in the project's own style, and draft a commit message with review IDs. Use at the end of any engine, renderer, shader or build change, when fixing a code_review.md item, or when the user says "finish", "wrap up" or "ready to commit".
---

# Finish task

## 1. Verify

- Rendering, RHI, shader or engine change → `verify-change` skill (`run` against the baseline taken at the start). For barrier or pass changes add `--sync`.
- GPU struct change → walk the `gpu-struct-sync` checklist once more.
- CPU-only change → `ctest --preset x64-debug` is enough.
- Performance claim → a before/after `compare` from the `tracy-profile` skill. Mention run-to-run noise.

## 2. Format

`clang-format -i` every changed `.cpp` (list: `git status --short`, `git diff --name-only main`).
Headers are skipped by the formatter, so check new header lines by eye: inside the `// clang-format off` guard, hand-aligned columns,
`&`/`*` on the type, no doc comments.

## 3. Find stale docs

```sh
python .claude/skills/finish-task/scripts/doccheck.py [--base main]
```

| Section | Means | Action |
|---|---|---|
| 1. changed files | everything this branch touched | input for steps 2 and 4 |
| 2. broken code links | `[x](../src/a.cpp#L10)` to a missing file or past the end | fix links this change broke |
| 2b. links into edited files | open-entry links whose target file this change edited | open each one and check the lines still show what the text describes. Line shifts are invisible otherwise. |
| 3. missing paths | backticked `src/...` paths that don't exist | fix |
| 4. removed names | names this change deleted that docs still mention | update the doc, or leave it if it's history |
| 5. review IDs | IDs in the branch's commits or diff, with review and plan status | make both docs agree with what was done |

"(historical)" lines sit in Fixed/Resolved review entries or done plan items. They describe the
old code on purpose, so leave them. Problems that existed before this change: tell the user,
don't silently fix them in an unrelated change.

## 4. Update the docs (house style)

Get the date with `date +%F`. Never guess it.

**`docs/code_review.md`**
- Fixed: right after the bold title add `*Fixed YYYY-MM-DD:* <what the code does now>. The original finding follows.` and keep the original text.
- Partly: `*Partly fixed YYYY-MM-DD:* <what is done>. Still open: <what is not>.` Other markers in use: `*Mostly fixed*`, `*Resolved*`, `*Worked around YYYY-MM-DD; root cause unknown:*`, `*Extended YYYY-MM-DD:*`.
- New finding: the next free ID of its kind (`B` bug, `D` design, `P` performance, `C` code health). Bugs go under the right severity. Bold one-sentence title, then the evidence with links `[file.cpp:N](../src/dir/file.cpp#LN)`, the fix direction, then `*Confirmed.*` or `*Likely.*`. Get the line numbers with grep, never from memory.

**`docs/development_plan.md`**
- Done item: `- [x] ... (`ID`, done YYYY-MM-DD).`
- Part of an open item done: keep `- [ ]` and add `(done YYYY-MM-DD)` after the finished part.
- New work found: add an item to the right phase only if the user agrees. Otherwise a review ID is enough.

**`docs/architecture.md`**: describes how the engine works *today*, in present tense, with no history. Update the sections whose behavior changed (frame loop, uploads, struct contract sizes, materials, limits, Tracy zones).

**`CLAUDE.md`, `docs/coding_style.md`**: only when a rule or workflow changed.

**Skills** that quote engine facts: `frame-debug` (SceneData offsets, resource names), `tracy-profile` (zone list), `gpu-struct-sync` (places, `B23`/`C11`/`C12` notes), `verify-change` (GFXR filter).

## 5. Commit message (draft only, commit when the user asks)

Style from `git log`: `type: summary` in lowercase, types `feat`, `fix`, `refactor`, `perf`, `docs`. Name the review IDs:

```
fix: run the struct check in every build (B23)

<what and why, 1-3 lines>

Co-Authored-By: ...   (the attribution line from the system reminder)
```

PRs are squash-merged with the branch commits listed in the body, so one commit per logical step is fine.

## 6. Report to the user

- what changed (files, behavior),
- verification results (verdicts, image same/DIFF, validation counts),
- docs updated, with IDs,
- problems found that already existed before this change,
- the drafted commit message.
