# Visual style of the guides

The guides teach with pictures. A reader should get the idea of a section from its picture alone; the text
around it only names things and gives the numbers. `docs/guides/shadow-mapping.html` is the model page: copy
its patterns.

## Which block for which content

| The section has... | Use | Not |
|---|---|---|
| a spatial or geometric idea (rays, surfaces, frustums, texels, memory layouts) | a `figure.diagram` that draws the real thing: the scene from the side or from above, the bytes, the grid | boxes with words in them |
| 2 to 6 variants, options, fixes, kinds | `.vgrid` of `.vcard`s: a small drawing each, a title, one or two lines, an optional `.cost` line | a table with a paragraph per cell |
| wrong against right, before against after | `.split` with `class="bad"` / `class="good"` columns, each with a small SVG; or a `.vgrid` with `.vcard.bad` / `.vcard.good` | two paragraphs |
| a short chain of steps (3 to 6) with no branching | `ol.flow` (add `cpu` / `gpu` / `mem` / `shader` to an item for its color) | a sentence with arrows |
| an idea with a parameter the reader can change (an angle, a distance, a size, a bias, a count), or a rule that sorts things (in / out, lit / dark, kept / culled) | a **lab**: a `figure.lab` the reader drags, slides or clicks (see "Labs" below), next to the static picture that names the parts | a table of cases, or three near-identical static pictures |
| a process that changes one picture over time | `.stepper` with one `.step` per state, each with its own SVG and one `<p>` | one crowded diagram, or a numbered list |
| numbers from a worked example | `.stats` tiles (`<b>` value, `<span>` meaning, `<small>` how it is computed) | a two-column table |
| a formula | `.formula` (`p.eq` lines, then a `dl` naming each term), or `<pre><code class="lang-text">` for several aligned lines | a formula inside a sentence |
| symptoms and their causes | `dl.pitfalls` (`<div><dt>what you see</dt><dd>why, and the fix</dd></div>`) | a table |
| a rule the reader must not miss | `.callout`, `.callout.warn`, `.callout.rule` with a `<strong>` title | bold text in a paragraph |
| reference data with 3+ columns, code maps, "This page / RASM" | a table inside `.table-wrap` | — |

A plain `.vgrid` fits as many `.vcard`s in a row as the window allows: four in the full page column, three
in a narrower one. So use it only for two, three or six cards. Use `.vgrid.two` for four cards (2 × 2) and
`.vgrid.three` for five (3 + 2), so no card is left alone on a row.

Keep tables for lookup data. A table whose cells hold sentences that describe shapes or behaviour should
become pictures.

## Text

- Short, plain sentences. The main reader's English is a second language. One idea per sentence. No idioms.
- A paragraph before a picture says what to look at, in one or two sentences. Do not repeat in text what the
  picture shows.
- A section with more than three paragraphs in a row needs a picture, cards or a callout between them.
- Never drop facts to make room: move them into the picture, a card, a caption or a callout.

## Drawing SVGs

- `viewBox="0 0 760 H"` for a `figure.diagram`; `0 0 240 130` for a `.vcard`; about `0 0 320 140` in a `.split`.
- Every `<svg>` has `role="img"` and an `aria-label` that says what the picture shows.
- Only classes from `docs/assets/style.css`. No `style`, no `fill=`/`stroke=` attributes, no `<style>`,
  no `<script>`. Colors follow the theme through the classes.
- Colors keep their meaning: `cpu` blue, `gpu` green, `mem` amber (buffers, textures), `shader` purple.
  `ok` / `bad` are right / wrong, `light` is light rays, `shade` is a shadowed or covered area, `accent` / `hot`
  is "look here", `solid` is an opaque object, `surface` a thick line for ground or geometry, `paper` and `zone`
  are backgrounds, `grid` thin grid lines (one `<path>` with `M..V..H..` segments).
- Text: default 13px, `small` 11px, `title` bold, `big` 16px, `mono`, `mid` / `end` anchors, `t-cpu` ...
  `t-ok` `t-bad` `t-hot` `t-faint` colors, `halo` for text that crosses a line.
- Arrows: `class="arrow"` plus a color (`cpu gpu mem shader hot ok bad light`), `dashed`, `thick`, and
  `flowing` for data that moves. Arrowheads are markers in the page's `<defs>`: `#ah`, `#ah-cpu`, `#ah-gpu`,
  `#ah-mem`, `#ah-hot`, and, when the page defines them, `#ah-shader`, `#ah-ok`, `#ah-bad`, `#ah-light`
  (copy a marker line and change the id and the `head` color class).
