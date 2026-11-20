// Labs of docs/guides/projection-depth.html.

// A distance with about three digits.
function projectionDepthDistance(v) {
    if (v >= 100) return v.toFixed(0);
    if (v >= 10) return v.toFixed(1);
    if (v >= 1) return v.toFixed(2);
    return v.toPrecision(2);
}

// The depth formulas of the page's table, 0..1 clip range.
function projectionDepthStandard(d, n, f) { return f * (d - n) / (d * (f - n)); }
function projectionDepthReversed(d, n, f) { return n * (f - d) / (d * (f - n)); }

// The depth range as a bar: how much of it the distances before and after D get.
RasmLab.register("depth-range", function (lab) {
    const svg = lab.svg(760, 250, "A distance axis on a log scale with the near plane, the far plane and a distance D. Below it, the depth range from 0 to 1 as two bars, standard and reversed, each cut at the depth of D.");
    const log = function (v) { return projectionDepthDistance(Math.pow(10, v)); };
    const near = lab.slider({ label: "Near plane", min: -2, max: 0, step: 0.05, value: -1, format: log });
    const far = lab.slider({ label: "Far plane", min: 2, max: 4, step: 0.05, value: 3, format: log });
    const dist = lab.slider({ label: "Distance D", min: -2, max: 4, step: 0.05, value: 1, format: log });
    const standardOut = lab.readout("standard depth at D");
    const reversedOut = lab.readout("reversed depth at D");
    const shareOut = lab.readout("of the depth range, for everything past D");
    const note = lab.note();

    const X0 = 60;
    const X1 = 720;
    function ax(d) { return X0 + (Math.log10(d) + 2) * (X1 - X0) / 6; }
    function bx(depth) { return X0 + (X1 - X0) * depth; }

    lab.el(svg, "text", { x: X0, y: 22, class: "small", text: "distance from the camera (log scale)" });
    const frustum = lab.el(svg, "rect", { y: 50, height: 20, class: "zone" });
    lab.el(svg, "line", { x1: X0, y1: 70, x2: X1, y2: 70, class: "arrow" });
    ["0.01", "0.1", "1", "10", "100", "1000", "10000"].forEach(function (label, k) {
        const x = X0 + k * (X1 - X0) / 6;
        lab.el(svg, "line", { x1: x, y1: 70, x2: x, y2: 76, class: "arrow" });
        lab.el(svg, "text", { x: x, y: 89, class: "mid small", text: label });
    });
    const nearLine = lab.el(svg, "line", { y1: 46, y2: 70, class: "arrow thick" });
    const farLine = lab.el(svg, "line", { y1: 46, y2: 70, class: "arrow thick" });
    const nearText = lab.el(svg, "text", { y: 40, class: "end small", text: "near" });
    const farText = lab.el(svg, "text", { y: 40, class: "small", text: "far" });
    const dDot = lab.el(svg, "circle", { cy: 70, r: 6, class: "accent" });
    const dText = lab.el(svg, "text", { y: 108, class: "mid small t-hot" });

    lab.el(svg, "text", { x: X0, y: 134, class: "small", text: "standard depth: 0 at the near plane, 1 at the far plane" });
    const standardBefore = lab.el(svg, "rect", { x: X0, y: 140, height: 24, class: "box" });
    const standardAfter = lab.el(svg, "rect", { y: 140, height: 24, class: "bad" });
    lab.el(svg, "text", { x: X0 - 8, y: 157, class: "end small", text: "0" });
    lab.el(svg, "text", { x: X1 + 8, y: 157, class: "small", text: "1" });
    const beforeText = lab.el(svg, "text", { x: X0, y: 180, class: "small" });
    const afterText = lab.el(svg, "text", { x: X1, y: 180, class: "end small t-bad" });

    lab.el(svg, "text", { x: X0, y: 208, class: "small", text: "reversed depth: 1 at the near plane, 0 at the far plane" });
    const reversedAfter = lab.el(svg, "rect", { x: X0, y: 214, height: 24, class: "bad" });
    const reversedBefore = lab.el(svg, "rect", { y: 214, height: 24, class: "box" });
    lab.el(svg, "text", { x: X0 - 8, y: 231, class: "end small", text: "0" });
    lab.el(svg, "text", { x: X1 + 8, y: 231, class: "small", text: "1" });

    lab.draw(function () {
        const n = Math.pow(10, near.value);
        const f = Math.pow(10, far.value);
        const d = lab.clamp(Math.pow(10, dist.value), n, f);
        const standard = projectionDepthStandard(d, n, f);
        const reversed = projectionDepthReversed(d, n, f);

        lab.set(frustum, { x: ax(n), width: ax(f) - ax(n) });
        lab.set(nearLine, { x1: ax(n), x2: ax(n) });
        lab.set(farLine, { x1: ax(f), x2: ax(f) });
        lab.set(nearText, { x: ax(n) + 4 });
        lab.set(farText, { x: ax(f) - 4 });
        lab.set(dDot, { cx: ax(d) });
        lab.set(dText, { x: lab.clamp(ax(d), 80, 700), text: `D = ${projectionDepthDistance(d)}` });

        lab.set(standardBefore, { width: bx(standard) - X0 });
        lab.set(standardAfter, { x: bx(standard), width: X1 - bx(standard) });
        lab.set(reversedAfter, { width: bx(reversed) - X0 });
        lab.set(reversedBefore, { x: bx(reversed), width: X1 - bx(reversed) });
        const share = 100 * (1 - standard);
        const shareText = share < 0.1 ? share.toFixed(3) : share.toFixed(1);
        lab.set(beforeText, { text: `near to D, ${projectionDepthDistance(d - n)} units: ${(100 - share).toFixed(share < 0.1 ? 3 : 1)}% of the range` });
        lab.set(afterText, { text: `D to far, ${projectionDepthDistance(f - d)} units: ${shareText}%` });

        standardOut.set(standard.toFixed(6));
        reversedOut.set(reversed.toFixed(6));
        shareOut.set(`${shareText}%`, share < 5 && f - d > d - n ? "bad" : "");
        if (d >= f) note.set("D is at the far plane: standard depth is 1 and reversed depth is 0.");
        else if (d <= n) note.set("D is at the near plane: standard depth is 0 and reversed depth is 1.");
        else if (share < 5 && f - d > d - n) note.set(`Everything from ${projectionDepthDistance(d)} to the far plane, ${projectionDepthDistance(f - d)} units, shares ${shareText}% of the depth range. D is only ${projectionDepthDistance(d / n)} times the near distance away.`, "bad");
        else note.set(`The ${projectionDepthDistance(d - n)} units before D take ${(100 - share).toFixed(1)}% of the depth range, and the ${projectionDepthDistance(f - d)} units after it take ${shareText}%.`);
    });
});

