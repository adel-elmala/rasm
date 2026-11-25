// Labs of docs/guides/text-and-labels.html.

// Section 2: one letter drawn at the chosen size from a small atlas of 16 texels per em, as a GPU samples it: bilinear
// between texels, then either the stored coverage (a bitmap) or the stored distance turned into coverage over one
// screen pixel (a distance field), with an optional halo. The letter is three strokes; its exact distance is the
// distance to the nearest stroke minus the stroke's half width.
RasmLab.register("sdf-glyph", function (lab) {
    const W = 760;
    const H = 260;
    const EM = 16;          // atlas texels per em
    const RANGE = 6;        // atlas texels the stored distance spans, from 0 to 1
    const PAD = 4;          // atlas texels around the em
    const SIZE = EM + 2 * PAD;
    const ctx = lab.canvas(W, H, "The letter A drawn at the chosen size from a 16-texel atlas, each screen pixel shown as a square: on the left as a bitmap, on the right as a distance field.");
    const size = lab.slider({ label: "Text size", min: 8, max: 120, step: 1, value: 64, format: function (v) { return `${v} px`; } });
    const halo = lab.slider({ label: "Halo", min: 0, max: 6, step: 0.5, value: 0, format: function (v) { return `${v} px`; } });
    const scaleOut = lab.readout("screen pixels per atlas texel");
    const reachOut = lab.readout("the farthest halo the atlas holds");
    const note = lab.note();

    // The strokes of an A, in ems (y down), and their half width.
    const STROKES = [[0.18, 0.92, 0.5, 0.1], [0.5, 0.1, 0.82, 0.92], [0.3, 0.62, 0.7, 0.62]];
    const HALF = 0.075;
    function segmentDistance(px, py, s) {
        const dx = s[2] - s[0], dy = s[3] - s[1];
        const t = Math.max(0, Math.min(1, ((px - s[0]) * dx + (py - s[1]) * dy) / (dx * dx + dy * dy)));
        return Math.hypot(px - s[0] - t * dx, py - s[1] - t * dy);
    }
    // Signed distance in ems, inside positive.
    function shape(x, y) {
        let d = Infinity;
        STROKES.forEach(function (s) { d = Math.min(d, segmentDistance(x, y, s)); });
        return HALF - d;
    }
    // The two atlases, 8 bits a texel: coverage (4 x 4 samples a texel) and 0.5 + distance / RANGE.
    const coverage = new Float32Array(SIZE * SIZE);
    const field = new Float32Array(SIZE * SIZE);
    for (let j = 0; j < SIZE; j++) {
        for (let i = 0; i < SIZE; i++) {
            let inside = 0;
            for (let s = 0; s < 16; s++) inside += shape((i - PAD + ((s % 4) + 0.5) / 4) / EM, (j - PAD + ((s >> 2) + 0.5) / 4) / EM) > 0 ? 1 : 0;
            coverage[j * SIZE + i] = Math.round(255 * inside / 16) / 255;
            const d = shape((i - PAD + 0.5) / EM, (j - PAD + 0.5) / EM) * EM;
            field[j * SIZE + i] = Math.round(255 * Math.max(0, Math.min(1, 0.5 + d / RANGE))) / 255;
        }
    }
    function bilinear(data, u, v) {
        const x = Math.max(0, Math.min(SIZE - 1.001, u - 0.5));
        const y = Math.max(0, Math.min(SIZE - 1.001, v - 0.5));
        const i = Math.floor(x), j = Math.floor(y), fx = x - i, fy = y - j;
        const a = data[j * SIZE + i], b = data[j * SIZE + i + 1], c = data[(j + 1) * SIZE + i], d = data[(j + 1) * SIZE + i + 1];
        return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
    }
    function rgb(name) {
        ctx.fillStyle = lab.css(name);
        const hex = ctx.fillStyle;
        return [parseInt(hex.slice(1, 3), 16), parseInt(hex.slice(3, 5), 16), parseInt(hex.slice(5, 7), 16)];
    }
    function mix(a, b, t) { return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t]; }

    lab.draw(function () {
        const px = size.value;
        const texelPixels = px / EM;
        const reach = Math.max(0, 0.5 * RANGE * texelPixels - 1);
        const background = rgb("--bg-soft"), ink = rgb("--fg"), glow = rgb("--mem");
        ctx.fillStyle = lab.css("--bg");
        ctx.fillRect(0, 0, W, H);
        // Each half shows the letter's em and a margin, magnified so that it fills 220 canvas pixels.
        const view = 1.3 * px;
        const block = 220 / view;
        [0, 1].forEach(function (half) {
            const left = half === 0 ? 50 : 430;
            for (let y = 0; y < Math.ceil(view); y++) {
                for (let x = 0; x < Math.ceil(view); x++) {
                    // The screen pixel's center in ems, then in atlas texels.
                    const ex = (x + 0.5) / px - 0.15, ey = (y + 0.5) / px - 0.15;
                    const u = ex * EM + PAD, v = ey * EM + PAD;
                    let color = background;
                    if (half === 0) {
                        color = mix(background, ink, bilinear(coverage, u, v));
                    } else {
                        const d = (bilinear(field, u, v) - 0.5) * RANGE * texelPixels;
                        const fill = Math.max(0, Math.min(1, d + 0.5));
                        const outer = halo.value > 0 ? Math.max(0, Math.min(1, d + Math.min(halo.value, reach) + 0.5)) : 0;
                        color = mix(mix(background, glow, outer), ink, fill);
                    }
                    ctx.fillStyle = `rgb(${color.map(Math.round).join(",")})`;
                    ctx.fillRect(left + x * block, 20 + y * block, Math.ceil(block), Math.ceil(block));
                }
            }
        });
        ctx.fillStyle = lab.css("--fg-soft");
        ctx.font = "13px sans-serif";
        ctx.fillText("bitmap: stored coverage", 50, 256);
        ctx.fillText("distance field: stored distance", 430, 256);
        scaleOut.set(texelPixels.toFixed(2));
        reachOut.set(`${reach.toFixed(1)} px`, halo.value > reach ? "bad" : "ok");
        if (halo.value > reach) note.set(`The atlas stores distances only ${(RANGE / 2).toFixed(0)} texels out, ${reach.toFixed(1)} px at this size (less a pixel for the smooth edge): the halo is cut there. A larger range holds a wider halo.`, "bad");
        else if (texelPixels > 1.5) note.set("Magnified: the bitmap blurs, because it blends coverage. The distance field blends distances, which stay straight lines between texels, so its edge stays one pixel wide.", "ok");
        else note.set("Near one texel per pixel both look alike. The distance field wins when the text is larger than its atlas.", "ok");
    });
});

