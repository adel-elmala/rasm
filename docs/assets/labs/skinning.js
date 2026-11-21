// Labs of docs/guides/skinning.html.

// A two-bone arm seen from the side. The strip of vertices is modelled straight (the bind pose). The upper arm's
// joint stays put; the forearm's joint turns by the elbow angle. Each vertex has a weight for the forearm that
// rises from 0 to 1 across a band around the elbow, and its posed place is the linear blend of the two joint
// matrices: (1 − w) · J0 · v + w · J1 · v, with J1 = T(elbow) · R(angle) · T(−elbow).
(function () {
    const SHOULDER_X = 150;
    const BASE_Y = 260;
    const BONE = 200;
    const HALF = 26;
    const COLUMNS = 25;

    function toSvg(p) {
        return { x: SHOULDER_X + p.x, y: BASE_Y - p.y };
    }

    // The forearm's weight at a distance x from the shoulder: a smooth step across [elbow − band, elbow + band].
    function forearmWeight(x, band) {
        if (band <= 0.001) return x < BONE ? 0 : 1;
        const t = Math.min(Math.max((x - (BONE - band)) / (2 * band), 0), 1);
        return t * t * (3 - 2 * t);
    }

    // The forearm's joint matrix applied to a bind-pose point: move the elbow to the origin (the inverse bind
    // matrix), turn, move back (the joint's world matrix now).
    function forearm(p, angle) {
        const c = Math.cos(angle);
        const s = Math.sin(angle);
        const x = p.x - BONE;
        return { x: BONE + c * x - s * p.y, y: s * x + c * p.y };
    }

    function skin(p, w, angle) {
        const q = forearm(p, angle);
        return { x: (1 - w) * p.x + w * q.x, y: (1 - w) * p.y + w * q.y };
    }

    // The bind-pose vertices: a top and a bottom row, from the shoulder to the wrist.
    function bindVertices() {
        const top = [];
        const bottom = [];
        for (let i = 0; i < COLUMNS; i++) {
            const x = (i / (COLUMNS - 1)) * 2 * BONE;
            top.push({ x: x, y: HALF });
            bottom.push({ x: x, y: -HALF });
        }
        return { top: top, bottom: bottom };
    }

    function pathOf(points) {
        return points.map(function (p, i) {
            const s = toSvg(p);
            return `${i === 0 ? "M" : "L"}${s.x.toFixed(1)},${s.y.toFixed(1)}`;
        }).join("");
    }

    function weightClass(w) {
        if (w <= 0.02) return "cpu";
        if (w >= 0.98) return "shader";
        return "hot";
    }

    RasmLab.register("bend-arm", function (lab) {
        const svg = lab.svg(760, 330, "An arm made of a strip of vertices, upper arm on the left, forearm on the right. The faint strip is the bind pose, straight. The colored strip is the skinned pose: the forearm turned by the elbow angle. Vertex dots are blue when they follow only the upper arm, purple when they follow only the forearm, orange when they mix both.");
        const angleControl = lab.slider({ label: "Elbow angle", min: 0, max: 150, step: 1, value: 60, format: function (v) { return `${v}°`; } });
        const bandControl = lab.slider({ label: "Blend band", min: 0, max: 120, step: 2, value: 40, format: function (v) { return v === 0 ? "rigid" : `±${v}`; } });
        const weightsOut = lab.readout("elbow vertex: upper / forearm");
        const thicknessOut = lab.readout("arm thickness at the elbow");
        const note = lab.note();

        const bind = bindVertices();
        const ghost = lab.el(svg, "path", { class: "ghost" });
        const strip = lab.el(svg, "path", { class: "gpu o-50" });
        const rungs = lab.el(svg, "path", { class: "grid" });
        const outline = lab.el(svg, "path", { class: "arrow gpu" });
        const dots = lab.el(svg, "g", {});
        const upperBone = lab.el(svg, "line", { class: "arrow cpu thick" });
        const foreBone = lab.el(svg, "line", { class: "arrow shader thick" });
        const shoulder = lab.el(svg, "circle", { r: 7, class: "mem" });
        const elbow = lab.el(svg, "circle", { r: 7, class: "mem" });
        const wrist = lab.el(svg, "circle", { r: 5, class: "mem" });
        lab.el(svg, "text", { x: 20, y: 24, class: "title", text: "(1 − w) · J₀ · v + w · J₁ · v" });
        const elbowLabel = lab.el(svg, "text", { class: "small t-faint", text: "elbow" });
        lab.el(svg, "text", { x: SHOULDER_X - 10, y: BASE_Y + 50, class: "end small t-faint", text: "shoulder" });

        const ghostPath = pathOf(bind.top) + pathOf(bind.bottom.slice().reverse()).replace("M", "L") + "Z";
        lab.set(ghost, { d: ghostPath });

        lab.draw(function () {
            const angle = (angleControl.value * Math.PI) / 180;
            const band = bandControl.value;
            const top = bind.top.map(function (p) { return skin(p, forearmWeight(p.x, band), angle); });
            const bottom = bind.bottom.map(function (p) { return skin(p, forearmWeight(p.x, band), angle); });
            const shape = pathOf(top) + pathOf(bottom.slice().reverse()).replace("M", "L") + "Z";
            lab.set(strip, { d: shape });
            lab.set(outline, { d: shape });
            let rungPath = "";
            for (let i = 0; i < COLUMNS; i++) {
                const a = toSvg(top[i]);
                const b = toSvg(bottom[i]);
                rungPath += `M${a.x.toFixed(1)},${a.y.toFixed(1)}L${b.x.toFixed(1)},${b.y.toFixed(1)}`;
            }
            lab.set(rungs, { d: rungPath });

            lab.clear(dots);
            for (let i = 0; i < COLUMNS; i++) {
                const w = forearmWeight(bind.top[i].x, band);
                [top[i], bottom[i]].forEach(function (p) {
                    const s = toSvg(p);
                    lab.el(dots, "circle", { cx: s.x, cy: s.y, r: 3.5, class: weightClass(w) });
                });
            }

            const s0 = toSvg({ x: 0, y: 0 });
            const s1 = toSvg({ x: BONE, y: 0 });
            const s2 = toSvg(forearm({ x: 2 * BONE, y: 0 }, angle));
            lab.set(upperBone, { x1: s0.x, y1: s0.y, x2: s1.x, y2: s1.y });
            lab.set(foreBone, { x1: s1.x, y1: s1.y, x2: s2.x, y2: s2.y });
            lab.set(shoulder, { cx: s0.x, cy: s0.y });
            lab.set(elbow, { cx: s1.x, cy: s1.y });
            lab.set(wrist, { cx: s2.x, cy: s2.y });
            lab.set(elbowLabel, { x: s1.x - 14, y: s1.y + 46 });

            // The column at the elbow: its weight, and the distance between its top and bottom vertex.
            const middle = (COLUMNS - 1) / 2;
            const w = forearmWeight(bind.top[middle].x, band);
            weightsOut.set(`${(1 - w).toFixed(2)} / ${w.toFixed(2)}`, w > 0.02 && w < 0.98 ? "hot" : "");
            const dx = top[middle].x - bottom[middle].x;
            const dy = top[middle].y - bottom[middle].y;
            const thickness = Math.sqrt(dx * dx + dy * dy) / (2 * HALF);
            thicknessOut.set(`${Math.round(thickness * 100)} %`, thickness < 0.75 ? "bad" : "ok");

            if (band === 0 && angleControl.value > 0)
                note.set("Rigid: each vertex follows one joint. The two halves overlap inside the bend and open a crack outside.", "bad");
            else if (thickness < 0.75)
                note.set("The mix of two matrices pulls the vertices toward each other: the elbow loses volume. Real rigs add joints or use dual quaternions.", "bad");
            else if (angleControl.value === 0)
                note.set("Elbow straight: both joint matrices are the identity, and every vertex stays at its bind pose.", "ok");
            else
                note.set("The blend band bends the skin smoothly: each vertex near the elbow is a weighted mix of where the two joints put it.", "ok");
        });
    });

    // The same arm. Each joint has a circle around the bind-pose vertices it moves (weight above 0). The joint matrix
    // moves its circle's center, and the circle around all moved circles (the center of their box, the radius to the
    // farthest edge) must hold every skinned vertex.
    RasmLab.register("skinned-bounds", function (lab) {
        const svg = lab.svg(760, 340, "The skinned arm with two dashed circles: one around the vertices the upper arm moves, one around those the forearm moves, each moved by its joint. A third, larger circle surrounds both. Every skinned vertex lies inside the large circle.");
        const angleControl = lab.slider({ label: "Elbow angle", min: 0, max: 150, step: 1, value: 110, format: function (v) { return `${v}°`; } });
        const bandControl = lab.slider({ label: "Blend band", min: 0, max: 120, step: 2, value: 40, format: function (v) { return v === 0 ? "rigid" : `±${v}`; } });
        const radiusOut = lab.readout("bounding radius");
        const insideOut = lab.readout("vertices inside");
        const note = lab.note();

        const bind = bindVertices();
        const vertices = bind.top.concat(bind.bottom);
        const union = lab.el(svg, "circle", { class: "zone" });
        const unionEdge = lab.el(svg, "circle", { class: "arrow ok thick nofill" });
        const sphereGroup = lab.el(svg, "g", {});
        const strip = lab.el(svg, "path", { class: "gpu o-50" });
        const dots = lab.el(svg, "g", {});
        lab.el(svg, "text", { x: 20, y: 24, class: "title", text: "One circle per joint, moved by its matrix; one circle around them all" });

        // A joint's circle in the bind pose: the box of its vertices, then the farthest one.
        function jointCircle(band, joint) {
            const moved = vertices.filter(function (p) {
                const w = forearmWeight(p.x, band);
                return joint === 0 ? w < 1 : w > 0;
            });
            let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
            moved.forEach(function (p) {
                minX = Math.min(minX, p.x); maxX = Math.max(maxX, p.x);
                minY = Math.min(minY, p.y); maxY = Math.max(maxY, p.y);
            });
            const c = { x: (minX + maxX) / 2, y: (minY + maxY) / 2 };
            let r = 0;
            moved.forEach(function (p) { r = Math.max(r, Math.hypot(p.x - c.x, p.y - c.y)); });
            return { c: c, r: r };
        }

        lab.draw(function () {
            const angle = (angleControl.value * Math.PI) / 180;
            const band = bandControl.value;
            // The joints' matrices are a turn and a move, so a circle keeps its radius.
            const circles = [jointCircle(band, 0), jointCircle(band, 1)];
            circles[1] = { c: forearm(circles[1].c, angle), r: circles[1].r };

            let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
            circles.forEach(function (k) {
                minX = Math.min(minX, k.c.x - k.r); maxX = Math.max(maxX, k.c.x + k.r);
                minY = Math.min(minY, k.c.y - k.r); maxY = Math.max(maxY, k.c.y + k.r);
            });
            const center = { x: (minX + maxX) / 2, y: (minY + maxY) / 2 };
            let radius = 0;
            circles.forEach(function (k) { radius = Math.max(radius, Math.hypot(k.c.x - center.x, k.c.y - center.y) + k.r); });

            // The view fits the big circle.
            const scale = Math.min(1, 150 / radius);
            const view = function (p) { return { x: 380 + (p.x - center.x) * scale, y: 185 - (p.y - center.y) * scale }; };
            const cs = view(center);
            lab.set(union, { cx: cs.x, cy: cs.y, r: radius * scale });
            lab.set(unionEdge, { cx: cs.x, cy: cs.y, r: radius * scale });
            lab.clear(sphereGroup);
            circles.forEach(function (k, j) {
                const s = view(k.c);
                lab.el(sphereGroup, "circle", { cx: s.x, cy: s.y, r: k.r * scale, class: `arrow ${j === 0 ? "cpu" : "shader"} dashed nofill` });
                lab.el(sphereGroup, "circle", { cx: s.x, cy: s.y, r: 4, class: j === 0 ? "cpu" : "shader" });
            });

            const posedTop = bind.top.map(function (p) { return skin(p, forearmWeight(p.x, band), angle); });
            const posedBottom = bind.bottom.map(function (p) { return skin(p, forearmWeight(p.x, band), angle); });
            const outline = posedTop.concat(posedBottom.slice().reverse()).map(function (p, i) {
                const s = view(p);
                return `${i === 0 ? "M" : "L"}${s.x.toFixed(1)},${s.y.toFixed(1)}`;
            }).join("") + "Z";
            lab.set(strip, { d: outline });

            lab.clear(dots);
            let inside = 0;
            const posed = posedTop.concat(posedBottom);
            posed.forEach(function (p) {
                const ok = Math.hypot(p.x - center.x, p.y - center.y) <= radius + 1e-6;
                if (ok) inside++;
                const s = view(p);
                lab.el(dots, "circle", { cx: s.x, cy: s.y, r: 3, class: ok ? "ok" : "bad" });
            });
            radiusOut.set(`${Math.round(radius)}`, "gpu");
            insideOut.set(`${inside} / ${posed.length}`, inside === posed.length ? "ok" : "bad");
            if (inside === posed.length)
                note.set("Every skinned vertex is inside: each one is a mix of points inside the joints' circles, and the big circle holds both circles and every mix.", "ok");
            else
                note.set("A vertex is outside the bounds: it would be culled while on screen.", "bad");
        });
    });
})();
