// Labs of docs/guides/inspecting-gpu-resources.html.

// Posts at view depths from 0.5 m to 64 m, seen from the side, and two strips of their grays: the stored reversed-Z
// depth with an infinite far plane (d = near / z, clamped to 1), and the linear view depth the inspector shows
// (1 - z / range, clamped to 0..1). A post whose gray is under 2 % looks like the sky behind it (depth 0).
RasmLab.register("reversed-z-linear", function (lab) {
    const DEPTHS = [0.5, 1, 2, 4, 8, 16, 32, 64];
    const X0 = 120;
    const X1 = 700;
    const STEP = 0.02;

    const svg = lab.svg(760, 330, "A camera on the left and eight posts at view depths 0.5, 1, 2, 4, 8, 16, 32 and 64 meters, on a doubling scale. Under them, two rows of gray swatches: the stored reversed-Z depth of each post, and its linear depth shown with a range.");
    const near = lab.slider({ label: "Near plane", min: 0.05, max: 1, step: 0.05, value: 0.1, format: function (v) { return v.toFixed(2) + " m"; } });
    const range = lab.slider({ label: "Range", min: 5, max: 200, step: 5, value: 50, format: function (v) { return v + " m"; } });
    const storedOut = lab.readout("stored d at 8 m");
    const rawOut = lab.readout("posts like the sky, stored");
    const linearOut = lab.readout("posts like the sky, linear");
    const note = lab.note();

    function xOf(i) {
        return X0 + (X1 - X0) * i / (DEPTHS.length - 1);
    }

    // The side view: the camera, the ground, the posts.
    lab.el(svg, "path", { d: "M20,96 H750", class: "surface" });
    lab.el(svg, "path", { d: "M26,60 L60,48 L60,72 Z", class: "solid" });
    lab.el(svg, "text", { x: 20, y: 40, class: "small", text: "camera" });
    DEPTHS.forEach(function (z, i) {
        lab.el(svg, "rect", { x: xOf(i) - 6, y: 50, width: 12, height: 46, class: "solid" });
        lab.el(svg, "text", { x: xOf(i), y: 116, class: "mid small", text: `${z} m` });
    });
    lab.el(svg, "text", { x: 20, y: 136, class: "small t-faint", text: "view depth doubles from post to post" });

    lab.el(svg, "text", { x: 20, y: 176, class: "small", text: "stored d" });
    lab.el(svg, "text", { x: 20, y: 192, class: "small t-faint", text: "near / z" });
    lab.el(svg, "text", { x: 20, y: 256, class: "small", text: "linear" });
    lab.el(svg, "text", { x: 20, y: 272, class: "small t-faint", text: "1 − z / range" });
    const raw = [];
    const linear = [];
    const rawText = [];
    const linearText = [];
    DEPTHS.forEach(function (z, i) {
        // The gray is data, so the fill is set by draw.
        raw.push(lab.el(svg, "rect", { x: xOf(i) - 30, y: 154, width: 60, height: 44 }));
        lab.el(svg, "rect", { x: xOf(i) - 30, y: 154, width: 60, height: 44, class: "nofill box" });
        rawText.push(lab.el(svg, "text", { x: xOf(i), y: 214, class: "mid small" }));
        linear.push(lab.el(svg, "rect", { x: xOf(i) - 30, y: 234, width: 60, height: 44 }));
        lab.el(svg, "rect", { x: xOf(i) - 30, y: 234, width: 60, height: 44, class: "nofill box" });
        linearText.push(lab.el(svg, "text", { x: xOf(i), y: 294, class: "mid small" }));
    });
    const rangeMark = lab.el(svg, "path", { class: "arrow hot dashed" });
    const rangeText = lab.el(svg, "text", { y: 320, class: "mid small t-hot" });

    function gray(v) {
        const c = Math.round(255 * Math.min(Math.max(v, 0), 1));
        return `rgb(${c},${c},${c})`;
    }

    function black(values) {
        return values.filter(function (v) { return v < STEP; }).length;
    }

    lab.draw(function () {
        const n = near.value;
        const r = range.value;
        const stored = DEPTHS.map(function (z) { return Math.min(n / z, 1); });
        const shown = DEPTHS.map(function (z) { return Math.min(Math.max(1 - z / r, 0), 1); });
        DEPTHS.forEach(function (z, i) {
            lab.set(raw[i], { fill: gray(stored[i]) });
            lab.set(rawText[i], { text: stored[i].toFixed(3) });
            lab.set(linear[i], { fill: gray(shown[i]) });
            lab.set(linearText[i], { text: shown[i].toFixed(2) });
        });
        // The range on the doubling scale of the posts.
        const t = Math.min(Math.max(Math.log2(r / DEPTHS[0]) / (DEPTHS.length - 1), 0), 1);
        const x = X0 + (X1 - X0) * t;
        lab.set(rangeMark, { d: `M${x},140 V300` });
        lab.set(rangeText, { x: Math.min(Math.max(x, 60), 700), text: `range ${r} m` });

        const rawBlack = black(stored);
        const linearBlack = black(shown);
        storedOut.set((n / 8).toFixed(4), "mem");
        rawOut.set(`${rawBlack} of ${DEPTHS.length}`, rawBlack === 0 ? "ok" : "bad");
        linearOut.set(`${linearBlack} of ${DEPTHS.length}`, linearBlack === 0 ? "ok" : "bad");
        if (linearBlack > 0)
            note.set(`${linearBlack} post${linearBlack > 1 ? "s are" : " is"} at or near the range, so black like the sky: raise the range past 64 m.`, "bad");
        else
            note.set(`Every post has a gray of its own, in steps of meters. The stored depth puts everything past ${(n / STEP).toFixed(0)} m under 2 %, like the sky.`, "ok");
    });
});
