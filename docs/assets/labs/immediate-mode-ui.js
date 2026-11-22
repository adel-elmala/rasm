// Labs of docs/guides/immediate-mode-ui.html.

// A clip rectangle in display points, two draggable corners, and the scissor in pixels it becomes: scaled by the
// framebuffer scale, rounded outward, clamped to the framebuffer. The same rule as the formula of the page.
RasmLab.register("ui-scissor", function (lab) {
    const COLUMNS = 16;
    const ROWS = 10;
    const UNIT = 36;
    const X0 = 92;
    const Y0 = 30;
    const svg = lab.svg(760, 430, "A display of 16 by 10 points with its pixel grid. A dashed clip rectangle with two draggable corners, and the solid scissor in pixels that covers it.");
    const scale = lab.choice({ label: "Framebuffer scale", value: "1.5", options: [
        { value: "1", label: "1 (normal)" }, { value: "1.5", label: "1.5" }, { value: "2", label: "2 (high DPI)" },
    ] });
    const clipOut = lab.readout("clip rectangle (points)");
    const scissorOut = lab.readout("scissor x, y, width, height (pixels)");
    const note = lab.note();

    lab.el(svg, "rect", { x: X0, y: Y0, width: COLUMNS * UNIT, height: ROWS * UNIT, class: "zone" });
    const grid = lab.el(svg, "path", { class: "grid" });
    const scissor = lab.el(svg, "rect", { class: "gpu o-50" });
    const clip = lab.el(svg, "rect", { class: "nofill accent thick dashed" });
    lab.el(svg, "rect", { x: X0, y: Y0, width: COLUMNS * UNIT, height: ROWS * UNIT, class: "nofill box" });
    lab.el(svg, "text", { x: X0, y: Y0 + ROWS * UNIT + 22, class: "small", text: "display: 16 × 10 points; thin lines are pixels" });
    const a = lab.handle(svg, { x: X0 + 3.3 * UNIT, y: Y0 + 2.6 * UNIT, label: "Top-left corner of the clip rectangle", minX: 10, maxX: 750, minY: 10, maxY: 400 });
    const b = lab.handle(svg, { x: X0 + 11.4 * UNIT, y: Y0 + 7.2 * UNIT, label: "Bottom-right corner of the clip rectangle", minX: 10, maxX: 750, minY: 10, maxY: 400 });

    function points(v, origin) { return (v - origin) / UNIT; }
    function round1(v) { return Math.round(v * 10) / 10; }

    lab.draw(function () {
        const s = Number(scale.value);
        const width = Math.round(COLUMNS * s);
        const height = Math.round(ROWS * s);
        const px = UNIT / s;
        let d = "";
        for (let i = 1; i < width; ++i)
            d += `M${X0 + i * px},${Y0}V${Y0 + ROWS * UNIT}`;
        for (let j = 1; j < height; ++j)
            d += `M${X0},${Y0 + j * px}H${X0 + COLUMNS * UNIT}`;
        lab.set(grid, { d: d });

        const left = points(Math.min(a.x, b.x), X0);
        const right = points(Math.max(a.x, b.x), X0);
        const top = points(Math.min(a.y, b.y), Y0);
        const bottom = points(Math.max(a.y, b.y), Y0);
        lab.set(clip, { x: X0 + left * UNIT, y: Y0 + top * UNIT, width: (right - left) * UNIT, height: (bottom - top) * UNIT });
        clipOut.set(`${round1(left)}, ${round1(top)} to ${round1(right)}, ${round1(bottom)}`);

        // scale, clamp, round outward
        const l = Math.max(left * s, 0);
        const t = Math.max(top * s, 0);
        const r = Math.min(right * s, width);
        const btm = Math.min(bottom * s, height);
        if (r <= l || btm <= t) {
            lab.set(scissor, { width: 0, height: 0 });
            scissorOut.set("none", "bad");
            note.set("Nothing of the rectangle is on the framebuffer: the draw is skipped.", "bad");
            return;
        }
        const x0 = Math.floor(l);
        const y0 = Math.floor(t);
        const x1 = Math.ceil(r);
        const y1 = Math.ceil(btm);
        lab.set(scissor, { x: X0 + x0 * px, y: Y0 + y0 * px, width: (x1 - x0) * px, height: (y1 - y0) * px });
        scissorOut.set(`${x0}, ${y0}, ${x1 - x0}, ${y1 - y0}`, "gpu");
        const clamped = left < 0 || top < 0 || right > COLUMNS || bottom > ROWS;
        note.set(clamped ? "Part of the rectangle is off the display: the scissor stops at the framebuffer's edge." : "Pixels the rectangle only partly covers are kept: the scissor rounds outward.", "ok");
    });
});

