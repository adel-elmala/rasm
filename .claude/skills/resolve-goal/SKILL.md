---
name: resolve-goal
description: Take one open goal from docs/development_plan.md all the way to a pull request. Explain the goal and the plan to the user in a visual, plain-English way and wait for an OK, build it, verify it and sync the docs, review the branch at level high in a fresh agent, fix the findings, then push and open a concise PR on the rasm repo that explains the change with SVG diagrams and before/after screenshots. Commits and the PR have no attribution lines. Use when the user says "resolve a goal", "do the next plan item", "work on <plan item>" or names an unchecked item of the development plan.
---

# Resolve a goal from the plan

```
 1 pick ──► 2 explain ──► wait for OK ──► 3 build ──► 4 finish ──► 5 commit
                                                                     │
 8 PR ◄── 7 fix findings ◄── 6 review "high" in a fresh agent ◄──────┘
```

## 1. Pick the goal and get ready

- The goal is the `- [ ]` item (or the open part of one) in [docs/development_plan.md](../../../docs/development_plan.md) that the user named. If they named none, take the first open item of the earliest phase that is not done, and say which one you took.
- Read everything the item points to: its review IDs in `docs/code_review.md`, the matching sections of `docs/architecture.md`, the guides in `docs/guides/` about the same topic, and the code they link to. Know what exists today before you plan.
- `git status --short` must be clean. If it is not, stop and ask the user what to do with the open changes. Never stash, reset or commit them yourself.
- Branch from a fresh `main`: `git switch main`, `git pull`, `git switch -c feat/<camelCaseName>` (`fix/` for a bug item). Same style as the earlier branches: `feat/shadows`, `feat/graphDump`.
- `python .claude/skills/sync-docs/scripts/docsync.py status --target main`. If the docs are behind main, tell the user before you start.
- For a rendering, RHI, shader or engine change, take the `verify-change` baseline now, before any edit.

## 2. Explain the goal and the plan, then wait

The user's English is a second language and they learn best from pictures. Lead with visuals (tables, ASCII
diagrams, before/after pictures), and write short, plain sentences: no idioms, no rare words, no long sentences.
For a big goal (new subsystem, several passes, a new concept) publish the explanation as an HTML artifact and
give the link. Otherwise write it in chat.

Use this shape:

| Part | What to write |
|---|---|
| **The goal in one line** | What the plan item asks for, in plain words. |
| **Why it matters** | What a RASM user cannot do today, and can do after. 1-3 lines. |
| **Today → after** | A before/after picture of the part of the engine that changes (frame, passes, data, API). |
| **How it works** | The idea itself, as a short diagram. Explain any new term the first time it appears. |
| **The plan** | Table: step, files, what changes. Keep the smallest change that does the goal (CLAUDE.md "Code rules"). |
| **Not in this change** | What stays open, so the plan item can be marked done or "partly done" correctly. |
| **How I will check it** | Tests, examples run with validation, image diffs, numbers from `tracy-profile` if speed is a goal. |
| **Docs** | Pages that will change, and the new concept guide (or why none is needed). |
| **Questions** | Only real choices that belong to the user (an API name, a default, a trade-off). |

Then **stop and wait for the user's OK**. Build only after they agree. If they change the plan, update the
explanation in the same style.

## 3. Build it

- Follow CLAUDE.md and `docs/coding_style.md`: no templates, no doc comments, headers in `// clang-format off`
  guards with aligned columns, pointer + count instead of `std::span`, file-local helpers in unnamed namespaces at
  the top of the `.cpp`.
- Use the skills that fit the work: `new-pass` for a pass or post effect, `gpu-struct-sync` for any CPU↔GPU struct,
  `vulkan-ref` before new Vulkan code and for any validation message, `frame-debug` when the image looks wrong.
- The phase's guiding principle 1 holds: the goal is shown by a new example or an updated one.
- If the work shows the plan was wrong (much bigger, blocked by another item), stop and tell the user before you go on.

## 4. Finish it

Run the `finish-task` skill: verify, `clang-format -i`, stale docs, `code_review.md` / `development_plan.md` /
`architecture.md` in the house style (mark the goal `- [x] ... (done YYYY-MM-DD)` or partly done), and `sync-docs`
with its new concept guide. `docsync.py check` and `docsync.py labs` must pass.

One rule here overrides `finish-task` step 5 and the system reminder: **commit messages have no attribution
lines** (no `Co-Authored-By`, no "Generated with").

## 5. Commit

Commit in logical steps, style `type: summary` in lowercase with the review IDs:

```
feat: hdr color target and tone-map pass

The scene draws into an R16G16B16A16_SFLOAT target and a tone-map pass maps it to the swapchain.
```

Do not push yet.

