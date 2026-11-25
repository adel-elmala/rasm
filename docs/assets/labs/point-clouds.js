// Labs of docs/guides/point-clouds.html.

// A seeded random number generator, so a lab draws the same points every time.
function makePointRandom(seed) {
    let state = seed >>> 0;
    return function () {
        state = (state + 0x6D2B79F5) >>> 0;
        let t = state;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// The first k points of a chunk of 1024 in three orders: as a scanner wrote them (rows), shuffled at random, and the
// Morton order read in bit-reversed steps (the page's rule). An 8 x 8 grid counts the cells the prefix leaves empty.
RasmLab.register("prefix-order", function (lab) {
    const W = 760;
    const H = 330;
    const N = 1024;
    const BITS = 10;
    const SIDE = 300;
    const CELLS = 8;
    const svg = lab.svg(W, H, "A square chunk of 1024 points. The first k points of the chosen order are drawn full, the others faint, over an 8 by 8 grid whose empty cells are marked.");
    const order = lab.choice({ label: "Order", options: [{ value: "rows", label: "as scanned (rows)" }, { value: "random", label: "random shuffle" }, { value: "morton", label: "Morton, bit-reversed" }], value: "morton" });
    const count = lab.slider({ label: "Points drawn (the prefix k)", min: 16, max: N, step: 1, value: 96 });
    const emptyOut = lab.readout("empty cells of 64");
    const shareOut = lab.readout("share of the chunk drawn");
    const note = lab.note();

    // Points on a jittered grid, written row by row, as a scan line would.
    const random = makePointRandom(7);
    const points = [];
    for (let y = 0; y < 32; ++y) {
        for (let x = 0; x < 32; ++x)
            points.push({ x: (x + 0.15 + 0.7 * random()) / 32, y: (y + 0.15 + 0.7 * random()) / 32 });
    }
    function spread(v) {
        let x = v & 0xFFFF;
        let result = 0;
        for (let i = 0; i < 16; ++i)
            result |= ((x >> i) & 1) << (2 * i);
        return result >>> 0;
    }
    function reverse(v, bits) {
        let r = 0;
        for (let i = 0; i < bits; ++i)
            r |= ((v >> i) & 1) << (bits - 1 - i);
        return r;
    }
    const orders = { rows: [], random: [], morton: [] };
    for (let i = 0; i < N; ++i)
        orders.rows.push(i);
    orders.random = orders.rows.slice();
    const shuffle = makePointRandom(11);
    for (let i = N - 1; i > 0; --i) {
        const j = Math.floor(shuffle() * (i + 1));
        const t = orders.random[i];
        orders.random[i] = orders.random[j];
        orders.random[j] = t;
    }
    const sorted = orders.rows.slice().sort(function (a, b) {
        const ca = spread(Math.round(points[a].x * 65535)) | (spread(Math.round(points[a].y * 65535)) << 1);
        const cb = spread(Math.round(points[b].x * 65535)) | (spread(Math.round(points[b].y * 65535)) << 1);
        return (ca >>> 0) - (cb >>> 0);
    });
    for (let i = 0; i < N; ++i)
        orders.morton.push(sorted[reverse(i, BITS)]);

    const left = (W - SIDE) / 2;
    const top = 15;
    lab.el(svg, "rect", { x: left, y: top, width: SIDE, height: SIDE, class: "paper" });
    const cellGroup = lab.el(svg, "g", {});
    let grid = "";
    for (let i = 1; i < CELLS; ++i)
        grid += "M" + (left + i * SIDE / CELLS) + "," + top + " V" + (top + SIDE) + " M" + left + "," + (top + i * SIDE / CELLS) + " H" + (left + SIDE) + " ";
    lab.el(svg, "path", { d: grid, class: "grid" });
    const dots = [];
    for (let i = 0; i < N; ++i)
        dots.push(lab.el(svg, "circle", { cx: left + points[i].x * SIDE, cy: top + points[i].y * SIDE, r: 2.2 }));
    lab.el(svg, "text", { x: left - 14, y: top + 14, class: "end small t-faint", text: "the chunk" });
    lab.el(svg, "text", { x: left + SIDE + 14, y: top + SIDE - 6, class: "small t-faint", text: "faint: not drawn" });

    lab.draw(function () {
        const list = orders[order.value];
        const k = Math.round(count.value);
        const drawn = new Array(N).fill(false);
        for (let i = 0; i < k; ++i)
            drawn[list[i]] = true;
        const counts = new Array(CELLS * CELLS).fill(0);
        for (let i = 0; i < N; ++i) {
            lab.set(dots[i], { class: drawn[i] ? "gpu" : "gpu o-25", r: drawn[i] ? 2.6 : 1.4 });
            if (drawn[i])
                ++counts[Math.min(CELLS - 1, Math.floor(points[i].y * CELLS)) * CELLS + Math.min(CELLS - 1, Math.floor(points[i].x * CELLS))];
        }
        lab.clear(cellGroup);
        let empty = 0;
        for (let c = 0; c < CELLS * CELLS; ++c) {
            if (counts[c] > 0)
                continue;
            ++empty;
            lab.el(cellGroup, "rect", { x: left + (c % CELLS) * SIDE / CELLS, y: top + Math.floor(c / CELLS) * SIDE / CELLS, width: SIDE / CELLS, height: SIDE / CELLS, class: "bad o-25" });
        }
        emptyOut.set(String(empty), empty === 0 ? "ok" : "bad");
        shareOut.set((100 * k / N).toFixed(1) + " %");
        if (empty === 0)
            note.set("Every cell has a point: the prefix is an even sample, so the chunk looks whole at this size.", "ok");
        else if (order.value === "morton")
            note.set("A prefix shorter than 64 points cannot fill 64 cells; from 64 points on this order leaves none empty.", "ok");
        else
            note.set(order.value === "rows" ? "The prefix is the first rows only: the rest of the chunk is missing." : "Random points clump and leave holes, even when there are many more points than cells.", "bad");
    });
});

// How many points a chunk keeps: k = density * D^2 / area, with D the chunk's sphere on screen and area pi / 4 of a
// round point's square, at most the chunk's points. The canvas draws a 200 x 200 pixel window of a flat chunk seen from
// above, at its real pixel size, with that many points per area, and counts the pixels no point covers: with one sample
// a pixel's centre, with 4x MSAA any of its four samples.
RasmLab.register("point-density", function (lab) {
    const W = 760;
    const H = 260;
    const WINDOW = 200;
    const TOTAL = 65536;
    const ctx = lab.canvas(W, H, "A 200 by 200 pixel window of a flat chunk seen from above, drawn with as many points as the density rule keeps, and the pixels left empty.");
    const distance = lab.slider({ label: "Chunk on screen (its sphere's diameter D, pixels)", min: 40, max: 1600, step: 10, value: 300 });
    const density = lab.choice({ label: "density", options: ["0.5", "1", "2", "4"], value: "2" });
    const samples = lab.choice({ label: "Samples per pixel", options: [{ value: "1", label: "1" }, { value: "4", label: "4 (MSAA)" }], value: "4" });
    const keptOut = lab.readout("points the chunk draws");
    const holesOut = lab.readout("empty pixels");
    const note = lab.note();
    const SIZE = 2;

    lab.draw(function () {
        const D = distance.value;
        const area = 0.785398 * SIZE * SIZE;
        const offsets = samples.value === "4" ? [[0.375, 0.125], [0.875, 0.375], [0.125, 0.625], [0.625, 0.875]] : [[0.5, 0.5]];
        const kept = Math.min(TOTAL, Math.max(1, Math.ceil(parseFloat(density.value) * D * D / area)));
        // A flat square chunk fills (D / sqrt 2)^2 pixels: its points per pixel, and so per window.
        const perPixel = kept / (D * D / 2);
        const inWindow = Math.round(perPixel * WINDOW * WINDOW);
        const covered = new Uint8Array(WINDOW * WINDOW);
        const random = makePointRandom(3);
        for (let i = 0; i < inWindow; ++i) {
            const cx = random() * WINDOW;
            const cy = random() * WINDOW;
            for (let y = Math.floor(cy - 1); y <= Math.floor(cy + 1); ++y) {
                for (let x = Math.floor(cx - 1); x <= Math.floor(cx + 1); ++x) {
                    if (x < 0 || y < 0 || x >= WINDOW || y >= WINDOW)
                        continue;
                    for (const offset of offsets) {
                        const dx = x + offset[0] - cx;
                        const dy = y + offset[1] - cy;
                        if (dx * dx + dy * dy <= 1)
                            covered[y * WINDOW + x] = 1;
                    }
                }
            }
        }
        let empty = 0;
        for (let i = 0; i < covered.length; ++i)
            empty += covered[i] ? 0 : 1;
        ctx.fillStyle = lab.css("--bg-soft");
        ctx.fillRect(0, 0, W, H);
        const scale = 1.2;
        const ox = (W - WINDOW * scale) / 2;
        const oy = (H - WINDOW * scale) / 2;
        const fill = lab.css("--gpu");
        const hole = lab.css("--bad");
        for (let y = 0; y < WINDOW; ++y) {
            for (let x = 0; x < WINDOW; ++x) {
                ctx.fillStyle = covered[y * WINDOW + x] ? fill : hole;
                ctx.fillRect(ox + x * scale, oy + y * scale, scale + 0.05, scale + 0.05);
            }
        }
        const share = 100 * empty / covered.length;
        keptOut.set(kept.toLocaleString("en-US") + (kept === TOTAL ? " (all)" : ""));
        holesOut.set(share.toFixed(1) + " %", share < 0.5 ? "ok" : "bad");
        if (kept === TOTAL && share >= 0.5)
            note.set("Every point is drawn and holes remain: the data is too sparse for 2-pixel points this close. Bigger points fill it.", "bad");
        else if (share < 0.5)
            note.set("The points cover the chunk: fewer would leave holes, more would cost time and add nothing.", "ok");
        else
            note.set("Too few points for this size on screen: raise the density to fill the holes.", "bad");
    });
});

// The compute path on a 12 x 8 pixel patch: each point writes the pixels whose centre its square holds (the centre
// snapped to 1/256 of a pixel, as the rasterizer snaps a triangle's corners), with a 64-bit max of depth << 32 | id,
// so the nearest point wins whatever the order the threads ran in, and the higher id on a tie, as a later triangle
// would. The picked point also shows its triangle (corners -1, -1; 3, -1; -1, 3 of its square): the pixel stage keeps
// the same pixels, so the two paths draw the same picture.
RasmLab.register("compute-raster", function (lab) {
    const W = 760;
    const H = 330;
    const COLS = 12;
    const ROWS = 8;
    const CELL = 36;
    const left = 30;
    const top = 20;
    const svg = lab.svg(W, H, "A patch of 12 by 8 pixels. Seven points, each with a depth and an id, write the pixels whose centre lies in their square with a 64-bit max. Each pixel shows the id of the point that won it.");
    const size = lab.slider({ label: "Point size (pixels)", min: 1, max: 2, step: 0.25, value: 2, format: function (v) { return v.toFixed(2); } });
    const written = lab.slider({ label: "Points written so far", min: 0, max: 7, step: 1, value: 7 });
    const order = lab.choice({ label: "Order the threads ran in", options: [{ value: "id", label: "by id" }, { value: "reversed", label: "reversed" }, { value: "shuffled", label: "shuffled" }], value: "shuffled" });
    const picked = lab.slider({ label: "Show the triangle of point", min: 0, max: 6, step: 1, value: 3 });
    const touchedOut = lab.readout("pixel writes (atomics)");
    const sameOut = lab.readout("same as by triangles");
    const note = lab.note();

    // The points: a centre in pixels, a depth (reversed-Z: nearer is larger) and an id.
    const points = [
        { x: 2.3, y: 2.6, depth: 0.40 }, { x: 3.1, y: 3.2, depth: 0.55 }, { x: 5.6, y: 4.1, depth: 0.30 }, { x: 6.2, y: 4.7, depth: 0.62 },
        { x: 8.5, y: 2.5, depth: 0.48 }, { x: 8.9, y: 3.0, depth: 0.48 }, { x: 4.4, y: 6.3, depth: 0.35 },
    ];
    const orders = { id: [0, 1, 2, 3, 4, 5, 6], reversed: [6, 5, 4, 3, 2, 1, 0], shuffled: [3, 0, 5, 2, 6, 4, 1] };
    const hues = ["--gpu", "--cpu", "--mem", "--shader", "--ok", "--accent", "--bad"];

    // The pixels a point of diameter d writes: x from ceil(c - d/2 - 0.5) to floor(c + d/2 - 0.5), at most two.
    function pixelsOf(p, d) {
        const cx = Math.round(p.x * 256) / 256;
        const cy = Math.round(p.y * 256) / 256;
        const list = [];
        const x0 = Math.ceil(cx - d / 2 - 0.5);
        const y0 = Math.ceil(cy - d / 2 - 0.5);
        const x1 = Math.min(Math.floor(cx + d / 2 - 0.5), x0 + 1);
        const y1 = Math.min(Math.floor(cy + d / 2 - 0.5), y0 + 1);
        for (let y = y0; y <= y1; ++y)
            for (let x = x0; x <= x1; ++x)
                if (x >= 0 && y >= 0 && x < COLS && y < ROWS)
                    list.push([x, y]);
        return list;
    }
    // The triangle path's test at a pixel's centre: inside the square (edges included), and the depth test with
    // GREATER_OR_EQUAL, drawn in id order.
    function byTriangles(d) {
        const winner = new Array(COLS * ROWS).fill(-1);
        for (let i = 0; i < points.length; ++i) {
            for (const [x, y] of pixelsOf(points[i], d)) {
                const w = winner[y * COLS + x];
                if (w < 0 || points[i].depth >= points[w].depth)
                    winner[y * COLS + x] = i;
            }
        }
        return winner;
    }

    const cells = [];
    const labels = [];
    for (let y = 0; y < ROWS; ++y) {
        for (let x = 0; x < COLS; ++x) {
            cells.push(lab.el(svg, "rect", { x: left + x * CELL, y: top + y * CELL, width: CELL, height: CELL, class: "paper" }));
            labels.push(lab.el(svg, "text", { x: left + x * CELL + CELL / 2, y: top + y * CELL + CELL / 2 + 4, class: "mid small mono", text: "" }));
        }
    }
    const triangle = lab.el(svg, "path", { class: "hot" });
    const square = lab.el(svg, "rect", { class: "arrow dashed nofill" });
    const dots = points.map(function (p) { return lab.el(svg, "circle", { cx: left + p.x * CELL, cy: top + p.y * CELL, r: 4 }); });
    const legendX = left + COLS * CELL + 24;
    lab.el(svg, "text", { x: legendX, y: top + 12, class: "small title", text: "point" });
    lab.el(svg, "text", { x: legendX + 60, y: top + 12, class: "small title", text: "depth" });
    const rows = points.map(function (p, i) {
        lab.el(svg, "text", { x: legendX + 60, y: top + 34 + i * 20, class: "small mono", text: p.depth.toFixed(2) });
        return lab.el(svg, "text", { x: legendX, y: top + 34 + i * 20, class: "small mono", text: String(i) });
    });
    lab.el(svg, "text", { x: legendX, y: top + 34 + 7 * 20 + 10, class: "small t-faint", text: "a pixel keeps max(depth << 32 | id):" });
    lab.el(svg, "text", { x: legendX, y: top + 34 + 7 * 20 + 26, class: "small t-faint", text: "the nearest point, then the higher id" });

    lab.draw(function () {
        const d = size.value;
        const list = orders[order.value];
        const k = Math.round(written.value);
        const word = new Array(COLS * ROWS).fill(-1);
        let touched = 0;
        for (let n = 0; n < k; ++n) {
            const i = list[n];
            for (const [x, y] of pixelsOf(points[i], d)) {
                ++touched;
                const w = word[y * COLS + x];
                if (w < 0 || points[i].depth > points[w].depth || (points[i].depth === points[w].depth && i > w))
                    word[y * COLS + x] = i;
            }
        }
        const reference = byTriangles(d);
        let same = true;
        for (let c = 0; c < COLS * ROWS; ++c) {
            const w = word[c];
            lab.set(cells[c], { class: w < 0 ? "paper" : "paper o-75", style: w < 0 ? null : "fill: var(" + hues[w] + ")" });
            lab.set(labels[c], { text: w < 0 ? "" : String(w) });
            if (k === points.length && w !== reference[c])
                same = false;
        }
        points.forEach(function (p, i) {
            const done = list.indexOf(i) < k;
            lab.set(dots[i], { style: "fill: var(" + hues[i] + ")", class: done ? "solid" : "o-25" });
            lab.set(rows[i], { class: done ? "small mono" : "small mono t-faint" });
        });
        const p = points[Math.round(picked.value)];
        const r = d / 2 * CELL;
        const cx = left + p.x * CELL;
        const cy = top + p.y * CELL;
        lab.set(square, { x: cx - r, y: cy - r, width: 2 * r, height: 2 * r });
        lab.set(triangle, { d: "M" + (cx - r) + "," + (cy + r) + " L" + (cx + 3 * r) + "," + (cy + r) + " L" + (cx - r) + "," + (cy - 3 * r) + " Z" });
        touchedOut.set(String(touched));
        sameOut.set(k === points.length ? (same ? "yes" : "no") : "–", same ? "ok" : "bad");
        if (k < points.length)
            note.set("Each thread writes its point's pixels with one atomic each, in any order; " + (points.length - k) + " points are still to run.", "");
        else if (order.value !== "id")
            note.set("Run in another order, the pixels end the same: the max keeps the nearest point, and the higher id on a tie (points 4 and 5).", "ok");
        else
            note.set("Every pixel holds the nearest point: the same pixels the points' triangles cover, cut to their squares.", "ok");
    });
});
