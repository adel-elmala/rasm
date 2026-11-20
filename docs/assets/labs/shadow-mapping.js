// Labs of docs/guides/shadow-mapping.html.

// A side view of a ball on the ground, a sun and a 1D shadow map. The ground is tested point by point against
// the map, like a fragment shader does, and colored by the result against the exact answer.
RasmLab.register("shadow-bias", function (lab) {
    const svg = lab.svg(760, 330, "A side view of a ball on the ground under a sun. The ground is colored by the shadow test: lit, in shadow, wrongly in shadow (acne) or wrongly lit (peter-panning).");
    const angle = lab.slider({ label: "Sun angle", min: 10, max: 70, value: 40, format: function (v) { return `${v}°`; } });
    const texels = lab.slider({ label: "Map texels", min: 8, max: 96, step: 4, value: 24 });
    const bias = lab.slider({ label: "Depth bias", min: 0, max: 0.6, step: 0.01, value: 0, format: function (v) { return v.toFixed(2); } });
    const acneOut = lab.readout("lit ground wrongly dark (acne)");
    const gapOut = lab.readout("ground wrongly lit at the ball (gap)");
    const needOut = lab.readout("bias that just removes acne");
    const note = lab.note();

    const SCALE = 32;
    const HALF = 11;
    const BALL = { x: -0.5, y: 1.5, r: 1.5 };
    function sx(x) { return 380 + x * SCALE; }
    function sy(y) { return 262 - y * SCALE; }

    const rays = lab.el(svg, "g");
    const volume = lab.el(svg, "polygon", { class: "shade o-25" });
    const steps = lab.el(svg, "path", { class: "arrow" });
    const biased = lab.el(svg, "path", { class: "arrow hot dashed" });
    lab.el(svg, "circle", { cx: sx(BALL.x), cy: sy(BALL.y), r: BALL.r * SCALE, class: "solid" });
    lab.el(svg, "line", { x1: sx(-HALF), y1: sy(0), x2: sx(HALF), y2: sy(0), class: "surface" });
    const band = lab.el(svg, "g");
    const sun = lab.el(svg, "circle", { r: 13, class: "light" });
    lab.el(svg, "text", { x: 20, y: 322, class: "small", text: "the band under the ground: the result of the shadow test at each point" });

    lab.draw(function () {
        const a = angle.value * Math.PI / 180;
        const dir = [Math.sin(a), -Math.cos(a)];     // the way light travels
        const side = [Math.cos(a), Math.sin(a)];     // the shadow map's axis
        function u(x, y) { return x * side[0] + y * side[1]; }
        function depth(x, y) { return x * dir[0] + y * dir[1]; }
        function point(uu, dd) { return [sx(side[0] * uu + dir[0] * dd), sy(side[1] * uu + dir[1] * dd)]; }
        function ballDepth(uu) {
            const h = uu - u(BALL.x, BALL.y);
            return Math.abs(h) < BALL.r ? depth(BALL.x, BALL.y) - Math.sqrt(BALL.r * BALL.r - h * h) : Infinity;
        }
        function groundDepth(uu) { return depth(uu / side[0], 0); }

        const n = texels.value;
        const uMin = u(-HALF, 0);
        const du = (u(HALF, 0) - uMin) / n;
        const stored = [];
        let stepPath = "";
        let biasPath = "";
        for (let i = 0; i < n; i++) {
            stored.push(Math.min(groundDepth(uMin + (i + 0.5) * du), ballDepth(uMin + (i + 0.5) * du)));
            const p0 = point(uMin + i * du, stored[i]);
            const p1 = point(uMin + (i + 1) * du, stored[i]);
            stepPath += `M${p0[0].toFixed(1)},${p0[1].toFixed(1)}L${p1[0].toFixed(1)},${p1[1].toFixed(1)}`;
            const b0 = point(uMin + i * du, stored[i] + bias.value);
            const b1 = point(uMin + (i + 1) * du, stored[i] + bias.value);
            biasPath += `M${b0[0].toFixed(1)},${b0[1].toFixed(1)}L${b1[0].toFixed(1)},${b1[1].toFixed(1)}`;
        }
        lab.set(steps, { d: stepPath });
        lab.set(biased, { d: bias.value > 0 ? biasPath : "M0,0" });

        const uLo = u(BALL.x, BALL.y) - BALL.r;
        const uHi = u(BALL.x, BALL.y) + BALL.r;

        // the ground, point by point: what the test says against the exact answer
        const SAMPLES = 440;
        const CLASSES = { lit: "lit", shadow: "shade", acne: "bad", leak: "accent" };
        lab.clear(band);
        let run = null;
        let acne = 0;
        let litTotal = 0;
        let leak = 0;           // wrongly lit points right after the ball's foot
        let gapOpen = true;
        for (let k = 0; k < SAMPLES; k++) {
            const x = -HALF + (k + 0.5) * (2 * HALF) / SAMPLES;
            const d = depth(x, 0);
            const i = lab.clamp(Math.floor((u(x, 0) - uMin) / du), 0, n - 1);
            const dark = d > stored[i] + bias.value + 1e-9;
            const reallyDark = ballDepth(u(x, 0)) < d - 1e-6;
            const kind = dark ? (reallyDark ? "shadow" : "acne") : (reallyDark ? "leak" : "lit");
            if (!reallyDark) litTotal += 1;
            if (kind === "acne") acne += 1;
            if (x > uLo / side[0] && gapOpen) {
                if (kind === "leak") leak += 1;
                else gapOpen = false;
            }
            if (!run || run.kind !== kind) {
                run = { kind: kind, el: lab.el(band, "rect", { x: sx(x) - 0.9, y: sy(0) + 5, width: 0, height: 12, class: CLASSES[kind] }), from: sx(x) - 0.9 };
            }
            lab.set(run.el, { width: (sx(x) + 0.9 - run.from).toFixed(1) });
        }

        // the sun, its rays and the exact shadow
        lab.clear(rays);
        for (let k = -3; k <= 3; k++) {
            const from = point(k * 2.6 * Math.cos(a), -7.4);
            const to = point(k * 2.6 * Math.cos(a), -5.9);
            lab.el(rays, "line", { x1: from[0], y1: from[1], x2: to[0], y2: to[1], class: "arrow light", "marker-end": "url(#ah-light)" });
        }
        const sunAt = point(-8.4 * Math.cos(a), -7.6);
        lab.set(sun, { cx: lab.clamp(sunAt[0], 16, 744), cy: lab.clamp(sunAt[1], 16, 200) });
        lab.set(volume, {
            points: `${sx(BALL.x - BALL.r * side[0])},${sy(BALL.y - BALL.r * side[1])} ${sx(uLo / side[0])},${sy(0)} ` +
                    `${sx(uHi / side[0])},${sy(0)} ${sx(BALL.x + BALL.r * side[0])},${sy(BALL.y + BALL.r * side[1])}`,
        });

        const need = du * Math.tan(a) / 2;
        const acneShare = 100 * acne / litTotal;
        const gap = leak * (2 * HALF) / SAMPLES;
        acneOut.set(`${acneShare.toFixed(0)} %`, acneShare > 0.5 ? "bad" : "ok");
        gapOut.set(`${gap.toFixed(2)} units`, gap > 0.15 ? "bad" : "ok");
        needOut.set(need.toFixed(2));
        if (acneShare > 0.5) note.set("Acne: the tilted ground changes depth inside one texel, so part of each texel is behind its stored step. Raise the bias.", "bad");
        else if (gap > 0.15) note.set("Peter-panning: the bias is larger than needed, so the shadow starts away from the ball. Lower it, or use more texels.", "bad");
        else note.set("Clean: the bias is just past half a texel's depth change, and the shadow still starts at the ball.", "ok");
    });
});