// Section 6: labels placed greedily, as a renderer does each frame: sorted by priority, each one placed when its box
// touches no cell a placed box took, on a grid of 8 px cells. The map zooms around its center; the text keeps its size
// in pixels, so zooming out makes the boxes collide.
RasmLab.register("place-labels", function (lab) {
    const W = 760;
    const H = 340;
    const CELL = 8;
    const svg = lab.svg(W, H, "A map with place names. Placed labels are drawn with their boxes; hidden ones are faint. The grid shows the cells the placed boxes took.");
    const zoom = lab.slider({ label: "Zoom", min: 0.35, max: 2, step: 0.05, value: 0.7, format: function (v) { return `${v.toFixed(2)}x`; } });
    const order = lab.choice({ label: "Order", options: [{ value: "priority", label: "by priority" }, { value: "list", label: "in list order" }], value: "priority" });
    const cellsToggle = lab.toggle({ label: "Show the taken cells", value: false });
    const placedOut = lab.readout("placed");
    const hiddenOut = lab.readout("hidden");
    const note = lab.note();

    // Places: a world position (around 0, 0), a name and a priority (a city 3, a town 2, a village 1).
    const PLACES = [
        { x: -40, y: -10, name: "Lindholm", p: 1 }, { x: 10, y: 0, name: "Marlow", p: 3 }, { x: 60, y: 30, name: "Ashby", p: 1 },
        { x: -150, y: 60, name: "Crane Hill", p: 2 }, { x: -120, y: -80, name: "Oakvale", p: 1 }, { x: 140, y: -60, name: "Eastport", p: 3 },
        { x: 180, y: -30, name: "Wick", p: 1 }, { x: 230, y: 70, name: "Fenwick", p: 2 }, { x: -260, y: -20, name: "Westmoor", p: 2 },
        { x: -40, y: 90, name: "Brook End", p: 1 }, { x: 90, y: -110, name: "Northgate", p: 2 }, { x: 30, y: 55, name: "Mill Lane", p: 1 },
        { x: -200, y: 110, name: "Rye", p: 1 }, { x: 280, y: -100, name: "Holt", p: 1 },
    ];
    const grid = lab.el(svg, "g");
    const boxes = lab.el(svg, "g");

    function textWidth(name, big) { return name.length * (big ? 7.4 : 6.5) + 8; }

    lab.draw(function () {
        const columns = Math.ceil(W / CELL), rows = Math.ceil(H / CELL);
        const taken = new Uint8Array(columns * rows);
        const items = PLACES.map(function (place, index) {
            const cx = W / 2 + place.x * zoom.value * 1.4, cy = H / 2 + place.y * zoom.value * 1.4;
            const w = textWidth(place.name, place.p === 3), h = place.p === 3 ? 20 : 17;
            // The text right of its point.
            return { place: place, index: index, cx: cx, cy: cy, low: { x: cx + 5, y: cy - h / 2 }, high: { x: cx + 5 + w, y: cy + h / 2 } };
        });
        const sorted = items.slice();
        if (order.value === "priority") sorted.sort(function (a, b) { return b.place.p - a.place.p || a.index - b.index; });
        let placed = 0;
        sorted.forEach(function (item) {
            const inView = item.cx >= 0 && item.cx < W && item.cy >= 0 && item.cy < H;
            const x0 = Math.floor(Math.max(item.low.x, 0) / CELL), y0 = Math.floor(Math.max(item.low.y, 0) / CELL);
            const x1 = Math.min(Math.floor(item.high.x / CELL), columns - 1), y1 = Math.min(Math.floor(item.high.y / CELL), rows - 1);
            let free = inView && x0 <= x1 && y0 <= y1;
            for (let y = y0; free && y <= y1; y++) for (let x = x0; free && x <= x1; x++) free = !taken[y * columns + x];
            if (free) for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) taken[y * columns + x] = 1;
            item.placed = free;
            item.inView = inView;
            placed += free ? 1 : 0;
        });

        lab.clear(grid);
        if (cellsToggle.value) {
            for (let y = 0; y < rows; y++) {
                for (let x = 0; x < columns; x++) {
                    if (taken[y * columns + x]) lab.el(grid, "rect", { x: x * CELL, y: y * CELL, width: CELL - 1, height: CELL - 1, class: "mem o-50" });
                }
            }
        }
        lab.clear(boxes);
        items.forEach(function (item) {
            if (!item.inView) return;
            const g = lab.el(boxes, "g", { class: item.placed ? "" : "o-25" });
            lab.el(g, "rect", { x: item.low.x.toFixed(1), y: item.low.y.toFixed(1), width: (item.high.x - item.low.x).toFixed(1), height: (item.high.y - item.low.y).toFixed(1), rx: 3, class: item.placed ? "box" : "box dashed" });
            lab.el(g, "circle", { cx: item.cx.toFixed(1), cy: item.cy.toFixed(1), r: 3.5, class: item.place.p === 3 ? "hot" : "cpu" });
            lab.el(g, "text", { x: (item.low.x + 4).toFixed(1), y: (item.cy + 4.5).toFixed(1), class: item.place.p === 3 ? "title" : "small", text: item.place.name });
        });
        const visible = items.filter(function (item) { return item.inView; }).length;
        placedOut.set(`${placed}`, "ok");
        hiddenOut.set(`${visible - placed}`, visible - placed > 0 ? "bad" : "ok");
        const cityHidden = items.some(function (item) { return item.inView && !item.placed && item.place.p === 3; });
        if (cityHidden) note.set("A city is hidden behind a village: in list order a small place that comes first takes the room. Sort by priority.", "bad");
        else if (visible - placed > 0) note.set("The cities (bold) are placed first; a smaller place whose box touches a taken cell is hidden. Zoom in and the boxes move apart.", "ok");
        else note.set("Every label fits: no box touches another's cells.", "ok");
    });
});
