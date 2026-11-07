---
name: sync-docs
description: Keep RASM's HTML docs (docs/index.html, the visual guides in docs/guides/, the API reference in docs/api/) in sync with the code on main. Finds the pages a code change made stale, checks names/paths/links/API coverage, updates the pages in their visual style, and moves the sync stamp. Use after any change to include/, src/, shaders/, examples/ or cmake/ (finish-task runs it), when the docs stamp is behind main, when adding a guide or an API page, or when the user says "sync docs" / "update the guides".
---

# Sync docs

The HTML docs must describe the code on `main`. `docs/assets/sync.js` holds the stamp: the commit whose code the
pages describe (shown at the bottom of every page). Each page lists the files it describes in
`<meta name="rasm-sources" content="...">`.

```sh
python .claude/skills/sync-docs/scripts/docsync.py status [--target main]   # what changed, which pages
python .claude/skills/sync-docs/scripts/docsync.py check                    # names, paths, links, API coverage
python .claude/skills/sync-docs/scripts/docsync.py stamp [--rev HEAD]       # docs now match --rev
```

## 1. Find what is stale

- **On a feature branch** (normal case, from `finish-task`): run `status --target HEAD`. It diffs the stamp's tree
  against your branch, so it lists the code your branch changed (plus anything main changed since the last sync).
- **Catching up main**: run `status` (target `main`). Use it when a session starts with the stamp behind main.
- If the stamp commit is missing (a deleted branch), review the pages whose sources changed in `git log`.

`status` prints, for each changed file, the pages that list it. Files under "no page lists" are either a new topic
(add it to the closest page's `rasm-sources`, or write a new guide) or something no page needs (tests, cmake
helpers): say which.

## 2. Update each stale page

Read the diff of each changed source (`git diff <stamp> <target> -- <file>`) and the page. Then:

- Fix every sentence, table, number and **diagram** that no longer matches. Diagrams are the main content: a
  renamed buffer, a new step or a new pointer must appear in the SVG too, not only in the text.
- New behaviour gets a picture, not a paragraph: extend the existing diagram or add one.
- New public function, type or constant → its `.api` block on the right API page (signature copied from the
  header), and its row in the `engine.html` index table if it is an `Engine` method.
- A removed name must disappear from every page (`check` finds them).
- A new topic that deserves its own guide: ask the user first. Then add the page to `SECTIONS` in
  `docs/assets/site.js` (the only list of pages) and link it from `docs/index.html`.
- Keep `docs/architecture.md` in step through `finish-task`; this skill owns the HTML pages only.

### House style (keep every page consistent)

- Copy the structure of an existing page (`docs/guides/culling.html` for guides, any `docs/api/` page for the
  reference): same `<head>`, shared arrowhead `<defs>`, `layout`/`sidebar`/`main`, eyebrow, `h1`, `lead`.
- Visual first; short, plain sentences (the main reader's English is a second language). Present tense, today's
  code only, no history. Known issues by ID from `docs/code_review.md`.
- Only classes from `docs/assets/style.css`; no inline styles, no `<style>`/`<script>` inside SVGs, no libraries.
  Colors keep their meaning: `cpu` blue, `gpu` green, `mem` amber (buffers), `shader` purple.
- Every identifier in `<code>` must exist in the code; never invent names or numbers. Example values that are not
  repo names (an asset path in a snippet, a user's file name) go in `<samp>`, which `check` does not test.

## 3. Check

```sh
python .claude/skills/sync-docs/scripts/docsync.py check
```

Fix everything it reports. Then screenshot each page you edited and read the image; fix text that overflows
its box or crosses an arrow:

```sh
"/c/Program Files/Google/Chrome/Application/chrome.exe" --headless=new --disable-gpu --hide-scrollbars \
  --window-size=1000,4000 --screenshot="<scratchpad>/page.png" "file:///E:/projects/rasm/docs/guides/page.html"
```

Use a smaller window (900x1600) to see diagram details; the image is shown downscaled. On macOS use
`/Applications/Google Chrome.app/Contents/MacOS/Google Chrome` with the same flags.

## 4. Stamp

Only when every page in the `status` list was reviewed and `check` passes:

- on a branch: `stamp --rev HEAD` (the branch's code is what main will get),
- catching up main: `stamp --rev main`.

Commit the page changes and `docs/assets/sync.js` together, `docs: ...` type, or inside the feature's own commit.

## 5. Report

Pages updated (what changed in each), new pages or API blocks, files no page covers, and the new stamp.
