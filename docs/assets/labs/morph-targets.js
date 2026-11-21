// Labs of docs/guides/morph-targets.html.

// A round shape of 32 vertices and three morph targets, each stored as one delta per vertex. The morphed vertex is
// base + Σ w_t · Δ_t. The bound is the base shape's sphere grown by Σ |w_t| · r_t, where r_t is the longest delta of
// target t: it holds every vertex for any weights, without looking at the vertices.
(function () {
    const CX = 380;
    const CY = 200;
    const SCALE = 75;
    const COUNT = 32;

    // The base shape: a circle of radius 1. Its sphere: center (0, 0), radius 1.
    function baseVertices() {
        const out = [];
        for (let i = 0; i < COUNT; i++) {
            const a = (i / COUNT) * 2 * Math.PI;
            out.push({ x: Math.cos(a), y: Math.sin(a) });
        }
        return out;
    }

    // Each target's delta of vertex p.
    const TARGETS = [
        { name: "tall", delta: function (p) { return { x: 0, y: 0.6 * p.y }; } },
        { name: "wide", delta: function (p) { return { x: 0.5 * p.x, y: -0.25 * p.y }; } },
        {
            name: "bump", delta: function (p) {
                const k = Math.pow(Math.max(0, p.x), 6);
                return { x: 0.8 * k * p.x, y: 0.8 * k * p.y };
            }
        },
    ];

    function toSvg(p) {
        return { x: CX + SCALE * p.x, y: CY - SCALE * p.y };
    }

    function pathOf(points) {
        return points.map(function (p, i) {
            const s = toSvg(p);
            return `${i === 0 ? "M" : "L"}${s.x.toFixed(1)},${s.y.toFixed(1)}`;
        }).join("") + "Z";
    }

    RasmLab.register("morph-bounds", function (lab) {
        const svg = lab.svg(760, 400, "A round base shape of 32 vertices, drawn faint. The colored shape is the base plus each target's deltas times its weight. A dashed circle is the base shape's bound; the solid circle is that bound grown by the sum of each weight times its target's longest delta, and it holds every vertex.");
        const sliders = TARGETS.map(function (target, t) {
            return lab.slider({ label: `Weight "${target.name}"`, min: -1, max: 1.5, step: 0.05, value: t === 0 ? 0.6 : 0, format: function (v) { return v.toFixed(2); } });
        });
        const boundOut = lab.readout("bound radius: 1 + Σ |w| · r");
        const farOut = lab.readout("farthest vertex");
        const readOut = lab.readout("targets read per vertex");
        const note = lab.note();

        const base = baseVertices();
        const radii = TARGETS.map(function (target) {
            let r = 0;
            base.forEach(function (p) {
                const d = target.delta(p);
                r = Math.max(r, Math.hypot(d.x, d.y));
            });
            return r;
        });

        const bound = lab.el(svg, "circle", { cx: CX, cy: CY, class: "zone o-50" });
        const boundEdge = lab.el(svg, "circle", { cx: CX, cy: CY, class: "arrow ok" });
        lab.el(svg, "path", { d: pathOf(base), class: "ghost" });
        const shape = lab.el(svg, "path", { class: "gpu o-50" });
        lab.el(svg, "circle", { cx: CX, cy: CY, r: SCALE, class: "arrow dashed" });
        const deltas = lab.el(svg, "path", { class: "arrow hot" });
        const dots = lab.el(svg, "g", {});
        lab.el(svg, "circle", { cx: CX, cy: CY, r: 3, class: "solid" });
        lab.el(svg, "text", { x: 20, y: 26, class: "title", text: "v′ = v + Σ wₜ · Δₜ" });
        lab.el(svg, "text", { x: 20, y: 46, class: "small t-faint", text: "dashed: the base shape's bound" });
        lab.el(svg, "text", { x: 20, y: 62, class: "small t-ok", text: "solid: grown by Σ |wₜ| · rₜ" });
        const radiiText = lab.el(svg, "text", { x: 740, y: 26, class: "end small mono t-faint" });
        lab.set(radiiText, { text: "r = " + radii.map(function (r, t) { return `${TARGETS[t].name} ${r.toFixed(2)}`; }).join(" · ") });

        const dotEls = base.map(function () {
            return lab.el(dots, "circle", { r: 3, class: "mem" });
        });

        lab.draw(function () {
            const weights = sliders.map(function (s) { return Number(s.value); });
            let grow = 0;
            let active = 0;
            weights.forEach(function (w, t) {
                grow += Math.abs(w) * radii[t];
                if (w !== 0) active++;
            });
            const radius = 1 + grow;

            let far = 0;
            let deltaPath = "";
            const posed = base.map(function (p, i) {
                let x = p.x;
                let y = p.y;
                weights.forEach(function (w, t) {
                    if (w === 0) return;
                    const d = TARGETS[t].delta(p);
                    x += w * d.x;
                    y += w * d.y;
                });
                const q = { x: x, y: y };
                far = Math.max(far, Math.hypot(x, y));
                const a = toSvg(p);
                const b = toSvg(q);
                if (Math.hypot(b.x - a.x, b.y - a.y) > 2)
                    deltaPath += `M${a.x.toFixed(1)},${a.y.toFixed(1)}L${b.x.toFixed(1)},${b.y.toFixed(1)}`;
                lab.set(dotEls[i], { cx: b.x.toFixed(1), cy: b.y.toFixed(1) });
                return q;
            });

            lab.set(shape, { d: pathOf(posed) });
            lab.set(deltas, { d: deltaPath });
            lab.set(bound, { r: (radius * SCALE).toFixed(1) });
            lab.set(boundEdge, { r: (radius * SCALE).toFixed(1) });

            boundOut.set(radius.toFixed(2), "ok");
            farOut.set(far.toFixed(2), far <= radius + 1e-6 ? "ok" : "bad");
            readOut.set(`${active} of ${TARGETS.length}`, active === 0 ? "ok" : "gpu");
            if (far > radius + 1e-6)
                note.set("A vertex is outside the bound: this cannot happen, the bound is wrong.", "bad");
            else if (active === 0)
                note.set("Every weight is 0: the shape is the base, and no target's deltas are read.", "ok");
            else if (radius - far > 0.4)
                note.set(`The bound holds every vertex, ${(radius - far).toFixed(2)} wider than needed: the targets move different vertices, but the sum adds their longest deltas anyway.`, "ok");
            else
                note.set("The bound holds every vertex, and it is close: the longest moves add up on the same vertices.", "ok");
        });
    });
})();
