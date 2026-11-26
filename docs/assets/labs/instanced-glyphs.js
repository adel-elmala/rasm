// Labs of docs/guides/instanced-glyphs.html.

// A top view of a field of glyphs and a camera's view wedge. Each glyph is tested as the cull pass tests it: its
// sphere against the wedge's left, right, near and far planes. A kept glyph gets a LOD by its depth from the camera,
// counts in its LOD and takes a place there; the list holds the LOD 0 glyphs, then LOD 1, then LOD 2, and each LOD's
// draw command starts at the sum of the counts before it.
RasmLab.register("glyph-cull", function (lab) {
    const svg = lab.svg(760, 420, "A top view of a camera's view wedge over a field of arrows. Arrows inside are kept and colored by their LOD; the others are culled. Below, the compacted list of the kept arrows, grouped by LOD.");
    const keptOut = lab.readout("glyphs kept");
    const countsOut = lab.readout("instances: LOD 0 / 1 / 2");
    const firstOut = lab.readout("first instance: LOD 0 / 1 / 2");
    const note = lab.note();

    const FIELD_H = 300;
    const HALF_FOV = 30 * Math.PI / 180;
    const NEAR = 20;
    const FAR = 520;
    const LOD_DEPTHS = [150, 300];          // a glyph nearer than the first is LOD 0, then LOD 1, then LOD 2
    const RADIUS = 7;
    const CLASSES = ["gpu", "mem", "shader"];

    // the same field every time: a small repeatable random generator, a jittered grid
    let seed = 11;
    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }
    const glyphs = [];
    for (let row = 0; row < 7; row++) {
        for (let column = 0; column < 15; column++) {
            const x = 60 + column * 46 + (random() - 0.5) * 24;
            const y = 24 + row * 40 + (random() - 0.5) * 18;
            const angle = Math.sin(x / 90) + Math.cos(y / 70) * 1.3;
            glyphs.push({ x: x, y: y, dx: Math.cos(angle) * 12, dy: Math.sin(angle) * 12 });
        }
    }

    const wedge = lab.el(svg, "polygon", { class: "cpu o-25" });
    const ring0 = lab.el(svg, "path", { class: "arrow dashed o-50" });
    const ring1 = lab.el(svg, "path", { class: "arrow dashed o-50" });
    const lines = glyphs.map(function () { return lab.el(svg, "line", { class: "arrow thick" }); });
    const camera = lab.handle(svg, { x: 70, y: 260, label: "Move the camera", minX: 10, maxX: 750, minY: 10, maxY: FIELD_H - 10 });
    const aim = lab.handle(svg, { x: 380, y: 120, r: 7, label: "Turn the camera", minX: 10, maxX: 750, minY: 10, maxY: FIELD_H - 10 });
    lab.el(svg, "path", { d: `M0,${FIELD_H + 6} H760`, class: "grid" });
    lab.el(svg, "text", { x: 20, y: FIELD_H + 28, text: "the list, in draw order" });
    const listGroup = lab.el(svg, "g", {});
    const labels = [0, 1, 2].map(function (lod) {
        return lab.el(svg, "text", { x: 20 + lod * 250, y: FIELD_H + 106, class: `t-${CLASSES[lod]}` });
    });

    lab.draw(function () {
        const yaw = Math.atan2(aim.y - camera.y, aim.x - camera.x);
        const forward = [Math.cos(yaw), Math.sin(yaw)];
        const left = [Math.cos(yaw - HALF_FOV), Math.sin(yaw - HALF_FOV)];
        const right = [Math.cos(yaw + HALF_FOV), Math.sin(yaw + HALF_FOV)];
        const nearScale = NEAR / Math.cos(HALF_FOV);
        const farScale = FAR / Math.cos(HALF_FOV);
        lab.set(wedge, {
            points: [
                [camera.x + left[0] * nearScale, camera.y + left[1] * nearScale],
                [camera.x + left[0] * farScale, camera.y + left[1] * farScale],
                [camera.x + right[0] * farScale, camera.y + right[1] * farScale],
                [camera.x + right[0] * nearScale, camera.y + right[1] * nearScale],
            ].map(function (p) { return `${p[0].toFixed(1)},${p[1].toFixed(1)}`; }).join(" "),
        });
        // the depths where the LOD changes, as lines across the wedge
        [ring0, ring1].forEach(function (ring, k) {
            const scale = LOD_DEPTHS[k] / Math.cos(HALF_FOV);
            lab.set(ring, { d: `M${(camera.x + left[0] * scale).toFixed(1)},${(camera.y + left[1] * scale).toFixed(1)} L${(camera.x + right[0] * scale).toFixed(1)},${(camera.y + right[1] * scale).toFixed(1)}` });
        });

        const counts = [0, 0, 0];
        const kept = [];
        glyphs.forEach(function (g, k) {
            const dx = g.x - camera.x;
            const dy = g.y - camera.y;
            const depth = dx * forward[0] + dy * forward[1];
            // signed distances, positive inside: left, right, near, far
            const d = Math.min(-left[1] * dx + left[0] * dy, right[1] * dx - right[0] * dy, depth - NEAR, FAR - depth);
            const visible = d >= -RADIUS;
            let lod = -1;
            if (visible) {
                lod = depth - RADIUS < LOD_DEPTHS[0] ? 0 : (depth - RADIUS < LOD_DEPTHS[1] ? 1 : 2);
                kept.push({ index: k, lod: lod, place: counts[lod] });
                counts[lod] += 1;
            }
            lab.set(lines[k], {
                x1: g.x - g.dx, y1: g.y - g.dy, x2: g.x + g.dx, y2: g.y + g.dy,
                class: visible ? `arrow thick ${CLASSES[lod]}` : "arrow o-25",
            });
        });

        // the compacted list: offset = counts of the LODs before + place
        const first = [0, counts[0], counts[0] + counts[1]];
        lab.clear(listGroup);
        const total = kept.length;
        const width = total > 0 ? Math.min(24, 720 / total) : 24;
        kept.forEach(function (g) {
            const offset = first[g.lod] + g.place;
            lab.el(listGroup, "rect", { x: 20 + offset * width, y: FIELD_H + 38, width: Math.max(width - 1, 1), height: 30, class: CLASSES[g.lod] });
        });
        [0, 1, 2].forEach(function (lod) {
            if (counts[lod] > 0) lab.el(listGroup, "path", { d: `M${20 + first[lod] * width},${FIELD_H + 74} v6`, class: "arrow" });
            lab.set(labels[lod], { text: `LOD ${lod}: ${counts[lod]} from ${first[lod]}` });
        });

        keptOut.set(`${total} / ${glyphs.length}`, "gpu");
        countsOut.set(`${counts[0]} / ${counts[1]} / ${counts[2]}`);
        firstOut.set(`${first[0]} / ${first[1]} / ${first[2]}`);
        if (total === 0) note.set("Nothing is in view: every command draws 0 instances, and the GPU does almost no work.", "bad");
        else note.set(`${glyphs.length - total} glyphs culled. The list is dense: LOD 1 starts right after LOD 0's ${counts[0]} glyphs, LOD 2 after the ${first[2]} before it.`, "ok");
    });
});

