// Labs of docs/guides/pbr.html.

// A sphere shaded per pixel with the page's BRDF: Lambert + GGX D, height-correlated Smith V, Schlick F.
// One directional light, the eye far away on the +z axis. Linear result -> c / (c + 1) -> sRGB for the screen.
RasmLab.register("pbr-sphere", function (lab) {
    const SIZE = 280;
    const RADIUS = 124;
    const BASE = [1.0, 0.71, 0.29];     // the base color: the page's values for gold
    const LIGHT = 3;                    // E, the light that arrives
    const ctx = lab.canvas(SIZE, SIZE, "A sphere lit by one light, shaded per pixel with the diffuse and specular formulas of this page.");
    const rough = lab.slider({ label: "Roughness r", min: 0.05, max: 1, step: 0.01, value: 0.5, format: function (v) { return v.toFixed(2); } });
    const metal = lab.slider({ label: "Metallic", min: 0, max: 1, step: 0.05, value: 0, format: function (v) { return v.toFixed(2); } });
    const angle = lab.slider({ label: "Light angle", min: 0, max: 150, step: 5, value: 60, format: function (v) { return `${v}°`; } });
    const f0Out = lab.readout("F0 (R, G, B)");
    const diffuseOut = lab.readout("diffuse color (R, G, B)");
    const peakOut = lab.readout("peak of D: 1 / (π α²)");
    const note = lab.note();

    const image = ctx.createImageData(SIZE, SIZE);
    function encode(c) {
        return 255 * (c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055);
    }
    function triple(c) {
        return c.map(function (x) { return x.toFixed(2); }).join(", ");
    }

    lab.draw(function () {
        const r = rough.value;
        const m = metal.value;
        const alpha = r * r;
        const a2 = alpha * alpha;
        const t = angle.value * Math.PI / 180;
        const l = [-0.8 * Math.sin(t), 0.6 * Math.sin(t), Math.cos(t)];     // toward the light
        const f0 = BASE.map(function (c) { return 0.04 + (c - 0.04) * m; });
        const diffuse = BASE.map(function (c) { return c * (1 - m); });

        // h = normalize(l + v), with v = (0, 0, 1): the same for every pixel
        const hLen = Math.sqrt(l[0] * l[0] + l[1] * l[1] + (l[2] + 1) * (l[2] + 1));
        const h = [l[0] / hLen, l[1] / hLen, (l[2] + 1) / hLen];
        const lh = lab.clamp(l[0] * h[0] + l[1] * h[1] + l[2] * h[2], 0, 1);
        const rise = Math.pow(1 - lh, 5);

        const data = image.data;
        for (let py = 0; py < SIZE; py++) {
            for (let px = 0; px < SIZE; px++) {
                const at = 4 * (py * SIZE + px);
                let x = (px + 0.5 - SIZE / 2) / RADIUS;
                let y = (SIZE / 2 - py - 0.5) / RADIUS;
                const d = Math.sqrt(x * x + y * y);
                const cover = lab.clamp((1 - d) * RADIUS + 0.5, 0, 1);      // soft edge
                if (cover <= 0) {
                    data[at + 3] = 0;
                    continue;
                }
                let z = 0;
                if (d < 1) z = Math.sqrt(1 - d * d);
                else { x /= d; y /= d; }
                const nl = x * l[0] + y * l[1] + z * l[2];
                let rgb = [0, 0, 0];
                if (nl > 0) {
                    const nv = Math.max(z, 1e-5);
                    const nh = lab.clamp(x * h[0] + y * h[1] + z * h[2], 0, 1);
                    const spread = nh * nh * (a2 - 1) + 1;
                    const D = a2 / (Math.PI * spread * spread);
                    const V = 0.5 / (nl * Math.sqrt(nv * nv * (1 - a2) + a2) + nv * Math.sqrt(nl * nl * (1 - a2) + a2));
                    rgb = f0.map(function (f, k) {
                        const F = f + (1 - f) * rise;
                        return (D * V * F + (1 - F) * diffuse[k] / Math.PI) * LIGHT * nl;
                    });
                }
                for (let k = 0; k < 3; k++) data[at + k] = encode(rgb[k] / (rgb[k] + 1));
                data[at + 3] = 255 * cover;
            }
        }
        ctx.putImageData(image, 0, 0);
        ctx.strokeStyle = lab.css("--fg-faint");
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.arc(SIZE / 2, SIZE / 2, RADIUS, 0, 2 * Math.PI);
        ctx.stroke();

        f0Out.set(triple(f0));
        diffuseOut.set(triple(diffuse));
        peakOut.set((1 / (Math.PI * a2)).toPrecision(3));
        const spot = r < 0.3 ? "small and bright" : (r > 0.65 ? "wide and dim" : "of medium size");
        if (m === 0) note.set(`A non-metal: the body shows the base color as diffuse light, and the highlight is white (F0 = 0.04). The highlight is ${spot}.`);
        else if (m === 1) note.set(`A metal: no diffuse light at all. The base color is now F0, so the highlight has the metal's color. The highlight is ${spot}.`);
        else note.set(`Part metal, part non-metal: ${Math.round(100 * (1 - m))} % of the base color is diffuse, and F0 is ${Math.round(100 * m)} % of the way from 0.04 to the base color.`);
    });
});

