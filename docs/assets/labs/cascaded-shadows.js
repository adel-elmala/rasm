// Labs of docs/guides/cascaded-shadows.html.

// A top view of a camera's frustum, cut into cascades by the split rule, with the sphere and the square box that
// each cascade fits around its slice. The camera has a 45° vertical field of view, a 16:9 image and a near plane at
// 0.1; every map has 2048 texels per side.
RasmLab.register("csm-splits", function (lab) {
    const svg = lab.svg(760, 430, "A top view of a camera's view frustum, cut into slices by the split rule. Each slice has a dashed sphere around it and a square shadow map box. The split depths are written under the frustum.");
    const count = lab.choice({ label: "Cascades", options: ["1", "2", "3", "4"], value: "4" });
    const lambda = lab.slider({ label: "Split mix (lambda)", min: 0, max: 1, step: 0.05, value: 0.7, format: function (v) { return v.toFixed(2); } });
    const distance = lab.slider({ label: "Shadow distance", min: 20, max: 200, step: 5, value: 60 });
    const nearOut = lab.readout("cascade 0: one texel");
    const farOut = lab.readout("last cascade: one texel");
    const gainOut = lab.readout("near texels vs one map");
    const note = lab.note();

    const NEAR = 0.1;
    const MAP = 2048;
    const TAN_Y = Math.tan(22.5 * Math.PI / 180);
    const TAN_X = TAN_Y * 16 / 9;
    const HALF_DIAGONAL = Math.sqrt(TAN_X * TAN_X + TAN_Y * TAN_Y);
    const ORIGIN = { x: 40, y: 190 };

    // The split rule: a mix of logarithmic and even, the last split at the distance.
    function splits(n, mix, far) {
        const out = [];
        for (let i = 1; i <= n; i++) {
            const t = i / n;
            out.push(mix * NEAR * Math.pow(far / NEAR, t) + (1 - mix) * (NEAR + (far - NEAR) * t));
        }
        out[n - 1] = far;
        return out;
    }

    // The smallest sphere centred on the view axis around the slice from zn to zf: the depth on the axis as far
    // from the near corners as from the far ones, kept inside the slice.
    function sphere(zn, zf) {
        const rn = zn * HALF_DIAGONAL;
        const rf = zf * HALF_DIAGONAL;
        const c = lab.clamp((zf * zf + rf * rf - zn * zn - rn * rn) / (2 * (zf - zn)), zn, zf);
        const r = Math.max(Math.hypot(c - zn, rn), Math.hypot(zf - c, rf));
        return { c: c, r: r };
    }

    function centimeters(r) {
        const cm = 2 * r / MAP * 100;
        return cm < 10 ? `${cm.toFixed(2)} cm` : `${cm.toFixed(1)} cm`;
    }

    // The far boxes are taller than the picture: they are cut at its top and bottom.
    const clip = lab.el(lab.el(svg, "defs"), "clipPath", { id: "csm-splits-clip" });
    lab.el(clip, "rect", { x: 0, y: 6, width: 760, height: 364 });
    const view = lab.el(svg, "g", { "clip-path": "url(#csm-splits-clip)" });
    const slices = lab.el(view, "g");
    const frustum = lab.el(view, "polygon", { class: "cpu o-50" });
    const boxes = lab.el(view, "g");
    const lines = lab.el(view, "g");
    const labels = lab.el(svg, "g");
    lab.el(svg, "circle", { cx: ORIGIN.x, cy: ORIGIN.y, r: 6, class: "solid" });
    lab.el(svg, "text", { x: 4, y: ORIGIN.y - 12, class: "small", text: "camera" });
    lab.el(svg, "line", { x1: 0, y1: 370, x2: 760, y2: 370, class: "grid" });
    lab.el(svg, "text", { x: 20, y: 424, class: "small t-faint", text: "split depths in world units · dashed circle: each cascade's sphere · square: its shadow map box" });

    lab.draw(function () {
        const n = parseInt(count.value, 10);
        const far = distance.value;
        const s = splits(n, lambda.value, far);
        const fits = [];
        for (let i = 0; i < n; i++) fits.push(sphere(i === 0 ? NEAR : s[i - 1], s[i]));
        const last = fits[n - 1];
        const scale = Math.min(690 / (last.c + last.r), 320 / last.r);
        function x(z) { return ORIGIN.x + z * scale; }
        function y(v) { return ORIGIN.y - v * scale; }

        lab.set(frustum, { points: `${x(NEAR)},${y(NEAR * TAN_X)} ${x(far)},${y(far * TAN_X)} ${x(far)},${y(-far * TAN_X)} ${x(NEAR)},${y(-NEAR * TAN_X)}` });

        lab.clear(slices);
        lab.clear(boxes);
        lab.clear(lines);
        lab.clear(labels);
        let labelX = 0;
        for (let i = 0; i < n; i++) {
            const zn = i === 0 ? NEAR : s[i - 1];
            const zf = s[i];
            if (i % 2 === 1) {
                lab.el(slices, "polygon", { class: "zone", points: `${x(zn)},${y(zn * TAN_X)} ${x(zf)},${y(zf * TAN_X)} ${x(zf)},${y(-zf * TAN_X)} ${x(zn)},${y(-zn * TAN_X)}` });
            }
            const f = fits[i];
            lab.el(boxes, "circle", { cx: x(f.c), cy: ORIGIN.y, r: f.r * scale, class: "ghost" });
            lab.el(boxes, "rect", { x: x(f.c - f.r), y: y(f.r), width: 2 * f.r * scale, height: 2 * f.r * scale, class: "hot" });
            lab.el(lines, "line", { x1: x(zf), y1: y(zf * TAN_X), x2: x(zf), y2: 370, class: "dashed" });
            // Labels under the picture, pushed apart when splits are close together.
            labelX = Math.max(x(zf), labelX + 58);
            lab.el(labels, "text", { x: labelX, y: 392, class: "mid small mono", text: zf < 10 ? zf.toFixed(2) : zf.toFixed(1) });
            lab.el(labels, "text", { x: labelX, y: 408, class: "mid small t-hot", text: `cascade ${i}` });
        }

        const one = sphere(NEAR, far);
        const gain = one.r / fits[0].r;
        nearOut.set(centimeters(fits[0].r), n > 1 ? "ok" : "bad");
        farOut.set(centimeters(last.r));
        gainOut.set(`× ${gain.toFixed(1)}`, gain >= 4 ? "ok" : "bad");
        if (n === 1) note.set(`One map: each texel is ${centimeters(one.r)} wide, next to the camera and at ${far} units alike. Near shadows are blurry. Add cascades.`, "bad");
        else if (s[0] < 2) note.set(`Almost logarithmic: cascade 0 ends ${s[0].toFixed(2)} units from the camera, where there is little to see. Most near shadows fall in cascade 1. Lower lambda.`, "bad");
        else if (gain < 4) note.set(`Almost even: cascade 0 already reaches ${s[0].toFixed(1)} units, so its texels are only ${gain.toFixed(1)} times smaller than one map's. Raise lambda.`, "bad");
        else note.set(`Cascade 0 ends at ${s[0].toFixed(1)} units, and its texels are ${gain.toFixed(1)} times smaller than one map's. The last cascade still reaches ${far} units.`, "ok");
    });
});