// A line from value a to value b, colored two ways: the end colors blended, or the blended value looked up in the
// colormap at each pixel. The colormap is viridis, sampled at 9 stops.
RasmLab.register("glyph-interpolate", function (lab) {
    const W = 760;
    const H = 200;
    const ctx = lab.canvas(W, H, "Three strips: the colormap with the two end values marked, the line colored by blending the end colors, and the line colored by looking up the blended value.");
    const startValue = lab.slider({ label: "Value at the start", min: 0, max: 1, step: 0.01, value: 0.05, format: function (v) { return v.toFixed(2); } });
    const endValue = lab.slider({ label: "Value at the end", min: 0, max: 1, step: 0.01, value: 0.95, format: function (v) { return v.toFixed(2); } });
    const diffOut = lab.readout("largest difference between the two lines");
    const note = lab.note();

    const VIRIDIS = [[68, 1, 84], [72, 40, 120], [62, 74, 137], [49, 104, 142], [38, 130, 142], [31, 158, 137], [53, 183, 121], [109, 205, 89], [180, 222, 44], [253, 231, 37]];
    function colormap(t) {
        const x = Math.min(Math.max(t, 0), 1) * (VIRIDIS.length - 1);
        const i = Math.min(Math.floor(x), VIRIDIS.length - 2);
        const f = x - i;
        return [0, 1, 2].map(function (c) { return VIRIDIS[i][c] * (1 - f) + VIRIDIS[i + 1][c] * f; });
    }
    function fill(rgb) {
        return `rgb(${rgb.map(function (c) { return Math.round(c); }).join(",")})`;
    }
    const X0 = 140;
    const X1 = 740;

    lab.draw(function () {
        const a = startValue.value;
        const b = endValue.value;
        ctx.clearRect(0, 0, W, H);
        ctx.font = "12px sans-serif";
        ctx.fillStyle = lab.css("--fg");
        ctx.fillText("the colormap", 10, 34);
        ctx.fillText("blend the colors", 10, 104);
        ctx.fillText("look up the value", 10, 164);
        const colorA = colormap(a);
        const colorB = colormap(b);
        let worst = 0;
        for (let x = X0; x < X1; x++) {
            const s = (x - X0) / (X1 - X0 - 1);
            ctx.fillStyle = fill(colormap(s));
            ctx.fillRect(x, 20, 1, 24);
            const blended = [0, 1, 2].map(function (c) { return colorA[c] * (1 - s) + colorB[c] * s; });
            const looked = colormap(a * (1 - s) + b * s);
            ctx.fillStyle = fill(blended);
            ctx.fillRect(x, 86, 1, 30);
            ctx.fillStyle = fill(looked);
            ctx.fillRect(x, 146, 1, 30);
            const d = Math.hypot(blended[0] - looked[0], blended[1] - looked[1], blended[2] - looked[2]) / Math.sqrt(3 * 255 * 255);
            worst = Math.max(worst, d);
        }
        // the two ends on the colormap
        ctx.fillStyle = lab.css("--fg");
        [a, b].forEach(function (t) {
            const x = X0 + t * (X1 - X0 - 1);
            ctx.fillRect(x - 1, 14, 3, 36);
        });
        diffOut.set(`${Math.round(worst * 100)} %`, worst > 0.08 ? "bad" : "ok");
        if (worst > 0.08) note.set("The ends are far apart in the colormap: blending their colors skips the colors between them.", "bad");
        else note.set("The ends are close in the colormap: both lines nearly agree. Looking up the value is right for any ends.", "ok");
    });
});