- Numbered markers: `<circle r="10" class="badge"/>` with `<text class="badge-n">1</text>` 4px below its center.
- Opacity steps: `o-25`, `o-50`, `o-75`.
- At most two short lines of explanation inside an SVG. Longer text goes in the `figcaption` or in HTML.
- Put a `.legend` above a diagram that uses more than two colors.
- Estimate text width as 6.5px per character at 13px and 5.5px at 11px (`mono`: 7.2px at 12px). Text must
  fit its box with 6px to spare and must not cross an arrow or another label.

## Labs: pictures the reader can change

A lab is a small simulation of the section's idea: it computes the real thing (the real plane test, the real
depth compare, the real allocator) and draws the result, so the reader can find the edge cases by hand.
`docs/guides/shadow-mapping.html` ("Find the bias") and `docs/guides/frustum-culling.html` are the models.

**In the page.** Two script tags in the `<head>`, after `site.js`, and one empty figure where the lab goes:

```html
<script defer src="../assets/lab.js"></script>
<script defer src="../assets/labs/frustum-culling.js"></script>
...
<figure class="lab" data-lab="frustum-cull" data-title="Cull a field of spheres">
  <figcaption>What to try, in one or two sentences.</figcaption>
</figure>
```

**In `docs/assets/labs/<page name>.js`.** One `RasmLab.register("name", function (lab) { ... })` per lab. The
function builds the picture once, then gives `lab.draw` the one function that updates it from the controls:

| Helper | Gives |
|---|---|
| `lab.svg(w, h, label)` | an `<svg>` (use width 760); `lab.el(parent, tag, attrs)` adds a child, `lab.set(el, attrs)` changes it (`text` sets the content), `lab.clear(el)` empties a group |
| `lab.canvas(w, h, label)` | a 2D canvas context, only for per-pixel pictures (shading, textures); `lab.css("--gpu")` reads a theme color for it |
| `lab.slider({ label, min, max, step, value, format })` | a slider; read `.value` |
| `lab.choice({ label, options, value })` | a row of buttons, one active; `options` are strings or `{ value, label }` |
| `lab.toggle({ label, value })`, `lab.button(label, fn)` | a checkbox; a button |
| `lab.handle(svg, { x, y, label, minX, maxX, minY, maxY })` | a point the reader drags (arrow keys work too); read `.x`, `.y` |
| `lab.clickable(el, label, fn)` | makes an SVG element a button (cells, slots, passes) |
| `lab.readout(label)` | a number tile under the picture: `.set(text, "ok" / "bad" / "gpu" / ...)` |
| `lab.note()` | one sentence under the controls that says what the picture shows now: `.set(text, "ok" / "bad")` |
| `lab.animate(fn(dt, t))` | calls `fn` every frame while the lab is on screen, with a Play / Pause button |
| `lab.draw(fn)`, `lab.redraw()` | the update function; controls call it by themselves |

Rules:

- A lab has a goal the reader can reach: a state that is right, a state that breaks. The `lab.note()` says which
  one the picture is in, and why, in plain words. The `figcaption` says what to try.
- Compute, do not fake: the picture comes from the same formula or algorithm as the text above it. Numbers in
  the readouts must agree with the page's worked example where they overlap.
- Same drawing rules as a static SVG: only classes from `style.css`, the same color meanings, text that fits.
  Set a `fill` or `stroke` attribute only when the color is data (a shaded pixel, a depth value as gray).
- At most 3 controls and 3 readouts. One idea per lab. A second idea is a second lab.
- No RASM names in a concept section's lab; a lab in "In RASM" may use the engine's real numbers and names.
- Keep the static figure that names the parts. A lab replaces a picture only when it shows everything the
  picture did, at its default state.
- Motion is for things that move in time (frames, queues, a ring of images). It must be pausable, which
  `lab.animate` gives. Do not animate decoration.
- The page must still read without JavaScript: the text and static figures carry the facts.
- The script is plain JavaScript like `site.js`: 4 spaces, `const` / `let`, `function` expressions, no
  libraries, no network, deterministic (seed your own random numbers).

Check labs with `python .claude/skills/sync-docs/scripts/docsync.py labs [page]`: it opens each page in
headless Chrome with `?labtest`, which moves every control to its ends, and fails on a lab that throws or does
not mount. Then look at it: `page.html?labonly=<name>&theme=dark` (and `light`) shows one lab alone, for a
900 × 760 screenshot. To see another state, change the control's default for the screenshot and put it back.

## Check the picture

Screenshot every page you change, in both themes, and read the image (see step 3 of `SKILL.md`). A plain
screenshot follows the OS theme, so force each one: copy the page to a temporary file with
`<html lang="en" data-theme="dark">`, screenshot it, do the same with `data-theme="light"`, and delete the
copies. Fix text that overflows a box, labels on top of lines, and shapes that are hard to see in
one theme.
