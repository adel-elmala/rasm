// Labs of docs/guides/large-world-precision.html.

// A house a few metres from a camera that is far from the world's origin, seen from the camera (render space), two
// ways: with plain floats (each world position rounded to a float, then the camera subtracted) and with the high/low
// split (each position as a float plus the float of what it left over, subtracted part by part). Math.fround rounds
// to a 32-bit float, as the GPU does.
RasmLab.register("float-split", function (lab) {
    const svg = lab.svg(760, 320, "Two panels with the same house seen from the camera. Left: plain floats, where the house's corners snap to the float steps of its world position. Right: the high/low split, where the house keeps its shape.");
    const distance = lab.slider({ label: "Distance from the origin (10^x m)", min: 0, max: 10, step: 0.05, value: 3, format: function (v) { return "10^" + v.toFixed(2) + " m"; } });
    const nudge = lab.slider({ label: "Move the camera (m)", min: 0, max: 1, step: 0.01, value: 0, format: function (v) { return v.toFixed(2) + " m"; } });
    const stepOut = lab.readout("float step there");
    const floatOut = lab.readout("plain floats: worst corner");
    const splitOut = lab.readout("high/low split: worst corner");
    const note = lab.note();

    const SCALE = 52;                                   // pixels per metre
    const PANELS = [{ x: 10, title: "Plain floats", cls: "bad" }, { x: 390, title: "High/low split", cls: "ok" }];
    const DIRECTION = [0.6, 0.8];                       // where the camera is, seen from the origin
    const HOUSE = [[-1, -1.1], [1, -1.1], [1, 0.3], [0, 1.1], [-1, 0.3]];
    const OFFSET = [3.37, 1.21];                        // the house's centre, from the camera

    const shapes = [];
    PANELS.forEach(function (panel, i) {
        lab.el(svg, "rect", { x: panel.x, y: 10, width: 360, height: 300, rx: 8, class: "zone" });
        lab.el(lab.el(lab.el(svg, "defs"), "clipPath", { id: "lab-split-clip-" + i }), "rect", { x: panel.x + 1, y: 40, width: 358, height: 269 });
        lab.el(svg, "text", { x: panel.x + 14, y: 30, class: "title", text: panel.title });
        const cx = panel.x + 180;
        const cy = 175;
        const grid = [];
        for (let k = -3; k <= 3; k++) grid.push("M" + (cx + k * SCALE) + ",44V306M" + (panel.x + 4) + "," + (cy + k * SCALE * 0.8) + "H" + (panel.x + 356));
        lab.el(svg, "path", { d: grid.join(""), class: "grid" });
        const group = lab.el(svg, "g", { "clip-path": "url(#lab-split-clip-" + i + ")" });
        lab.el(group, "polygon", { points: HOUSE.map(function (p) { return (cx + p[0] * SCALE) + "," + (cy - p[1] * SCALE); }).join(" "), class: "solid o-25" });
        shapes.push({ polygon: lab.el(group, "polygon", { class: "arrow thick " + panel.cls }), cx: cx, cy: cy });
    });
    lab.el(svg, "text", { x: 380, y: 300, class: "mid small t-faint", text: "grey: where the house is; outline: where the GPU draws it (1 grid cell = 1 m)" });

    function split(v) {
        const high = Math.fround(v);
        return [high, Math.fround(v - high)];
    }

    function length(v) {
        if (v >= 1) return v.toFixed(v < 10 ? 2 : 0) + " m";
        if (v >= 1e-3) return (v * 1e3).toFixed(1) + " mm";
        if (v >= 1e-6) return (v * 1e6).toFixed(2) + " µm";
        return (v * 1e9).toFixed(2) + " nm";
    }

    lab.draw(function () {
        const d = Math.pow(10, distance.value);
        const camera = [DIRECTION[0] * d + nudge.value, DIRECTION[1] * d];
        let floatError = 0;
        let splitError = 0;
        const plain = [];
        const pairs = [];
        HOUSE.forEach(function (corner) {
            const local = [corner[0] + OFFSET[0], corner[1] + OFFSET[1]];
            const p = [];
            const q = [];
            for (let a = 0; a < 2; a++) {
                const world = camera[a] + local[a];
                p.push(Math.fround(Math.fround(world) - Math.fround(camera[a])) - OFFSET[a]);
                const w = split(world);
                const c = split(camera[a]);
                const high = Math.fround(w[0] - c[0]);
                const low = Math.fround(w[1] - c[1]);
                q.push(Math.fround(high + low) - OFFSET[a]);
            }
            floatError = Math.max(floatError, Math.hypot(p[0] - corner[0], p[1] - corner[1]));
            splitError = Math.max(splitError, Math.hypot(q[0] - corner[0], q[1] - corner[1]));
            plain.push(p);
            pairs.push(q);
        });
        [plain, pairs].forEach(function (points, i) {
            const shape = shapes[i];
            lab.set(shape.polygon, { points: points.map(function (p) { return (shape.cx + p[0] * SCALE) + "," + (shape.cy - p[1] * SCALE); }).join(" ") });
        });
        const step = Math.pow(2, Math.floor(Math.log2(d * Math.SQRT2)) - 23);
        stepOut.set(length(step), step > 1e-3 ? "bad" : "gpu");
        floatOut.set(length(floatError), floatError > 1e-3 ? "bad" : "ok");
        splitOut.set(length(splitError), splitError > 1e-3 ? "bad" : "ok");
        if (floatError > 1e-3 && splitError <= 1e-3)
            note.set("Plain floats are off by " + length(floatError) + ": the house breaks and jumps as the camera moves. The split keeps it within " + length(splitError) + ".", "bad");
        else if (splitError > 1e-3)
            note.set("Even the split loses millimetres this far out: a world this big needs an origin per region.", "bad");
        else
            note.set("Near the origin both ways are exact to well under a millimetre.", "ok");
    });
});
