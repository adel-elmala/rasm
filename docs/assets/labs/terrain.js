// Labs of docs/guides/terrain.html.

// Section 4: the crack between two levels, seen from the side along their shared edge. The ground's height along the
// edge is a sum of two waves. The coarse tile has COARSE cells along the edge, the fine one 2 or 4 times as many; each
// edge is a straight line between its own points. The gap is where the two lines part; a skirt hangs down from both
// edges, and the hole is the part of the gap the upper edge's skirt does not reach.
RasmLab.register("terrain-skirt", function (lab) {
    const EDGE = 256;           // metres along the edge
    const COARSE = 2;           // the coarse tile's cells along it
    const X0 = 40;
    const X1 = 720;
    const BASE_Y = 140;
    const PX_PER_M = 2;
    const svg = lab.svg(760, 400, "The shared edge of a coarse and a fine tile seen from the side: the ground, the two edges, the gap between them, and the skirts that hang from them.");
    const hills = lab.slider({ label: "Hills", min: 0, max: 60, step: 1, value: 30, format: function (v) { return `${v} m`; } });
    const skirt = lab.slider({ label: "Skirt", min: 0, max: 60, step: 1, value: 4, format: function (v) { return `${v} m`; } });
    const levels = lab.choice({ label: "Levels apart", options: [{ value: "1", label: "1" }, { value: "2", label: "2" }], value: "1" });
    const gapOut = lab.readout("largest gap");
    const holeOut = lab.readout("largest hole");
    const note = lab.note();

    lab.el(svg, "text", { x: X0, y: 24, class: "title", text: "Along the shared edge" });
    const legend = [
        ["arrow cpu thick", "coarse edge"],
        ["arrow gpu thick", "fine edge"],
        ["shade o-50", "the gap"],
        ["bad", "a hole: the sky shows"],
    ];
    const LEGEND_X = [220, 330, 430, 520];
    legend.forEach(function (row, i) {
        const x = LEGEND_X[i];
        if (row[0].indexOf("arrow") === 0) lab.el(svg, "line", { x1: x, y1: 20, x2: x + 18, y2: 20, class: row[0] });
        else lab.el(svg, "rect", { x: x, y: 13, width: 18, height: 14, class: row[0] });
        lab.el(svg, "text", { x: x + 24, y: 24, class: "small", text: row[1] });
    });
    const ground = lab.el(svg, "polyline", { class: "surface o-50" });
    const skirts = lab.el(svg, "g");
    const gapArea = lab.el(svg, "polygon", { class: "shade o-50" });
    const holes = lab.el(svg, "g");
    const coarseLine = lab.el(svg, "polyline", { class: "arrow cpu thick" });
    const fineLine = lab.el(svg, "polyline", { class: "arrow gpu thick" });
    const points = lab.el(svg, "g");
    lab.el(svg, "text", { x: X0, y: 392, class: "small t-faint", text: `0 m` });
    lab.el(svg, "text", { x: X1, y: 392, class: "end small t-faint", text: `${EDGE} m` });

    function height(t, amplitude) {
        return amplitude * (0.6 * Math.sin(2 * Math.PI * 1.3 * t + 0.4) + 0.4 * Math.sin(2 * Math.PI * 3.1 * t + 1.1));
    }

    // A tile's edge: a straight line between its points, at t from 0 to 1.
    function edgeAt(t, cells, amplitude) {
        const x = Math.min(t * cells, cells - 1e-9);
        const i = Math.floor(x);
        const f = x - i;
        return (1 - f) * height(i / cells, amplitude) + f * height((i + 1) / cells, amplitude);
    }

    function px(t) {
        return (X0 + (X1 - X0) * t).toFixed(1);
    }

    function py(metres) {
        return (BASE_Y - metres * PX_PER_M).toFixed(1);
    }

    lab.draw(function () {
        const amplitude = hills.value;
        const fineCells = COARSE * (levels.value === "1" ? 2 : 4);
        const s = skirt.value;
        const SAMPLES = 160;
        const groundPoints = [];
        const coarsePoints = [];
        const finePoints = [];
        const upper = [];
        const lower = [];
        let largestGap = 0;
        let largestHole = 0;
        for (let k = 0; k <= SAMPLES; k++) {
            const t = k / SAMPLES;
            const c = edgeAt(t, COARSE, amplitude);
            const f = edgeAt(t, fineCells, amplitude);
            groundPoints.push(`${px(t)},${py(height(t, amplitude))}`);
            coarsePoints.push(`${px(t)},${py(c)}`);
            finePoints.push(`${px(t)},${py(f)}`);
            upper.push(Math.max(c, f));
            lower.push(Math.min(c, f));
            largestGap = Math.max(largestGap, Math.abs(c - f));
            largestHole = Math.max(largestHole, Math.abs(c - f) - s);
        }
        lab.set(ground, { points: groundPoints.join(" ") });
        lab.set(coarseLine, { points: coarsePoints.join(" ") });
        lab.set(fineLine, { points: finePoints.join(" ") });
        const gap = [];
        for (let k = 0; k <= SAMPLES; k++) gap.push(`${px(k / SAMPLES)},${py(upper[k])}`);
        for (let k = SAMPLES; k >= 0; k--) gap.push(`${px(k / SAMPLES)},${py(lower[k])}`);
        lab.set(gapArea, { points: gap.join(" ") });

        // Each edge's skirt, hanging s metres below it.
        lab.clear(skirts);
        [[COARSE, "cpu o-25"], [fineCells, "gpu o-25"]].forEach(function (edge) {
            const poly = [];
            for (let k = 0; k <= SAMPLES; k++) poly.push(`${px(k / SAMPLES)},${py(edgeAt(k / SAMPLES, edge[0], amplitude))}`);
            for (let k = SAMPLES; k >= 0; k--) poly.push(`${px(k / SAMPLES)},${py(edgeAt(k / SAMPLES, edge[0], amplitude) - s)}`);
            lab.el(skirts, "polygon", { points: poly.join(" "), class: edge[1] });
        });

        // The holes: from the lower edge up to where the upper edge's skirt ends.
        lab.clear(holes);
        let run = null;
        for (let k = 0; k <= SAMPLES; k++) {
            const open = upper[k] - s > lower[k] + 1e-6;
            if (open) {
                if (!run) run = { top: [], bottom: [] };
                run.top.push(`${px(k / SAMPLES)},${py(upper[k] - s)}`);
                run.bottom.unshift(`${px(k / SAMPLES)},${py(lower[k])}`);
            }
            if ((!open || k === SAMPLES) && run) {
                lab.el(holes, "polygon", { points: run.top.concat(run.bottom).join(" "), class: "bad" });
                run = null;
            }
        }

        lab.clear(points);
        for (let i = 0; i <= fineCells; i++) lab.el(points, "circle", { cx: px(i / fineCells), cy: py(height(i / fineCells, amplitude)), r: 4, class: "gpu" });
        for (let i = 0; i <= COARSE; i++) lab.el(points, "circle", { cx: px(i / COARSE), cy: py(height(i / COARSE, amplitude)), r: 5, class: "cpu" });

        gapOut.set(`${largestGap.toFixed(1)} m`, largestGap > 0.05 ? "hot" : "ok");
        holeOut.set(largestHole > 0.05 ? `${largestHole.toFixed(1)} m` : "none", largestHole > 0.05 ? "bad" : "ok");
        if (largestGap < 0.05) note.set("Flat ground: both edges are the same line, so there is no gap.", "ok");
        else if (largestHole > 0.05) note.set(`The skirt (${s} m) is shorter than the gap (${largestGap.toFixed(1)} m): the sky shows through the red holes.`, "bad");
        else note.set(`The skirt (${s} m) reaches past the largest gap (${largestGap.toFixed(1)} m): no hole is left.`, "ok");
    });
});
