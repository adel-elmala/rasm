// Labs of docs/guides/keyframe-animation.html.

// One channel with five keys. The reader drags the time and picks the rule; the curve, the point and the readouts
// come from the same sampling rules as the page: a binary search for the key, then STEP, LINEAR or the cubic
// Hermite curve with the tangents scaled by the gap.
RasmLab.register("sample-channel", function (lab) {
    const svg = lab.svg(760, 290, "A graph of one channel's value over 3.2 seconds with five keys. The curve between the keys follows the chosen rule: steps, straight lines or a smooth cubic curve with a tangent at each key. A handle on the time line sets the time; a point on the curve shows the value there.");
    const rule = lab.choice({ label: "Interpolation", options: ["STEP", "LINEAR", "CUBICSPLINE"], value: "LINEAR" });
    const keyOut = lab.readout("keys around t");
    const sOut = lab.readout("s");
    const valueOut = lab.readout("value");
    const note = lab.note();

    const TIMES = [0.0, 0.8, 1.6, 2.4, 3.2];
    const VALUES = [0.2, 0.9, 0.4, 0.75, 0.3];
    const TANGENTS = [1.2, 0.0, -0.4, 0.0, -0.8];   // slope per second, the same in and out
    const X0 = 70;
    const X1 = 720;
    const Y0 = 230;
    const Y1 = 40;
    const toX = function (t) { return X0 + (t / TIMES[TIMES.length - 1]) * (X1 - X0); };
    const toY = function (v) { return Y0 - v * (Y0 - Y1); };

    lab.el(svg, "line", { x1: X0, y1: Y0, x2: X1 + 20, y2: Y0, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "line", { x1: X0, y1: Y0, x2: X0, y2: Y1 - 20, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: X0 - 8, y: Y1 - 22, class: "end small", text: "value" });
    lab.el(svg, "text", { x: X1 + 24, y: Y0 + 4, class: "small", text: "t" });
    const tangentGroup = lab.el(svg, "g", {});
    const curve = lab.el(svg, "path", { class: "arrow gpu thick" });
    TIMES.forEach(function (t, k) {
        lab.el(svg, "circle", { cx: toX(t), cy: toY(VALUES[k]), r: 6, class: "mem" });
        lab.el(svg, "text", { x: toX(t), y: Y0 + 18, class: "mid small", text: `${t.toFixed(1)} s` });
    });
    const marker = lab.el(svg, "line", { y1: Y0, class: "arrow hot dashed" });
    const point = lab.el(svg, "circle", { r: 6, class: "hot" });
    const handle = lab.handle(svg, { x: toX(1.1), y: Y0 + 38, label: "Drag the time", minX: X0, maxX: X1, minY: Y0 + 38, maxY: Y0 + 38 });

    function findKey(t) {
        let low = 0;
        let high = TIMES.length - 1;
        while (high - low > 1) {
            const middle = Math.floor((low + high) / 2);
            if (TIMES[middle] <= t) low = middle; else high = middle;
        }
        return low;
    }
    function sample(t, how) {
        const last = TIMES.length - 1;
        if (t <= TIMES[0]) return { value: VALUES[0], k: 0, s: 0 };
        if (t >= TIMES[last]) return { value: VALUES[last], k: last, s: 0 };
        const k = findKey(t);
        const d = TIMES[k + 1] - TIMES[k];
        const s = (t - TIMES[k]) / d;
        if (how === "STEP") return { value: VALUES[k], k: k, s: s };
        if (how === "LINEAR") return { value: VALUES[k] + (VALUES[k + 1] - VALUES[k]) * s, k: k, s: s };
        const s2 = s * s;
        const s3 = s2 * s;
        const value = (2 * s3 - 3 * s2 + 1) * VALUES[k] + (s3 - 2 * s2 + s) * d * TANGENTS[k] + (-2 * s3 + 3 * s2) * VALUES[k + 1] + (s3 - s2) * d * TANGENTS[k + 1];
        return { value: value, k: k, s: s };
    }

    lab.draw(function () {
        const how = rule.value;
        let d = "";
        const steps = 320;
        for (let i = 0; i <= steps; i++) {
            const t = (i / steps) * TIMES[TIMES.length - 1];
            d += `${i === 0 ? "M" : "L"}${toX(t).toFixed(1)},${toY(sample(t, how).value).toFixed(1)}`;
        }
        lab.set(curve, { d: d });

        lab.clear(tangentGroup);
        if (how === "CUBICSPLINE") {
            TIMES.forEach(function (t, k) {
                const dt = 0.25;
                lab.el(tangentGroup, "line", { x1: toX(t - dt), y1: toY(VALUES[k] - TANGENTS[k] * dt), x2: toX(t + dt), y2: toY(VALUES[k] + TANGENTS[k] * dt), class: "arrow hot" });
            });
        }

        const t = ((handle.x - X0) / (X1 - X0)) * TIMES[TIMES.length - 1];
        const result = sample(t, how);
        lab.set(marker, { x1: handle.x, x2: handle.x, y2: toY(result.value) });
        lab.set(point, { cx: handle.x, cy: toY(result.value) });
        const last = TIMES.length - 1;
        const outside = t <= TIMES[0] || t >= TIMES[last];
        keyOut.set(outside ? (t <= TIMES[0] ? "before key 0" : `after key ${last}`) : `${result.k} and ${result.k + 1}`);
        sOut.set(outside ? "—" : result.s.toFixed(2));
        const above = result.value > Math.max.apply(null, VALUES) + 1e-6 || result.value < Math.min.apply(null, VALUES) - 1e-6;
        valueOut.set(result.value.toFixed(3), above ? "hot" : "gpu");
        if (outside)
            note.set("Outside the keys the channel holds the first or the last value.", "ok");
        else if (how === "STEP")
            note.set(`STEP holds key ${result.k}'s value until key ${result.k + 1}, then jumps.`, "ok");
        else if (how === "LINEAR")
            note.set(`LINEAR is ${(100 * (1 - result.s)).toFixed(0)} % key ${result.k} and ${(100 * result.s).toFixed(0)} % key ${result.k + 1}.`, "ok");
        else
            note.set(above ? "The cubic curve can pass beyond the keys' values: the tangents push it." : "The cubic curve leaves each key along its tangent, so it has no corners.", above ? "bad" : "ok");
    });
});

// A ball that loops along a track once per 2 seconds, sampled at 60 Hz for 24 frames. Its time is computed the way a
// GPU does, in 32-bit floats (Math.fround): from the clock itself, or from the clock minus a base just behind it.
RasmLab.register("clock-precision", function (lab) {
    const svg = lab.svg(760, 200, "A track with 24 dots: where a looping ball is drawn in 24 frames at 60 frames per second, at the chosen clock time. With even spacing the motion is smooth; with uneven spacing it stutters.");
    const hours = lab.slider({ label: "Clock", min: 0, max: 48, step: 1, value: 0, format: function (v) { return `${v} h`; } });
    const base = lab.toggle({ label: "Subtract a base (64 s window)", value: false });
    const stepOut = lab.readout("float step at the clock");
    const errorOut = lab.readout("worst error in a frame");
    const note = lab.note();

    const LOOP = 2.0;
    const FRAMES = 24;
    const X0 = 40;
    const X1 = 720;
    lab.el(svg, "text", { x: 20, y: 24, class: "title", text: "24 frames of a ball that loops every 2 s" });
    lab.el(svg, "line", { x1: X0, y1: 110, x2: X1, y2: 110, class: "surface" });
    const dots = lab.el(svg, "g", {});
    const label = lab.el(svg, "text", { x: 20, y: 170, class: "small" });

    function ulp(x) {
        if (x < 1e-30) return Math.pow(2, -149);
        return Math.pow(2, Math.floor(Math.log2(x)) - 23);
    }
    function wrap(t) {
        return Math.fround(t - Math.floor(t / LOOP) * LOOP);
    }

    lab.draw(function () {
        const start = hours.value * 3600 + 0.123;
        const baseTime = base.value ? 64 * Math.floor(start / 64) : 0;
        lab.clear(dots);
        let worst = 0;
        const span = 24 / 60;
        for (let n = 0; n < FRAMES; n++) {
            const clock = start + n / 60;
            const sinceBase = Math.fround(clock - baseTime);
            const phase = Math.fround(((baseTime % LOOP) + LOOP) % LOOP);
            const t = wrap(Math.fround(phase + sinceBase));
            const exact = ((clock % LOOP) + LOOP) % LOOP;
            let error = Math.abs(t - exact);
            error = Math.min(error, LOOP - error);
            worst = Math.max(worst, error);
            const progress = (n / 60 + (t - exact)) / span;
            lab.el(dots, "circle", { cx: X0 + 20 + progress * (X1 - X0 - 40), cy: 110, r: 7, class: error < 1e-4 ? "gpu" : "bad" });
        }
        const stepMs = 1000 * ulp(base.value ? 64 : start);
        stepOut.set(`${stepMs < 0.01 ? stepMs.toExponential(1) : stepMs.toFixed(3)} ms`);
        const ok = worst < 1e-4;
        errorOut.set(`${(1000 * worst).toFixed(3)} ms`, ok ? "ok" : "bad");
        lab.set(label, { text: base.value ? "the GPU adds at most 64 s to a phase under 2 s" : `the GPU adds the whole clock: ${Math.round(start)} s` });
        note.set(ok ? "The dots are evenly spaced: every frame moves the same distance." : "The dots bunch up: the float clock can only move in steps, so some frames move more than others.", ok ? "ok" : "bad");
    });
});
