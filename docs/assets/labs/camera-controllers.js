// Labs of docs/guides/camera-controllers.html.

// A percentage with enough digits to tell small values apart.
function cameraControllersPercent(v) {
    const p = 100 * v;
    return `${p < 0.1 ? p.toFixed(2) : p.toFixed(1)}%`;
}

// Distance left to the target over half a second, stepped at a chosen frame rate with the two ways to pick t.
RasmLab.register("easing-rate", function (lab) {
    const svg = lab.svg(760, 290, "A plot of the distance left to the target over half a second. Steps with a factor computed from the frame time lie on one curve at every frame rate. Steps with a fixed factor of 0.1 leave the curve when the frame rate changes.");
    const fps = lab.slider({ label: "Frame rate", min: 10, max: 240, step: 2, value: 30, format: function (v) { return `${v} fps`; } });
    const speed = lab.slider({ label: "Easing speed k", min: 1, max: 20, step: 0.5, value: 6, format: function (v) { return v.toFixed(1); } });
    const tOut = lab.readout("t per frame, from the frame time");
    const timeOut = lab.readout("left after 0.5 s, t from the frame time");
    const fixedOut = lab.readout("left after 0.5 s, fixed t = 0.1");
    const note = lab.note();

    const X0 = 70;
    const X1 = 730;
    const Y0 = 236;
    const Y1 = 36;
    const SPAN = 0.5;
    function px(time) { return X0 + (X1 - X0) * time / SPAN; }
    function py(left) { return Y0 + (Y1 - Y0) * left; }

    lab.el(svg, "path", { d: `M${X0},${py(0.5)}H${X1}M${px(0.25)},${Y1}V${Y0}`, class: "grid" });
    lab.el(svg, "line", { x1: X0, y1: Y0, x2: X1 + 14, y2: Y0, class: "arrow" });
    lab.el(svg, "line", { x1: X0, y1: Y0, x2: X0, y2: Y1 - 14, class: "arrow" });
    lab.el(svg, "text", { x: X0 - 8, y: py(1) + 4, class: "end small", text: "100%" });
    lab.el(svg, "text", { x: X0 - 8, y: py(0.5) + 4, class: "end small", text: "50%" });
    lab.el(svg, "text", { x: X0 - 8, y: py(0) + 4, class: "end small", text: "0" });
    lab.el(svg, "text", { x: px(0), y: Y0 + 18, class: "mid small", text: "0" });
    lab.el(svg, "text", { x: px(0.25), y: Y0 + 18, class: "mid small", text: "0.25 s" });
    lab.el(svg, "text", { x: px(0.5), y: Y0 + 18, class: "mid small", text: "0.5 s" });
    lab.el(svg, "text", { x: (X0 + X1) / 2, y: Y0 + 40, class: "mid small", text: "time since the target moved" });
    lab.el(svg, "text", { x: X0 + 10, y: Y1 - 14, class: "small", text: "distance left" });
    const curve = lab.el(svg, "polyline", { class: "arrow dashed" });
    const fixedLine = lab.el(svg, "polyline", { class: "arrow bad" });
    const fixedDots = lab.el(svg, "g");
    const timeDots = lab.el(svg, "g");

    lab.draw(function () {
        const k = speed.value;
        const dt = 1 / fps.value;
        const t = 1 - Math.exp(-k * dt);
        const FIXED = 0.1;

        let points = "";
        for (let i = 0; i <= 80; i++) {
            const time = SPAN * i / 80;
            points += `${px(time).toFixed(1)},${py(Math.exp(-k * time)).toFixed(1)} `;
        }
        lab.set(curve, { points: points });

        // step both values frame by frame, as a controller does
        lab.clear(fixedDots);
        lab.clear(timeDots);
        const r = fps.value > 90 ? 2 : 3.5;
        let leftTime = 1;
        let leftFixed = 1;
        let fixedPoints = `${px(0)},${py(1)} `;
        const frames = Math.floor(SPAN * fps.value + 1e-9);
        for (let i = 1; i <= frames; i++) {
            leftTime += (0 - leftTime) * t;
            leftFixed += (0 - leftFixed) * FIXED;
            const x = px(i * dt).toFixed(1);
            fixedPoints += `${x},${py(leftFixed).toFixed(1)} `;
            lab.el(fixedDots, "circle", { cx: x, cy: py(leftFixed).toFixed(1), r: r, class: "bad" });
            lab.el(timeDots, "circle", { cx: x, cy: py(leftTime).toFixed(1), r: r, class: "accent" });
        }
        lab.set(fixedLine, { points: fixedPoints });

        // the values at exactly 0.5 s
        const endTime = Math.exp(-k * SPAN);
        const endFixed = Math.pow(1 - FIXED, SPAN * fps.value);
        const ratio = endFixed / endTime;
        tOut.set(t.toFixed(3));
        timeOut.set(cameraControllersPercent(endTime), "ok");
        fixedOut.set(cameraControllersPercent(endFixed), ratio > 1.25 || ratio < 0.8 ? "bad" : "");
        if (ratio > 1.25) note.set(`At ${fps.value} fps the fixed factor runs too few times: the camera is ${ratio.toFixed(ratio < 10 ? 1 : 0)} times farther from its target than it should be. The steps from the frame time stay on the curve.`, "bad");
        else if (ratio < 0.8) note.set(`At ${fps.value} fps the fixed factor runs too many times: the camera arrives too fast, with ${(1 / ratio).toFixed(1 / ratio < 10 ? 1 : 0)} times less distance left than it should have. The steps from the frame time stay on the curve.`, "bad");
        else note.set(`Here the two agree, but only by chance: at ${fps.value} fps and k = ${k.toFixed(1)} the right factor is ${t.toFixed(3)}, close to 0.1. Change the frame rate and they part again.`, "ok");
    });
});

