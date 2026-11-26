// Labs of docs/guides/weighted-oit.html.

// One pixel's see-through layers on a view ray, seen from the side. The exact color mixes them back to front with the
// over operator; weighted blending adds them in any order, each times alpha times its weight, divides by the sum of
// alpha times weight, and lays the mean over the background by the coverage 1 - product(1 - alpha). A layer's weight
// comes from its place in the range of the view's see-through draws (the bracket), not in this pixel's layers. The
// reader drags the layers and picks the weight.
RasmLab.register("weighted-pixel", function (lab) {
    const svg = lab.svg(760, 260, "A side view: an eye on the left looks right through three see-through layers, red, green and blue, at the depths you drag them to. On the right, the exact color (sorted, back to front), the weighted color, and their coverage, which is the same.");
    const weightChoice = lab.choice({ label: "Weight", options: [{ value: "none", label: "none (plain mean)" }, { value: "depth", label: "falls with depth" }], value: "depth" });
    const alphaSlider = lab.slider({ label: "Alpha of each layer", min: 0.1, max: 0.9, step: 0.05, value: 0.4, format: function (v) { return v.toFixed(2); } });
    const errorOut = lab.readout("difference from exact");
    const coverOut = lab.readout("coverage");
    const note = lab.note();

    const EYE_X = 40;
    const NEAR = 90;
    const FAR = 440;
    const Y = 120;
    const HALVINGS = 5;
    const BACKGROUND = [0.85, 0.85, 0.85];
    const layers = [
        { name: "red", color: [0.9, 0.15, 0.1], x: 200 },
        { name: "green", color: [0.15, 0.75, 0.2], x: 250 },
        { name: "blue", color: [0.1, 0.25, 0.9], x: 300 },
    ];

    lab.el(svg, "text", { x: 20, y: 24, class: "title", text: "one pixel, seen from the side" });
    lab.el(svg, "circle", { cx: EYE_X, cy: Y, r: 10, class: "box" });
    lab.el(svg, "text", { x: EYE_X, y: Y + 30, class: "mid small", text: "eye" });
    lab.el(svg, "line", { x1: EYE_X + 10, y1: Y, x2: FAR + 30, y2: Y, class: "arrow light dashed" });
    lab.el(svg, "rect", { x: FAR + 30, y: 40, width: 18, height: 160, class: "solid" });
    lab.el(svg, "text", { x: FAR + 39, y: 222, class: "mid small", text: "background" });
    lab.el(svg, "path", { d: `M${NEAR},236 V244 H${FAR} V236`, class: "arrow gpu" });
    lab.el(svg, "text", { x: (NEAR + FAR) / 2, y: 232, class: "mid small t-gpu", text: "the view's see-through range: place 0 to 1" });

    const els = layers.map(function (layer, i) {
        const rect = lab.el(svg, "rect", { y: 50, width: 12, height: 140, fill: rgb(layer.color), "fill-opacity": 0.75 });
        const weight = lab.el(svg, "text", { y: 44, class: "mid small t-faint" });
        const handle = lab.handle(svg, { x: layer.x, y: Y + 40 + i * 16, label: `Move the ${layer.name} layer`, minX: NEAR, maxX: FAR, minY: Y + 40 + i * 16, maxY: Y + 40 + i * 16 });
        return { rect: rect, weight: weight, handle: handle };
    });

    lab.el(svg, "text", { x: 540, y: 34, class: "title", text: "exact" });
    const exact = lab.el(svg, "rect", { x: 540, y: 42, width: 90, height: 54, rx: 4 });
    lab.el(svg, "text", { x: 650, y: 34, class: "title", text: "weighted" });
    const weighted = lab.el(svg, "rect", { x: 650, y: 42, width: 90, height: 54, rx: 4 });
    lab.el(svg, "text", { x: 540, y: 124, class: "small", text: "coverage: exact and weighted" });
    lab.el(svg, "rect", { x: 540, y: 132, width: 200, height: 14, class: "zone" });
    const coverBar = lab.el(svg, "rect", { x: 540, y: 132, height: 14, class: "gpu" });
    lab.el(svg, "text", { x: 540, y: 176, class: "small", text: "mean = sum(C a w) / sum(a w)" });
    lab.el(svg, "text", { x: 540, y: 194, class: "small", text: "result = mean c + background (1 - c)" });
    lab.el(svg, "text", { x: 540, y: 212, class: "small", text: "c = 1 - product(1 - a)" });

    function rgb(c) {
        const to = function (v) { return Math.round(255 * Math.pow(lab.clamp(v, 0, 1), 1 / 2.2)); };
        return `rgb(${to(c[0])}, ${to(c[1])}, ${to(c[2])})`;
    }
    // The place 0 to 1 between the nearest and the farthest layer, as the weights measure it.
    function place(x, near, far) {
        return far > near ? (x - near) / (far - near) : 0;
    }

    lab.draw(function () {
        const alpha = alphaSlider.value;
        layers.forEach(function (layer, i) {
            layer.x = els[i].handle.x;
            lab.set(els[i].rect, { x: layer.x - 6 });
        });
        const near = NEAR;
        const far = FAR;
        // Exact: back to front, over.
        let c = BACKGROUND.slice();
        layers.slice().sort(function (a, b) { return b.x - a.x; }).forEach(function (layer) {
            for (let k = 0; k < 3; k++) c[k] = layer.color[k] * alpha + c[k] * (1 - alpha);
        });
        // Weighted: any order.
        const sum = [0, 0, 0];
        let sumW = 0;
        let reveal = 1;
        layers.forEach(function (layer, i) {
            const w = weightChoice.value === "depth" ? Math.pow(2, -HALVINGS * place(layer.x, near, far)) : 1;
            lab.set(els[i].weight, { x: layer.x, text: `w ${w.toFixed(2)}` });
            for (let k = 0; k < 3; k++) sum[k] += layer.color[k] * alpha * w;
            sumW += alpha * w;
            reveal *= 1 - alpha;
        });
        const coverage = 1 - reveal;
        const got = [0, 1, 2].map(function (k) { return sum[k] / sumW * coverage + BACKGROUND[k] * reveal; });
        lab.set(exact, { fill: rgb(c) });
        lab.set(weighted, { fill: rgb(got) });
        lab.set(coverBar, { width: 200 * coverage });
        const error = Math.max(Math.abs(got[0] - c[0]), Math.abs(got[1] - c[1]), Math.abs(got[2] - c[2]));
        const ok = error < 0.06;
        errorOut.set(`${Math.round(error * 100)} %`, ok ? "ok" : "bad");
        coverOut.set(`${Math.round(coverage * 100)} %`, "gpu");
        if (weightChoice.value === "none")
            note.set(ok ? "Close: the layers are thin or alike, so a plain mean is near the exact mix." : "A plain mean lets the far layer count as much as the near one: the near layer should hide more of it.", ok ? "ok" : "bad");
        else
            note.set(ok ? "The near layer weighs more, as it hides the ones behind it: close to the exact mix, in any order." : "The weights do not match how much each layer hides here (layers far apart, or thick): the coverage is exact, the mix is not.", ok ? "ok" : "bad");
    });
});