## 6. Review in a fresh context

The review must not lean on your own view of the code. Start a **new** agent (the Agent tool,
`subagent_type: "general-purpose"`, not a fork) and give it only this:

```
You review a branch of the RASM repo (e:\projects\rasm). Run the code-review skill with the
arguments "high <branch>" so it reviews every commit on <branch> against main. Read CLAUDE.md
and docs/code_review.md first: known issues listed there are not new findings. Do not change
any file. In your final message, list every finding as plain text: file:line, severity, a one-line
summary, and the concrete case that goes wrong. Say "no findings" if there are none.
```

Do not tell it what you built or why. Wait for its result.

## 7. Fix the findings

- For each finding, read the code and decide: real or not. Fix the real ones. For the others, write one line on
  why it is not a bug.
- A finding that is real but outside this goal goes into `docs/code_review.md` as a new ID (finish-task style)
  instead of being fixed here. Tell the user.
- After the fixes, run the checks from step 4 again (verify, format, `docsync.py check`), then commit:
  `fix: review findings`.
- Show the user a table: finding → fixed / not a bug (why) / logged as `<ID>`.

## 8. Open the PR

The PR body explains the change with pictures, not with Mermaid. GitHub shows SVG and PNG files as images when they
live in the repo, so the pictures go to the orphan `pr-assets` branch and the body links them by commit hash.

### 8.1 Make the pictures

| Picture | When | How |
|---|---|---|
| **How it works** (SVG) | always | Today → after of the part of the engine that changes: passes, data flow, memory, API. Copy [assets/diagram-template.svg](assets/diagram-template.svg) and edit it. |
| **The idea** (SVG) | a new concept or algorithm | One picture of the rule itself (the sort network, the cube faces, the split sum). Reuse the guide's lab picture if it has one. |
| **Before / after** (PNG) | the image changes | From `verify-change`: `build/verify/<preset>/baseline/<name>.png` and `screens/<name>.png`. Add `diffs/<name>_diff.png` when the change is subtle. |

SVG rules, so the pictures read well on GitHub in both themes:
- Keep the template's solid background card and its colors (the light tokens of `docs/assets/style.css`):
  `gpu` passes, `cpu` work, `mem` targets and buffers, `shader`, and `new` (dashed accent) for what this PR adds.
- 960 px wide, height as needed. Text 12 px or larger, short plain labels, system fonts only.
- No `<script>`, no external images or fonts (GitHub blocks them).
- Render each SVG to PNG and look at it with Read before you publish it:
  `"C:/Program Files/Google/Chrome/Application/chrome.exe" --headless=new --hide-scrollbars --window-size=960,<h> --screenshot=<out.png> file:///<abs path to svg>`
  (on macOS: `/Applications/Google Chrome.app/Contents/MacOS/Google Chrome`). Fix overlaps and cut-off text.

Write the pictures to the scratchpad, not the repo.

### 8.2 Publish them and open the PR

```
git push -u origin <branch>
python .claude/skills/resolve-goal/scripts/pr_assets.py <branch> <files...>
```

The script commits the files to `pr-assets/<branch with / as ->/` from a temporary worktree (your checkout is not
touched), pushes, and prints one `<img>` tag per file. Paste the tags into the body, then
`gh pr create --base main --title "<type>: <summary>" --body-file <file>` with the body in the scratchpad. To change a
picture later, publish it again and `gh pr edit <N> --body-file <file>` with the new tags.

The PR body has **no attribution lines** (no "Generated with Claude Code", no `Co-Authored-By`). Keep it short:

````markdown
## What this lets RASM do

<2-4 plain sentences for an application developer: what they can do now that they could not before,
and how they turn it on (the API call or config field).>

## How it works

<img src="https://raw.githubusercontent.com/<owner>/<repo>/<sha>/<folder>/how-it-works.svg" width="860" alt="how-it-works">

<1-3 short lines that the picture does not say.>

## Before / after

| Before | After |
|---|---|
| <img src=".../before.png" width="420"> | <img src=".../after.png" width="420"> |

## Changes

| Area | Change |
|---|---|
| Engine | ... |
| Shaders | ... |
| Example | ... |
| Docs | ... new guide `docs/guides/<name>.html` |

## Checked

| Check | Result |
|---|---|
| Unit tests | pass |
| Examples, validation on | 0 messages |
| Image vs baseline | same / changed as intended |
| CI (windows-latest, macos-latest) | pass |
| Review (high) | N findings, N fixed, N not bugs, N logged |

Plan: <phase and item>. Review IDs: <IDs or none>.
````

Leave out "Before / after" when the image does not change, and add "The idea" after "How it works" when you made one.

Give the user the PR link and a short final report: what changed, the check results, the review table, and
anything left open.