// An orbit turn, blended two ways: a straight mix of the two positions, and a turn along the arc.
RasmLab.register("orbit-mix", function (lab) {
    const svg = lab.svg(760, 290, "Left: an orbit circle around a center, with the current and the target camera position. A straight mix of the positions cuts through the circle; a blend of the rotation follows the circle. Right: the distance to the center during the blend, for both.");
    const turn = lab.slider({ label: "Turn", min: 10, max: 180, step: 5, value: 120, format: function (v) { return `${v}°`; } });
    const blend = lab.slider({ label: "Blend t", min: 0, max: 1, step: 0.01, value: 0.5, format: function (v) { return v.toFixed(2); } });
    const mixOut = lab.readout("distance to the center, straight mix");
    const arcOut = lab.readout("distance to the center, along the arc");
    const worstOut = lab.readout("closest point of the straight mix");
    const note = lab.note();

    const CX = 190;
    const CY = 170;
    const R = 110;
    const X0 = 450;
    const X1 = 730;
    const Y0 = 240;
    const Y1 = 60;
    function at(angle, radius) { return [CX + radius * Math.cos(angle), CY - radius * Math.sin(angle)]; }
    function gx(t) { return X0 + (X1 - X0) * t; }
    function gy(d) { return Y0 + (Y1 - Y0) * d; }

    lab.el(svg, "circle", { cx: CX, cy: CY, r: R, class: "ghost" });
    const chord = lab.el(svg, "line", { class: "arrow bad" });
    const arc = lab.el(svg, "path", { class: "arrow ok thick" });
    const spokeMix = lab.el(svg, "line", { class: "arrow bad dashed" });
    const spokeArc = lab.el(svg, "line", { class: "arrow ok dashed" });
    lab.el(svg, "circle", { cx: CX, cy: CY, r: 4, class: "solid" });
    lab.el(svg, "text", { x: CX, y: CY + 20, class: "mid small", text: "center" });
    const from = lab.el(svg, "circle", { r: 7, class: "cpu" });
    const to = lab.el(svg, "circle", { r: 7, class: "cpu" });
    const fromText = lab.el(svg, "text", { class: "mid small halo", text: "current" });
    const toText = lab.el(svg, "text", { class: "mid small halo", text: "target" });
    const mixDot = lab.el(svg, "circle", { r: 6, class: "bad" });
    const arcDot = lab.el(svg, "circle", { r: 6, class: "ok" });

    lab.el(svg, "path", { d: `M${X0},${gy(0.5)}H${X1}`, class: "grid" });
    lab.el(svg, "line", { x1: X0, y1: Y0, x2: X1 + 12, y2: Y0, class: "arrow" });
    lab.el(svg, "line", { x1: X0, y1: Y0, x2: X0, y2: Y1 - 16, class: "arrow" });
    lab.el(svg, "text", { x: X0 - 8, y: gy(1) + 4, class: "end small", text: "100%" });
    lab.el(svg, "text", { x: X0 - 8, y: gy(0.5) + 4, class: "end small", text: "50%" });
    lab.el(svg, "text", { x: X0 - 8, y: gy(0) + 4, class: "end small", text: "0" });
    lab.el(svg, "text", { x: gx(0), y: Y0 + 18, class: "mid small", text: "t = 0" });
    lab.el(svg, "text", { x: gx(1), y: Y0 + 18, class: "mid small", text: "t = 1" });
    lab.el(svg, "text", { x: X0 + 8, y: Y1 - 22, class: "small", text: "distance to the center, during the blend" });
    lab.el(svg, "line", { x1: gx(0), y1: gy(1), x2: gx(1), y2: gy(1), class: "arrow ok thick" });
    const mixCurve = lab.el(svg, "polyline", { class: "arrow bad" });
    const marker = lab.el(svg, "line", { class: "dashed" });
    const mixMark = lab.el(svg, "circle", { r: 5, class: "bad" });
    const arcMark = lab.el(svg, "circle", { r: 5, class: "ok" });

    lab.draw(function () {
        const angle = turn.value * Math.PI / 180;
        const t = blend.value;
        const a0 = Math.PI / 2 + angle / 2;
        const a1 = Math.PI / 2 - angle / 2;
        const p0 = at(a0, R);
        const p1 = at(a1, R);
        // straight mix of the two positions, and its distance to the center (the orbit distance is 1)
        function mixDistance(s) {
            const x = (1 - s) * Math.cos(a0) + s * Math.cos(a1);
            const y = (1 - s) * Math.sin(a0) + s * Math.sin(a1);
            return Math.hypot(x, y);
        }
        const mixed = [(1 - t) * p0[0] + t * p1[0], (1 - t) * p0[1] + t * p1[1]];
        const onArc = at(a0 + (a1 - a0) * t, R);

        lab.set(from, { cx: p0[0], cy: p0[1] });
        lab.set(to, { cx: p1[0], cy: p1[1] });
        const l0 = at(a0, R + 20);
        const l1 = at(a1, R + 20);
        lab.set(fromText, { x: (l0[0] - 14).toFixed(1), y: (l0[1] + 4).toFixed(1) });
        lab.set(toText, { x: (l1[0] + 12).toFixed(1), y: (l1[1] + 4).toFixed(1) });
        lab.set(chord, { x1: p0[0], y1: p0[1], x2: p1[0], y2: p1[1] });
        lab.set(arc, { d: `M${p0[0].toFixed(1)},${p0[1].toFixed(1)} A${R},${R} 0 0,1 ${onArc[0].toFixed(1)},${onArc[1].toFixed(1)}` });
        lab.set(spokeMix, { x1: CX, y1: CY, x2: mixed[0], y2: mixed[1] });
        lab.set(spokeArc, { x1: CX, y1: CY, x2: onArc[0], y2: onArc[1] });
        lab.set(mixDot, { cx: mixed[0], cy: mixed[1] });
        lab.set(arcDot, { cx: onArc[0], cy: onArc[1] });

        let points = "";
        for (let i = 0; i <= 50; i++) points += `${gx(i / 50).toFixed(1)},${gy(mixDistance(i / 50)).toFixed(1)} `;
        lab.set(mixCurve, { points: points });
        const d = mixDistance(t);
        lab.set(marker, { x1: gx(t), y1: Y0, x2: gx(t), y2: gy(1) });
        lab.set(mixMark, { cx: gx(t), cy: gy(d) });
        lab.set(arcMark, { cx: gx(t), cy: gy(1) });

        const worst = Math.cos(angle / 2);
        mixOut.set(`${(100 * d).toFixed(0)}%`, d < 0.98 ? "bad" : "");
        arcOut.set("100%", "ok");
        worstOut.set(`${(100 * worst).toFixed(0)}%`, worst < 0.98 ? "bad" : "");
        if (worst < 0.02) note.set("A half turn: the straight mix passes through the center itself. The arc keeps the distance.", "bad");
        else if (d < 0.98) note.set(`The straight mix is at ${(100 * d).toFixed(0)}% of the orbit distance here: the view zooms in, then out again. The arc stays at 100%.`, "bad");
        else if (worst < 0.98) note.set(`At this t the two are close, but halfway the straight mix drops to ${(100 * worst).toFixed(0)}% of the orbit distance.`, "");
        else note.set("For a small turn the two paths are almost the same. The error grows with the angle.", "ok");
    });
});
