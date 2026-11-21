// Labs of docs/guides/bloom.html.

// One row of pixels with a bright spot, through the bloom chain of the page in one dimension: each down level samples
// the level above at five bilinear taps with the 13-tap filter's weights along a row (1/8, 1/4, 1/4, 1/4, 1/8), each
// up level adds a three-tap tent of the level below, and the last one divides by the levels it added. The chain stops
// at the last level of at least 2 pixels. The tone map mixes the bloom in and maps grays with the page's AgX curve.
RasmLab.register("bloom-chain", function (lab) {
    const svg = lab.svg(760, 330, "One row of pixels with a bright spot in the middle. Top: the pixels on screen after the bloom and the tone curve. Middle: the light along the row on a log scale: the scene, the bloom and their mix. Bottom: the chain's levels and which of them run.");
    const brightness = lab.slider({ label: "Spot brightness (stops above 1)", min: 0, max: 12, step: 1, value: 8, format: function (v) { return `${v} (× ${Math.pow(2, v)})`; } });
    const strength = lab.slider({ label: "Bloom strength", min: 0, max: 0.2, step: 0.01, value: 0.04, format: function (v) { return v.toFixed(2); } });
    const width = lab.choice({ label: "Source width", options: [{ value: "128", label: "128 px" }, { value: "24", label: "24 px" }, { value: "6", label: "6 px" }], value: "128" });
    const levelsOut = lab.readout("levels that run");
    const energyOut = lab.readout("light the bloom keeps");
    const glowOut = lab.readout("pixels made brighter");
    const note = lab.note();

    const LEVELS = 6;
    const MIN_LEVEL_SIZE = 2;
    const BACKGROUND = 0.05;
    const PLOT_X = 40;
    const PLOT_W = 700;
    const PLOT_TOP = 96;
    const PLOT_BOTTOM = 246;
    const EV_MIN = -6;
    const EV_MAX = 13;

    // AgX for a gray, as on the tone-mapping page: a gray stays gray through both matrices.
    function agxGray(x) {
        const ev = Math.max(-12.47393, Math.min(4.026069, Math.log2(Math.max(x, 1e-10))));
        const t = (ev + 12.47393) / 16.5;
        const t2 = t * t;
        const t4 = t2 * t2;
        const v = 15.5 * t4 * t2 - 40.14 * t4 * t + 31.96 * t4 - 6.868 * t2 * t + 0.4298 * t2 + 0.1191 * t - 0.00232;
        return Math.pow(Math.max(0, Math.min(1, v)), 2.2);
    }
    function toByte(linear) {
        const s = linear <= 0.0031308 ? 12.92 * linear : 1.055 * Math.pow(linear, 1 / 2.4) - 0.055;
        return Math.round(255 * Math.max(0, Math.min(1, s)));
    }
    // A bilinear lookup at texel position p (texel centers at whole numbers), clamped at the edges.
    function sample(row, p) {
        const i = Math.floor(p);
        const f = p - i;
        const at = function (k) { return row[Math.max(0, Math.min(row.length - 1, k))]; };
        return at(i) * (1 - f) + at(i + 1) * f;
    }
    // The position, in the source's texels, of target texel x's center.
    function sourcePosition(x, targetSize, sourceSize) {
        return (x + 0.5) / targetSize * sourceSize - 0.5;
    }
    function downsample(source, size) {
        const taps = [[-2, 0.125], [-1, 0.25], [0, 0.25], [1, 0.25], [2, 0.125]];
        const out = [];
        for (let x = 0; x < size; x++) {
            const p = sourcePosition(x, size, source.length);
            out.push(taps.reduce(function (sum, tap) { return sum + tap[1] * sample(source, p + tap[0]); }, 0));
        }
        return out;
    }
    function upsample(base, lower, scale) {
        return base.map(function (value, x) {
            let tent = 0;
            if (lower) {
                const p = sourcePosition(x, base.length, lower.length);
                tent = 0.25 * sample(lower, p - 1) + 0.5 * sample(lower, p) + 0.25 * sample(lower, p + 1);
            }
            return (value + tent) * scale;
        });
    }
    function levelCount(sizes) {
        let count = 0;
        while (count < sizes.length && sizes[count] >= MIN_LEVEL_SIZE) count++;
        return Math.max(count, 1);
    }

    const cells = lab.el(svg, "g", {});
    lab.el(svg, "text", { x: PLOT_X, y: 22, class: "small", text: "on screen, after the bloom and the tone curve" });
    lab.el(svg, "rect", { x: PLOT_X, y: PLOT_TOP, width: PLOT_W, height: PLOT_BOTTOM - PLOT_TOP, class: "zone" });
    let grid = "";
    for (let ev = EV_MIN; ev <= EV_MAX; ev += 2) {
        const y = PLOT_BOTTOM - (ev - EV_MIN) / (EV_MAX - EV_MIN) * (PLOT_BOTTOM - PLOT_TOP);
        grid += `M${PLOT_X},${y.toFixed(1)}H${PLOT_X + PLOT_W}`;
    }
    lab.el(svg, "path", { d: grid, class: "grid" });
    lab.el(svg, "text", { x: PLOT_X - 6, y: PLOT_TOP + 4, class: "end small", text: "8192" });
    lab.el(svg, "text", { x: PLOT_X - 6, y: PLOT_BOTTOM + 4, class: "end small", text: "1/64" });
    lab.el(svg, "text", { x: PLOT_X, y: PLOT_TOP - 8, class: "small", text: "light along the row, log scale: one grid line = 4 times" });
    const sceneLine = lab.el(svg, "path", { class: "arrow mem dashed" });
    const bloomLine = lab.el(svg, "path", { class: "arrow gpu thick" });
    const mixLine = lab.el(svg, "path", { class: "arrow shader" });
    const levelGroup = lab.el(svg, "g", {});

    function plotY(value) {
        const ev = Math.max(EV_MIN, Math.min(EV_MAX, Math.log2(Math.max(value, 1e-6))));
        return PLOT_BOTTOM - (ev - EV_MIN) / (EV_MAX - EV_MIN) * (PLOT_BOTTOM - PLOT_TOP);
    }
    function path(values) {
        const step = PLOT_W / values.length;
        return values.map(function (v, x) { return `${x === 0 ? "M" : "L"}${(PLOT_X + (x + 0.5) * step).toFixed(1)},${plotY(v).toFixed(1)}`; }).join(" ");
    }

    lab.draw(function () {
        const n = Number(width.value);
        const scene = [];
        for (let x = 0; x < n; x++) scene.push(BACKGROUND);
        scene[n >> 1] = Math.pow(2, brightness.value);

        const sizes = [];
        for (let i = 1; i <= LEVELS; i++) sizes.push(Math.max(1, n >> i));
        const levels = levelCount(sizes);
        const down = [];
        let source = scene;
        for (let i = 0; i < levels; i++) {
            source = downsample(source, sizes[i]);
            down.push(source);
        }
        let up = levels > 1 ? down[levels - 1] : null;
        for (let i = levels - 2; i >= 0; i--) up = upsample(down[i], up, i === 0 ? 1 / levels : 1);
        if (levels === 1) up = upsample(down[0], null, 1);

        const bloom = scene.map(function (v, x) { return sample(up, sourcePosition(x, n, up.length)); });
        const mixed = scene.map(function (v, x) { return v + (bloom[x] - v) * strength.value; });

        lab.clear(cells);
        const cellW = PLOT_W / n;
        let brighter = 0;
        mixed.forEach(function (v, x) {
            const byte = toByte(agxGray(v));
            if (byte > toByte(agxGray(scene[x]))) brighter++;
            lab.el(cells, "rect", { x: PLOT_X + x * cellW, y: 30, width: n > 64 ? cellW + 0.6 : Math.max(cellW - 1, 1), height: 34, fill: `rgb(${byte}, ${byte}, ${byte})` });
        });
        lab.set(sceneLine, { d: path(scene) });
        lab.set(bloomLine, { d: path(bloom) });
        lab.set(mixLine, { d: path(mixed) });

        lab.clear(levelGroup);
        lab.el(levelGroup, "text", { x: PLOT_X, y: 276, class: "small", text: "the chain's levels (each half the one before):" });
        sizes.forEach(function (size, i) {
            const runs = i < levels;
            lab.el(levelGroup, "text", { x: PLOT_X + i * 117, y: 298, class: runs ? "small t-ok" : "small t-faint", text: `1/${1 << (i + 1)}: ${size} px` });
            lab.el(levelGroup, "text", { x: PLOT_X + i * 117, y: 314, class: runs ? "small t-ok" : "small t-faint", text: runs ? "runs" : "too small" });
        });

        const sceneSum = scene.reduce(function (a, b) { return a + b; }, 0);
        const bloomSum = bloom.reduce(function (a, b) { return a + b; }, 0);
        levelsOut.set(`${levels} of ${LEVELS}`, levels === LEVELS ? "ok" : "hot");
        energyOut.set(`${Math.round(100 * bloomSum / sceneSum)} %`, "gpu");
        glowOut.set(`${brighter} of ${n}`);
        if (levels < LEVELS)
            note.set(`A ${n}-pixel source halves to under 2 pixels after level ${levels}, so the chain stops there: a smaller glow, and no level of zero size.`, "ok");
        else if (strength.value === 0)
            note.set("Strength 0: the image is the scene alone, and the passes would dispatch nothing.", "ok");
        else
            note.set(`The spot keeps its light but spreads a little of it (${Math.round(strength.value * 100)} %) over a wide area, sharp near the spot and soft far away.`, "ok");
    });
});
