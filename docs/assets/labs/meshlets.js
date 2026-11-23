// Labs of docs/guides/meshlets.html.

// A top view of a closed shape cut into meshlets of a few edges each (a 2D stand-in for triangles). Each meshlet has a
// bounding circle and a normal cone; the cone test culls a meshlet when the camera sees only its back faces.
RasmLab.register("meshlet-cones", function (lab) {
    const svg = lab.svg(760, 340, "A top view of a closed shape cut into meshlets. A camera the reader drags; meshlets whose every edge faces away from it are culled by the cone test.");
    const size = lab.choice({ label: "Edges per meshlet", options: ["4", "8", "16", "32"], value: "8" });
    const culledOut = lab.readout("meshlets culled by their cone");
    const edgesOut = lab.readout("edges skipped");
    const backOut = lab.readout("edges that face away");
    const note = lab.note();

    const CENTER = { x: 500, y: 170 };
    const EDGES = 128;
    const points = [];
    for (let k = 0; k < EDGES; k++) {
        const a = 2 * Math.PI * k / EDGES;
        const r = 112 + 16 * Math.sin(3 * a + 0.4) + 9 * Math.cos(5 * a);
        points.push([CENTER.x + r * Math.cos(a), CENTER.y + r * Math.sin(a)]);
    }
    // each edge's outward normal: the points go counter-clockwise on screen (y down), so (dy, -dx) points out
    const edges = points.map(function (p, k) {
        const q = points[(k + 1) % EDGES];
        const dx = q[0] - p[0];
        const dy = q[1] - p[1];
        const length = Math.hypot(dx, dy);
        return { a: p, b: q, n: [dy / length, -dx / length] };
    });

    lab.el(svg, "polygon", { points: points.map(function (p) { return `${p[0].toFixed(1)},${p[1].toFixed(1)}`; }).join(" "), class: "zone" });
    const meshletGroup = lab.el(svg, "g", {});
    lab.el(svg, "text", { x: 20, y: 24, class: "small t-faint", text: "drag the camera around the shape" });
    const camera = lab.handle(svg, { x: 120, y: 170, label: "Move the camera", minX: 10, maxX: 750, minY: 10, maxY: 330 });

    lab.draw(function () {
        const per = Number(size.value);
        const count = EDGES / per;
        lab.clear(meshletGroup);
        let culled = 0;
        let skipped = 0;
        let back = 0;
        for (let m = 0; m < count; m++) {
            const group = edges.slice(m * per, (m + 1) * per);
            // the normal cone: the mean normal as axis, the widest normal sets the angle (meshoptimizer's cutoff is
            // the sine of the half angle, and a cone of 90 degrees or more cannot cull)
            let ax = 0;
            let ay = 0;
            group.forEach(function (e) { ax += e.n[0]; ay += e.n[1]; });
            const al = Math.hypot(ax, ay);
            ax /= al;
            ay /= al;
            let minDot = 1;
            group.forEach(function (e) { minDot = Math.min(minDot, e.n[0] * ax + e.n[1] * ay); });
            const cutoff = minDot <= 0 ? 1 : Math.sqrt(1 - minDot * minDot);
            // the bounding circle: the middle of the end points' box, out to the farthest point
            let minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
            group.forEach(function (e) {
                [e.a, e.b].forEach(function (p) { minX = Math.min(minX, p[0]); minY = Math.min(minY, p[1]); maxX = Math.max(maxX, p[0]); maxY = Math.max(maxY, p[1]); });
            });
            const cx = (minX + maxX) / 2;
            const cy = (minY + maxY) / 2;
            let radius = 0;
            group.forEach(function (e) { [e.a, e.b].forEach(function (p) { radius = Math.max(radius, Math.hypot(p[0] - cx, p[1] - cy)); }); });
            // the cone test: the camera is behind every edge's line
            const tx = cx - camera.x;
            const ty = cy - camera.y;
            const isCulled = tx * ax + ty * ay >= cutoff * Math.hypot(tx, ty) + radius;
            group.forEach(function (e) {
                const facesAway = (e.a[0] - camera.x) * e.n[0] + (e.a[1] - camera.y) * e.n[1] > 0;
                if (facesAway) back += 1;
            });
            if (isCulled) {
                culled += 1;
                skipped += per;
            }
            const d = group.map(function (e, k) { return `${k === 0 ? "M" : "L"}${e.a[0].toFixed(1)},${e.a[1].toFixed(1)}`; }).join("") +
                `L${group[group.length - 1].b[0].toFixed(1)},${group[group.length - 1].b[1].toFixed(1)}`;
            lab.el(meshletGroup, "path", { d: d, class: isCulled ? "arrow bad dashed" : (m % 2 === 0 ? "arrow gpu thick" : "arrow shader thick") });
            if (!isCulled && per >= 8) {
                lab.el(meshletGroup, "line", { x1: cx, y1: cy, x2: cx + ax * 26, y2: cy + ay * 26, class: "arrow", "marker-end": "url(#ah)" });
            }
        }
        culledOut.set(`${culled} / ${count}`, culled > 0 ? "ok" : "");
        edgesOut.set(`${Math.round(100 * skipped / EDGES)} %`, "gpu");
        backOut.set(`${Math.round(100 * back / EDGES)} %`);
        if (culled === 0) note.set("No meshlet is culled: from here every meshlet has an edge that may face the camera, or its cone is too wide.", "bad");
        else note.set(`${culled} meshlets face fully away and are skipped. The test never culls an edge that faces the camera; it misses some that face away, more so with big meshlets, whose cones are wider.`, "ok");
    });
});