// A row of pixels across a glyph's edge, white text on a dark window (or the reverse), blended by coverage once as
// sRGB numbers and once in linear light.
RasmLab.register("ui-blend", function (lab) {
    const COVERAGE = [1, 1, 0.9, 0.7, null, 0.3, 0.1, 0, 0];
    const svg = lab.svg(760, 250, "Two rows of nine pixels across a glyph edge, from fully covered to empty. The top row blends the colors as sRGB numbers, the bottom row in linear light. The middle pixel's coverage is set by the slider.");
    const coverage = lab.slider({ label: "Coverage of the middle pixel", min: 0, max: 1, step: 0.05, value: 0.5, format: function (v) { return v.toFixed(2); } });
    const style = lab.choice({ label: "Colors", value: "light", options: [
        { value: "light", label: "light text, dark window" }, { value: "dark", label: "dark text, light window" },
    ] });
    const numbersOut = lab.readout("middle pixel, mixed as numbers (byte)");
    const linearOut = lab.readout("middle pixel, mixed in linear light (byte)");
    const note = lab.note();

    function decode(c) { return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); }
    function encode(l) { return l <= 0.0031308 ? 12.92 * l : 1.055 * Math.pow(l, 1 / 2.4) - 0.055; }
    function gray(v) { const b = Math.round(v * 255); return `rgb(${b},${b},${b})`; }

    const SIZE = 64;
    const X0 = 90;
    const rows = [];
    [["as numbers", 40], ["linear light", 140]].forEach(function (row) {
        lab.el(svg, "text", { x: X0 - 10, y: row[1] + SIZE / 2 + 4, class: "end small", text: row[0] });
        rows.push(COVERAGE.map(function (c, i) {
            // The gray is data, so the fill is set by draw and the rectangle has no class; an outline keeps white visible.
            const pixel = lab.el(svg, "rect", { x: X0 + i * (SIZE + 6), y: row[1], width: SIZE, height: SIZE });
            lab.el(svg, "rect", { x: X0 + i * (SIZE + 6), y: row[1], width: SIZE, height: SIZE, class: "nofill box" });
            return pixel;
        }));
    });
    const labels = COVERAGE.map(function (c, i) {
        return lab.el(svg, "text", { x: X0 + i * (SIZE + 6) + SIZE / 2, y: 26, class: "mid small" });
    });
    lab.el(svg, "rect", { x: X0 + 4 * (SIZE + 6) - 3, y: 37, width: SIZE + 6, height: 170, class: "nofill accent thick" });
    lab.el(svg, "text", { x: X0, y: 236, class: "small", text: "coverage of each pixel above; the outlined one is the slider's" });

    lab.draw(function () {
        const light = style.value === "light";
        const text = light ? 1.0 : 0.1;
        const back = light ? 0.1 : 1.0;
        let middle = [0, 0];
        COVERAGE.forEach(function (fixed, i) {
            const c = fixed === null ? coverage.value : fixed;
            const numbers = c * text + (1 - c) * back;
            const linear = encode(c * decode(text) + (1 - c) * decode(back));
            lab.set(rows[0][i], { fill: gray(numbers) });
            lab.set(rows[1][i], { fill: gray(linear) });
            lab.set(labels[i], { text: c.toFixed(2) });
            if (fixed === null)
                middle = [numbers, linear];
        });
        const n = Math.round(middle[0] * 255);
        const l = Math.round(middle[1] * 255);
        numbersOut.set(String(n), "ok");
        linearOut.set(`${l} (${l - n >= 0 ? "+" : ""}${l - n})`, l === n ? "ok" : "bad");
        if (l === n)
            note.set("At full or no coverage both ways agree: only edges differ.", "ok");
        else if (light)
            note.set("In linear light the edge is brighter: light text looks bolder than designed.", "bad");
        else
            note.set("In linear light the edge is brighter: dark text looks thinner than designed.", "bad");
    });
});