// The weight of a few layers across a scene's depth, for a scene drawn at any scale. A weight with fixed metres in
// it (McGuire and Bavoil's eq. 8: 10 / (1e-5 + (z / 5)^2 + (z / 200)^6), kept between 0.01 and 3000) works for a scene
// a few metres deep and loses its depth order when the scene is kilometres away; a weight from the layer's place in
// the see-through draws' own depth range (halving every fifth of it; log of the depth with perspective, the depth
// orthographic) is the same at every scale.
RasmLab.register("weight-scale", function (lab) {
    const svg = lab.svg(760, 250, "A plot of the weight of five layers against their depth in the scene, for a weight with fixed metres and for a weight from the layer's place in the depth range. A slider scales the whole scene from metres to kilometres.");
    const scale = lab.slider({ label: "Scene distance", min: 0, max: 4, step: 0.25, value: 0, format: function (v) { return formatMetres(4 * Math.pow(10, v)); } });
    const projection = lab.choice({ label: "Projection", options: ["perspective", "orthographic"], value: "perspective" });
    const fixedOut = lab.readout("fixed metres: near / far weight");
    const rangeOut = lab.readout("range: near / far weight");
    const note = lab.note();

    const G = { x: 70, y: 30, w: 620, h: 170 };
    // Five layers between 4 and 12 units from the eye, scaled with the scene.
    const LAYERS = [4, 5, 6.5, 9, 12];
    lab.el(svg, "rect", { x: G.x, y: G.y, width: G.w, height: G.h, class: "zone" });
    lab.el(svg, "text", { x: G.x, y: 20, class: "title", text: "weight of each layer (log scale), nearest on the left" });
    lab.el(svg, "text", { x: G.x - 6, y: G.y + 4, class: "end small", text: "max" });
    lab.el(svg, "text", { x: G.x - 6, y: G.y + G.h + 4, class: "end small", text: "1/1000" });
    const depthLabels = LAYERS.map(function (z, i) { return lab.el(svg, "text", { x: lx(i), y: G.y + G.h + 18, class: "mid small" }); });
    const fixedLine = lab.el(svg, "path", { class: "arrow mem thick" });
    const rangeLine = lab.el(svg, "path", { class: "arrow gpu thick" });
    const fixedDots = LAYERS.map(function () { return lab.el(svg, "circle", { r: 5, class: "mem" }); });
    const rangeDots = LAYERS.map(function () { return lab.el(svg, "circle", { r: 5, class: "gpu" }); });
    lab.el(svg, "text", { x: G.x + G.w - 6, y: G.y + 18, class: "end small t-mem", text: "fixed metres" });
    lab.el(svg, "text", { x: G.x + G.w - 6, y: G.y + 34, class: "end small t-gpu", text: "place in the range" });

    function formatMetres(m) {
        return m >= 1000 ? `${(m / 1000).toFixed(m >= 10000 ? 0 : 1)} km` : `${m.toFixed(0)} m`;
    }
    function lx(i) {
        return G.x + 40 + i * (G.w - 80) / (LAYERS.length - 1);
    }
    // log10 of the weight relative to the largest, mapped from 0 (top) down to -3 (bottom).
    function gy(relative) {
        return G.y + G.h * lab.clamp(-Math.log10(relative) / 3, 0, 1);
    }
    function fixedWeight(z) {
        return lab.clamp(10 / (1e-5 + Math.pow(z / 5, 2) + Math.pow(z / 200, 6)), 1e-2, 3e3);
    }
    function rangeWeight(z, near, far, orthographic) {
        const place = orthographic ? (z - near) / (far - near) : Math.log2(z / near) / Math.log2(far / near);
        return Math.pow(2, -5 * lab.clamp(place, 0, 1));
    }

    lab.draw(function () {
        const s = Math.pow(10, scale.value);
        const orthographic = projection.value === "orthographic";
        const zs = LAYERS.map(function (z) { return z * s; });
        const fixed = zs.map(fixedWeight);
        const range = zs.map(function (z) { return rangeWeight(z, zs[0], zs[zs.length - 1], orthographic); });
        const fixedMax = Math.max.apply(null, fixed);
        let fixedPath = "";
        let rangePath = "";
        zs.forEach(function (z, i) {
            lab.set(depthLabels[i], { text: formatMetres(z) });
            const fy = gy(fixed[i] / fixedMax);
            const ry = gy(range[i]);
            lab.set(fixedDots[i], { cx: lx(i), cy: fy });
            lab.set(rangeDots[i], { cx: lx(i), cy: ry });
            fixedPath += `${i === 0 ? "M" : "L"}${lx(i)},${fy}`;
            rangePath += `${i === 0 ? "M" : "L"}${lx(i)},${ry}`;
        });
        lab.set(fixedLine, { d: fixedPath });
        lab.set(rangeLine, { d: rangePath });
        const fixedRatio = fixed[0] / fixed[fixed.length - 1];
        const rangeRatio = range[0] / range[range.length - 1];
        fixedOut.set(`${fixedRatio.toFixed(fixedRatio < 10 ? 2 : 0)} : 1`, fixedRatio > 2 ? "ok" : "bad");
        rangeOut.set(`${rangeRatio.toFixed(0)} : 1`, "ok");
        note.set(fixedRatio > 2 ? "At this distance both weights fall with depth: the near layer counts more." : "The fixed weight is flat here: every layer counts the same, and the far ones bleed through. The range's weight has not changed.", fixedRatio > 2 ? "ok" : "bad");
    });
});