// Two surfaces close together, far away: the 32-bit floats their depths round to, standard and reversed.
RasmLab.register("z-fight", function (lab) {
    const svg = lab.svg(760, 270, "Two rulers whose ticks are neighboring 32-bit floats: one for standard depth, one for reversed depth. Each shows the floats that two close surfaces are stored as. With standard depth and a far distance, both land on the same float.");
    const GAPS = [0.001, 0.003, 0.01, 0.03, 0.1, 0.3, 1, 3];
    const near = lab.slider({ label: "Near plane", min: -2, max: 0, step: 0.05, value: -1, format: function (v) { return projectionDepthDistance(Math.pow(10, v)); } });
    const dist = lab.slider({ label: "Distance D", min: 1, max: 990, step: 1, value: 900 });
    const gap = lab.slider({ label: "Gap between the surfaces", min: 0, max: GAPS.length - 1, step: 1, value: 5, format: function (v) { return String(GAPS[v]); } });
    const standardOut = lab.readout("floats apart, standard depth");
    const reversedOut = lab.readout("floats apart, reversed depth");
    const stepOut = lab.readout("smallest step standard depth can store at D");
    const note = lab.note();

    const FAR = 1000;
    const TICK0 = 150;
    const SPACING = 57.5;
    const f32 = new Float32Array(1);
    const u32 = new Uint32Array(f32.buffer);
    // the position of a positive float among all floats: neighbors differ by 1
    function floatIndex(v) { f32[0] = v; return u32[0]; }
    function floatAt(index) { u32[0] = index; return f32[0]; }

    const title = lab.el(svg, "text", { x: 20, y: 24, class: "small" });
    function ruler(y, name, color) {
        lab.el(svg, "text", { x: 20, y: y + 4, class: `title t-${color}`, text: name });
        const cells = lab.el(svg, "g");
        lab.el(svg, "line", { x1: TICK0 - 30, y1: y, x2: TICK0 + 8 * SPACING + 30, y2: y, class: `arrow ${color}` });
        let ticks = "";
        for (let k = 0; k < 9; k++) ticks += `M${TICK0 + k * SPACING},${y - 10}v20`;
        lab.el(svg, "path", { d: ticks, class: `arrow ${color}` });
        return {
            y: y,
            cells: cells,
            away: lab.el(svg, "line", { class: "arrow", "marker-end": "url(#ah)" }),
            awayText: lab.el(svg, "text", { y: y - 22, class: "small" }),
            first: lab.el(svg, "text", { y: y - 22, class: "mid small" }),
            second: lab.el(svg, "text", { y: y - 22, class: "mid small" }),
            stored: lab.el(svg, "text", { x: TICK0 - 30, y: y + 34, class: "mono small" }),
            verdict: lab.el(svg, "text", { x: 744, y: y + 34, class: "end small" }),
        };
    }
    const standard = ruler(96, "standard", "cpu");
    const reversed = ruler(206, "reversed", "hot");
    lab.el(svg, "text", { x: 20, y: 262, class: "small t-faint", text: "Each tick is one 32-bit float. A surface is stored as the tick nearest to its depth." });

    // Fills one ruler; returns how many floats lie between the two stored depths.
    function show(row, depthA, depthB, nameA, nameB) {
        const a = floatIndex(depthA);
        const b = floatIndex(depthB);
        const steps = b - a;      // signed: where B sits relative to A
        const center = TICK0 + 4 * SPACING;
        lab.clear(row.cells);
        function cell(offset, cls) {
            lab.el(row.cells, "rect", { x: center + offset * SPACING - SPACING / 2 + 2, y: row.y - 14, width: SPACING - 4, height: 28, rx: 5, class: cls });
        }
        const inView = Math.abs(steps) <= 4;
        if (steps === 0) {
            cell(0, "bad");
            lab.set(row.first, { x: center, text: `${nameA} and ${nameB}`, class: "mid small t-bad" });
            lab.set(row.second, { text: "" });
        } else {
            cell(0, "ok");
            lab.set(row.first, { x: center, text: nameA, class: "mid small" });
            if (inView) cell(steps, "ok");
            lab.set(row.second, { x: center + steps * SPACING, text: inView ? nameB : "" });
        }
        const side = steps > 0 ? 1 : -1;
        lab.set(row.away, inView
            ? { class: "hide" }
            : { class: "arrow", x1: center + side * (SPACING / 2 + 6), y1: row.y - 26, x2: center + side * (4 * SPACING + 24), y2: row.y - 26 });
        lab.set(row.awayText, {
            x: center + side * 60,
            y: row.y - 34,
            class: inView ? "hide" : (side > 0 ? "small" : "end small"),
            text: `${nameB}: ${Math.abs(steps).toLocaleString("en-US")} floats away`,
        });
        lab.set(row.stored, { text: `stored: ${floatAt(a).toPrecision(8)} and ${floatAt(b).toPrecision(8)}` });
        const count = Math.abs(steps);
        lab.set(row.verdict, {
            text: count === 0 ? "the same float: z-fighting" : `${count.toLocaleString("en-US")} float${count === 1 ? "" : "s"} apart`,
            class: `end small ${count === 0 ? "t-bad" : "t-ok"}`,
        });
        return count;
    }

    lab.draw(function () {
        const n = Math.pow(10, near.value);
        const d = dist.value;
        const g = GAPS[gap.value];
        const nameA = String(d);
        const nameB = String(Number((d + g).toFixed(3)));
        lab.set(title, { text: `Near ${projectionDepthDistance(n)}, far ${FAR}. One surface at distance ${nameA}, a second one right behind it at ${nameB}.` });
        const s = show(standard, projectionDepthStandard(d, n, FAR), projectionDepthStandard(d + g, n, FAR), nameA, nameB);
        const r = show(reversed, projectionDepthReversed(d, n, FAR), projectionDepthReversed(d + g, n, FAR), nameA, nameB);

        // the page's formula: float gap at the stored depth, divided by how fast depth changes with distance
        const at = floatIndex(projectionDepthStandard(d, n, FAR));
        const floatGap = floatAt(at + 1) - floatAt(at);
        const slope = FAR * n / (d * d * (FAR - n));
        const step = floatGap / slope;
        standardOut.set(s.toLocaleString("en-US"), s === 0 ? "bad" : "ok");
        reversedOut.set(r.toLocaleString("en-US"), r === 0 ? "bad" : "ok");
        stepOut.set(step >= 0.01 ? step.toFixed(2) : step.toExponential(1).replace("e-", " × 10^−"), step > g ? "bad" : "");
        if (s === 0 && r === 0) note.set("Both forms round the two surfaces to the same float. The gap is too small for any 32-bit depth buffer at this distance.", "bad");
        else if (s === 0) note.set(`Z-fighting with standard depth: both surfaces are stored as the same float, so the depth test cannot order them. Reversed depth keeps them ${r.toLocaleString("en-US")} floats apart.`, "bad");
        else if (s <= 2) note.set(`Standard depth keeps the surfaces only ${s} float${s === 1 ? "" : "s"} apart: the smallest rounding error on the way can swap them. Reversed depth has ${r.toLocaleString("en-US")} floats between them.`, "bad");
        else note.set(`Both forms tell the surfaces apart here: ${s.toLocaleString("en-US")} floats with standard depth, ${r.toLocaleString("en-US")} with reversed depth.`, "ok");
    });
});
