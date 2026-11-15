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
| a process that changes one picture over time | `.stepper` with one `.step` per state, each with its own SVG and one `<p>` | one crowded diagram, or a numbered list |
| numbers from a worked example | `.stats` tiles (`<b>` value, `<span>` meaning, `<small>` how it is computed) | a two-column table |
| a formula | `.formula` (`p.eq` lines, then a `dl` naming each term), or `<pre><code class="lang-text">` for several aligned lines | a formula inside a sentence |
| symptoms and their causes | `dl.pitfalls` (`<div><dt>what you see</dt><dd>why, and the fix</dd></div>`) | a table |
| a rule the reader must not miss | `.callout`, `.callout.warn`, `.callout.rule` with a `<strong>` title | bold text in a paragraph |
| reference data with 3+ columns, code maps, "This page / RASM" | a table inside `.table-wrap` | — |

The page column fits three `.vcard`s in a row. Use `.vgrid.two` for four cards (2 × 2) and `.vgrid.three` to
force three columns, so no card is left alone on a row.

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

## Check the picture

Screenshot every page you change, in both themes, and read the image (see step 3 of `SKILL.md`). For the
light theme, copy the page to a temporary file with `<html lang="en" data-theme="light">`, screenshot it, and
delete the copy. Fix text that overflows a box, labels on top of lines, and shapes that are hard to see in
one theme.