// One facet from the side: the eye direction v at an angle θ from the facet normal h, the light mirrored on
// the other side. Schlick's formula gives the share reflected; the rest goes into the surface.
RasmLab.register("fresnel-angle", function (lab) {
    const svg = lab.svg(760, 302, "Left: a facet with its normal h, light coming in and the eye at the mirror angle. A bar shows the share reflected and the share that goes in. Right: the Schlick curve against the angle, with a point at the current angle.");
    const f0 = lab.slider({ label: "F0", min: 0.02, max: 1, step: 0.01, value: 0.04, format: function (v) { return v.toFixed(2); } });
    const cosOut = lab.readout("cos θ");
    const powOut = lab.readout("(1 − cos θ)⁵");
    const fOut = lab.readout("F: the share reflected");
    const note = lab.note();

    const P = { x: 190, y: 196 };
    const ARC = 140;
    const PLOT = { x0: 450, x1: 730, y0: 220, y1: 50 };
    function plotX(deg) { return PLOT.x0 + (PLOT.x1 - PLOT.x0) * deg / 90; }
    function plotY(f) { return PLOT.y0 + (PLOT.y1 - PLOT.y0) * f; }
    function schlick(cos) { return f0.value + (1 - f0.value) * Math.pow(1 - cos, 5); }

    // the facet
    lab.el(svg, "path", { d: `M${P.x},${P.y - ARC} A${ARC},${ARC} 0 0 1 ${P.x + ARC},${P.y}`, class: "dashed" });
    lab.el(svg, "line", { x1: P.x, y1: P.y, x2: P.x, y2: P.y - ARC - 8, class: "dashed" });
    lab.el(svg, "text", { x: P.x, y: P.y - ARC - 16, class: "mid title", text: "h" });
    const lightRay = lab.el(svg, "line", { class: "arrow light", "marker-end": "url(#ah-light)" });
    const lightText = lab.el(svg, "text", { class: "end small t-light", text: "light" });
    const inRay = lab.el(svg, "line", { class: "arrow gpu dashed", "marker-end": "url(#ah-gpu)" });
    const eyeRay = lab.el(svg, "line", { class: "arrow cpu", "marker-end": "url(#ah-cpu)" });
    const eyeText = lab.el(svg, "text", { class: "small t-cpu", text: "eye" });
    const angleArc = lab.el(svg, "path", { class: "arrow" });
    const angleText = lab.el(svg, "text", { class: "mid small halo" });
    lab.el(svg, "line", { x1: 40, y1: P.y, x2: 340, y2: P.y, class: "surface" });
    const barOut = lab.el(svg, "rect", { x: 40, y: 258, height: 16, class: "accent" });
    const barIn = lab.el(svg, "rect", { y: 258, height: 16, class: "gpu" });
    const barOutText = lab.el(svg, "text", { x: 40, y: 292, class: "small t-hot" });
    const barInText = lab.el(svg, "text", { x: 340, y: 292, class: "end small t-gpu" });

    // the curve
    lab.el(svg, "text", { x: 440, y: 22, class: "title", text: "F against the angle, for this F0" });
    lab.el(svg, "line", { x1: PLOT.x0, y1: PLOT.y0, x2: PLOT.x1 + 12, y2: PLOT.y0, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "line", { x1: PLOT.x0, y1: PLOT.y0, x2: PLOT.x0, y2: PLOT.y1 - 14, class: "arrow", "marker-end": "url(#ah)" });
    [0, 30, 60, 90].forEach(function (deg) {
        lab.el(svg, "text", { x: plotX(deg), y: 240, class: "mid mono small", text: `${deg}°` });
    });
    lab.el(svg, "text", { x: 444, y: 54, class: "end mono small", text: "1" });
    lab.el(svg, "text", { x: 444, y: 224, class: "end mono small", text: "0" });
    lab.el(svg, "line", { x1: PLOT.x0 - 4, y1: PLOT.y1, x2: PLOT.x1, y2: PLOT.y1, class: "ghost" });
    lab.el(svg, "text", { x: 590, y: 262, class: "mid small", text: "angle θ between v and h" });
    const guide = lab.el(svg, "path", { class: "dashed" });
    const curve = lab.el(svg, "polyline", { class: "hot" });
    const dot = lab.el(svg, "circle", { r: 5, class: "accent" });
    const dotText = lab.el(svg, "text", { class: "end mono small halo" });

    const eye = lab.handle(svg, { x: P.x + ARC * Math.sin(Math.PI / 3), y: P.y - ARC * Math.cos(Math.PI / 3), label: "Move the eye", minX: P.x, maxX: P.x + ARC, minY: P.y - ARC, maxY: P.y });

    lab.draw(function () {
        // keep the eye on the arc: only its angle from h counts
        const theta = lab.clamp(Math.atan2(eye.x - P.x, P.y - eye.y), 0, Math.PI / 2);
        const sin = Math.sin(theta);
        const cos = Math.max(0, Math.cos(theta));
        eye.x = P.x + ARC * sin;
        eye.y = P.y - ARC * cos;
        lab.set(eye.el, { cx: eye.x, cy: eye.y });
        const deg = theta * 180 / Math.PI;
        const F = schlick(cos);

        const from = [P.x - ARC * sin, P.y - ARC * cos];
        lab.set(lightRay, { x1: from[0], y1: from[1], x2: P.x - 5 * sin, y2: P.y - 5 * cos });
        lab.set(lightText, { x: from[0] - 8, y: from[1] - 6 });
        lab.set(eyeRay, { x1: P.x, y1: P.y, x2: P.x + (ARC - 14) * sin, y2: P.y - (ARC - 14) * cos });
        lab.set(eyeText, { x: eye.x + 14, y: eye.y - 8 });
        // the part that goes in bends toward the normal (drawn for an index of refraction of 1.5)
        const sinIn = sin / 1.5;
        lab.set(inRay, { x1: P.x, y1: P.y + 3, x2: P.x + 46 * sinIn, y2: P.y + 3 + 46 * Math.sqrt(1 - sinIn * sinIn) });
        lab.set(angleArc, { d: `M${P.x},${P.y - 40} A40,40 0 0 1 ${(P.x + 40 * sin).toFixed(1)},${(P.y - 40 * cos).toFixed(1)}` });
        lab.set(angleText, { x: P.x + 62 * Math.sin(theta / 2), y: P.y - 62 * Math.cos(theta / 2) + 4, text: `θ = ${deg.toFixed(0)}°` });

        lab.set(barOut, { width: (300 * F).toFixed(1) });
        lab.set(barIn, { x: (40 + 300 * F).toFixed(1), width: (300 * (1 - F)).toFixed(1) });
        lab.set(barOutText, { text: `reflected: ${(100 * F).toFixed(1)} %` });
        lab.set(barInText, { text: `goes in: ${(100 * (1 - F)).toFixed(1)} %` });

        let points = "";
        for (let a = 0; a <= 90; a += 2) points += `${plotX(a).toFixed(1)},${plotY(schlick(Math.cos(a * Math.PI / 180))).toFixed(1)} `;
        lab.set(curve, { points: points });
        const dx = plotX(deg);
        const dy = plotY(F);
        lab.set(dot, { cx: dx, cy: dy });
        lab.set(guide, { d: `M${dx.toFixed(1)},${PLOT.y0} V${dy.toFixed(1)} H${PLOT.x0}` });
        lab.set(dotText, { x: dx - 9, y: dy - 9, text: `F = ${F.toFixed(3)}` });

        cosOut.set(cos.toFixed(3));
        const rise = Math.pow(1 - cos, 5);
        powOut.set(rise.toFixed(5));
        fOut.set(F.toFixed(3));
        if (deg >= 89.5) note.set("At 90° the facet reflects all the light, whatever F0 is.");
        else if (rise < 0.02) note.set(`The flat part of the curve: at ${deg.toFixed(0)}°, F is still almost F0 (${f0.value.toFixed(2)}).`);
        else note.set(`The curve rises: at ${deg.toFixed(0)}°, F is ${(100 * rise).toFixed(0)} % of the way from F0 (${f0.value.toFixed(2)}) to 1.`);
    });
});
