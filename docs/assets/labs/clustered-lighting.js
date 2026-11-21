// Labs of docs/guides/clustered-lighting.html.

// A side view of a camera's view, cut into tiles (rows) and slices (columns). Each light is a circle; a cluster counts
// the circles that touch its box, the box around its four corners.
RasmLab.register("clusters", function (lab) {
    const svg = lab.svg(760, 340, "A side view of a camera's view cut into 6 tiles and 10 depth slices. Lights are circles; each cluster is shaded by how many lights touch it.");
    const slicing = lab.choice({ label: "Slices", options: [{ value: "grow", label: "growing" }, { value: "even", label: "even" }], value: "grow" });
    const range = lab.slider({ label: "Light range", min: 15, max: 110, step: 5, value: 40 });
    const mostOut = lab.readout("most lights in one cluster");
    const averageOut = lab.readout("lights per lit cluster, on average");
    const dragOut = lab.readout("clusters the bright light is in");
    const note = lab.note();

    const EYE = { x: 30, y: 170 };
    const TAN_HALF = 0.42;      // the half field of view's tangent
    const NEAR = 12;
    const MAX_DEPTH = 715;
    const TILES = 6;
    const SLICES = 10;

    let seed = 11;
    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }
    // lights in view space: depth along the axis, height above it
    const lights = [];
    for (let k = 0; k < 16; k++) {
        const depth = 60 + random() * 620;
        lights.push({ depth: depth, height: (random() * 2 - 1) * depth * TAN_HALF * 0.9 });
    }

    const cellGroup = lab.el(svg, "g", {});
    const lineGroup = lab.el(svg, "g", {});
    const lightGroup = lab.el(svg, "g", {});
    lab.el(svg, "circle", { cx: EYE.x, cy: EYE.y, r: 6, class: "solid" });
    lab.el(svg, "text", { x: EYE.x + 4, y: EYE.y + 24, class: "small", text: "camera" });
    const drag = lab.handle(svg, { x: 250, y: 150, label: "Move the bright light", minX: 60, maxX: 740, minY: 20, maxY: 320 });
    const lightFill = lab.css("--light");

    // The slices cover the lights' depths: from the nearest circle's near side, no nearer than the near plane, to the
    // farthest circle's far side.
    function sliceDepth(k, near, far) {
        const t = k / SLICES;
        return slicing.value === "even" ? near + (far - near) * t : near * Math.pow(far / near, t);
    }
    function toScreen(depth, height) {
        return [EYE.x + depth, EYE.y - height];
    }

    lab.draw(function () {
        const all = lights.concat([{ depth: drag.x - EYE.x, height: EYE.y - drag.y, dragged: true }]);
        const r = range.value;
        let near = Infinity;
        let far = -Infinity;
        all.forEach(function (l) {
            near = Math.min(near, l.depth - r);
            far = Math.max(far, l.depth + r);
        });
        near = Math.max(near, NEAR);
        far = Math.min(far, MAX_DEPTH);

        lab.clear(cellGroup);
        lab.clear(lineGroup);
        lab.clear(lightGroup);
        let most = 0;
        let sum = 0;
        let lit = 0;
        let draggedIn = 0;
        const cells = [];
        for (let t = 0; t < TILES; t++) {
            const y0 = -1 + 2 * t / TILES;
            const y1 = -1 + 2 * (t + 1) / TILES;
            for (let s = 0; s < SLICES; s++) {
                const d0 = sliceDepth(s, near, far);
                const d1 = sliceDepth(s + 1, near, far);
                const corners = [[d0, y0 * d0 * TAN_HALF], [d1, y0 * d1 * TAN_HALF], [d1, y1 * d1 * TAN_HALF], [d0, y1 * d0 * TAN_HALF]];
                const minH = Math.min(corners[0][1], corners[1][1]);
                const maxH = Math.max(corners[2][1], corners[3][1]);
                let count = 0;
                all.forEach(function (l) {
                    // the sphere-box test: the box's closest point to the center, within the radius
                    const cx = Math.min(Math.max(l.depth, d0), d1);
                    const cy = Math.min(Math.max(l.height, minH), maxH);
                    const dx = l.depth - cx;
                    const dy = l.height - cy;
                    if (dx * dx + dy * dy <= r * r) {
                        count += 1;
                        if (l.dragged) draggedIn += 1;
                    }
                });
                most = Math.max(most, count);
                if (count > 0) {
                    sum += count;
                    lit += 1;
                }
                cells.push({ corners: corners, count: count });
            }
        }
        cells.forEach(function (c) {
            const points = c.corners.map(function (p) { const q = toScreen(p[0], p[1]); return `${q[0].toFixed(1)},${q[1].toFixed(1)}`; }).join(" ");
            const cell = lab.el(cellGroup, "polygon", { points: points, class: "grid" });
            if (c.count > 0) lab.set(cell, { fill: lightFill, "fill-opacity": Math.min(0.12 + 0.18 * c.count, 0.9).toFixed(2) });
            if (c.count > 1) {
                const mid = toScreen((c.corners[0][0] + c.corners[1][0]) / 2, (c.corners[0][1] + c.corners[2][1] + c.corners[1][1] + c.corners[3][1]) / 4);
                if (c.corners[1][0] - c.corners[0][0] > 26 && c.corners[2][1] - c.corners[0][1] > 16) lab.el(cellGroup, "text", { x: mid[0], y: mid[1] + 4, class: "mid small", text: String(c.count) });
            }
        });
        // the view's edges
        const top = toScreen(MAX_DEPTH, MAX_DEPTH * TAN_HALF);
        const bottom = toScreen(MAX_DEPTH, -MAX_DEPTH * TAN_HALF);
        lab.el(lineGroup, "path", { d: `M${EYE.x},${EYE.y}L${top[0]},${top[1]}M${EYE.x},${EYE.y}L${bottom[0]},${bottom[1]}`, class: "arrow" });
        all.forEach(function (l) {
            const p = toScreen(l.depth, l.height);
            lab.el(lightGroup, "circle", { cx: p[0], cy: p[1], r: r, class: l.dragged ? "hot" : "ghost" });
            lab.el(lightGroup, "circle", { cx: p[0], cy: p[1], r: 3, class: l.dragged ? "accent" : "light" });
        });

        mostOut.set(String(most), most > 6 ? "bad" : "gpu");
        averageOut.set(`${lit > 0 ? (sum / lit).toFixed(1) : "0"} of ${all.length}`, "gpu");
        dragOut.set(String(draggedIn), draggedIn > 0 ? "ok" : "bad");
        if (draggedIn === 0) note.set("The bright light is outside the view: no cluster lists it, and no pixel pays for it.", "ok");
        else if (slicing.value === "even") note.set(`Even slices: the near clusters are long and thin, the far ones flat. The bright light is in ${draggedIn} clusters.`, "ok");
        else note.set(`Growing slices keep the clusters about as deep as they are wide. A pixel shades only its cluster's lights, ${(sum / Math.max(lit, 1)).toFixed(1)} on average instead of ${all.length}.`, "ok");
    });
});
