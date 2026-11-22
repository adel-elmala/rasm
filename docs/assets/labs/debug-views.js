// Labs of docs/guides/debug-views.html.

// Three quads at three depths over a grid of pixels, drawn in the order the reader picks, each fragment counted with
// additive blending. With the depth test, a fragment is counted only when it is nearer than what the pixel holds,
// and then writes its depth (an opaque draw). The count of each pixel is shown with the heat ramp of the page.
RasmLab.register("overdraw-count", function (lab) {
    const COLUMNS = 24;
    const ROWS = 12;
    const CELL = 26;
    const X0 = 68;
    const Y0 = 20;
    // Depth: smaller is nearer. Rectangles in cells.
    const QUADS = [
        { name: "A", depth: 1, x: 2, y: 2, w: 10, h: 7 },
        { name: "B", depth: 2, x: 7, y: 4, w: 10, h: 7 },
        { name: "C", depth: 3, x: 11, y: 1, w: 11, h: 7 },
    ];
    // The heat ramp of the debug display: one step per doubling, 1 to 64.
    const HEAT = [[0.05, 0.05, 0.35], [0.10, 0.30, 0.95], [0.05, 0.80, 0.90], [0.15, 0.85, 0.20], [0.98, 0.88, 0.10], [0.95, 0.15, 0.10], [1, 1, 1]];

    const svg = lab.svg(760, 400, "A grid of 24 by 12 pixels covered by three overlapping quads A, B and C, at three depths. Each pixel is colored by how many fragments were shaded there, from the heat ramp, with the count written in it.");
    const order = lab.choice({ label: "Draw order", value: "back", options: [
        { value: "back", label: "back to front (C, B, A)" }, { value: "front", label: "front to back (A, B, C)" }, { value: "mixed", label: "B, A, C" },
    ] });
    const depthTest = lab.toggle({ label: "Depth test", value: true });
    const shadedOut = lab.readout("fragments shaded");
    const coveredOut = lab.readout("pixels covered");
    const wasteOut = lab.readout("shaded for nothing");
    const note = lab.note();

    function heat(count) {
        if (count < 1)
            return "rgb(0,0,0)";
        const t = Math.min(Math.log2(count), 6);
        const step = Math.min(Math.floor(t), 5);
        const f = t - step;
        const c = HEAT[step].map(function (v, i) { return Math.round(255 * (v + (HEAT[step + 1][i] - v) * f)); });
        return `rgb(${c[0]},${c[1]},${c[2]})`;
    }

    const cells = [];
    const labels = [];
    for (let j = 0; j < ROWS; ++j) {
        for (let i = 0; i < COLUMNS; ++i) {
            // The color is data, so the fill is set by draw.
            cells.push(lab.el(svg, "rect", { x: X0 + i * CELL, y: Y0 + j * CELL, width: CELL, height: CELL }));
            labels.push(lab.el(svg, "text", { x: X0 + i * CELL + CELL / 2, y: Y0 + j * CELL + CELL / 2 + 4, class: "mid small halo" }));
        }
    }
    let grid = "";
    for (let i = 0; i <= COLUMNS; ++i)
        grid += `M${X0 + i * CELL},${Y0}V${Y0 + ROWS * CELL}`;
    for (let j = 0; j <= ROWS; ++j)
        grid += `M${X0},${Y0 + j * CELL}H${X0 + COLUMNS * CELL}`;
    lab.el(svg, "path", { d: grid, class: "grid" });
    QUADS.forEach(function (q) {
        lab.el(svg, "rect", { x: X0 + q.x * CELL, y: Y0 + q.y * CELL, width: q.w * CELL, height: q.h * CELL, class: "nofill box thick" });
        lab.el(svg, "text", { x: X0 + q.x * CELL + 6, y: Y0 + q.y * CELL - 4, class: "small halo", text: `${q.name}, depth ${q.depth}` });
    });
    lab.el(svg, "text", { x: X0, y: Y0 + ROWS * CELL + 26, class: "small", text: "depth 1 is nearest; each pixel shows its count" });

    lab.draw(function () {
        const sequence = order.value === "back" ? [2, 1, 0] : order.value === "front" ? [0, 1, 2] : [1, 0, 2];
        let shaded = 0;
        let covered = 0;
        for (let j = 0; j < ROWS; ++j) {
            for (let i = 0; i < COLUMNS; ++i) {
                let depth = Infinity;
                let count = 0;
                sequence.forEach(function (k) {
                    const q = QUADS[k];
                    if (i < q.x || i >= q.x + q.w || j < q.y || j >= q.y + q.h)
                        return;
                    if (depthTest.value && !(q.depth < depth))
                        return;
                    depth = q.depth;
                    ++count;
                });
                const n = j * COLUMNS + i;
                lab.set(cells[n], { fill: heat(count) });
                lab.set(labels[n], { text: count > 0 ? String(count) : "" });
                shaded += count;
                covered += count > 0 ? 1 : 0;
            }
        }
        shadedOut.set(String(shaded), "gpu");
        coveredOut.set(String(covered));
        const waste = shaded - covered;
        wasteOut.set(String(waste), waste === 0 ? "ok" : "bad");
        if (!depthTest.value)
            note.set("Without the depth test every fragment is shaded, in any order: the count is just how many quads cover the pixel.", "bad");
        else if (waste === 0)
            note.set("Front to back: the nearest quad writes its depth first and the others fail the test before shading. Each pixel shades once.", "ok");
        else
            note.set("A nearer quad drawn after a farther one passes the test again: the pixel shades twice or more, and only the last one shows.", "bad");
    });
});
