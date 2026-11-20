// Labs of docs/guides/frustum-culling.html.

// A top view: a camera that turns, its frustum, and a field of bounding spheres tested against four planes.
RasmLab.register("frustum-cull", function (lab) {
    const svg = lab.svg(760, 340, "A top view of a camera, its view frustum and many bounding spheres. Spheres that touch the frustum are drawn; the others are culled.");
    const fov = lab.slider({ label: "Field of view", min: 20, max: 120, step: 2, value: 60, format: function (v) { return `${v}°`; } });
    const far = lab.slider({ label: "Far plane", min: 150, max: 600, step: 10, value: 420 });
    const drawnOut = lab.readout("spheres drawn");
    const culledOut = lab.readout("culled: work the GPU skips");
    const probeOut = lab.readout("the big sphere: distance to its nearest plane");
    const note = lab.note();

    const CAMERA = { x: 110, y: 170 };
    const NEAR = 26;
    const PROBE_R = 26;

    // the same scene every time: a small repeatable random generator
    let seed = 7;
    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }
    const spheres = [];
    for (let k = 0; k < 70; k++) spheres.push({ x: 30 + random() * 700, y: 20 + random() * 300, r: 5 + random() * 11 });

    const frustum = lab.el(svg, "polygon", { class: "cpu o-50" });
    const circles = spheres.map(function (s) { return lab.el(svg, "circle", { cx: s.x, cy: s.y, r: s.r }); });
    const probeCircle = lab.el(svg, "circle", { r: PROBE_R });
    const probeLine = lab.el(svg, "line", { class: "arrow hot" });
    const outline = lab.el(svg, "polygon", { class: "hot" });
    lab.el(svg, "circle", { cx: CAMERA.x, cy: CAMERA.y, r: 7, class: "solid" });
    lab.el(svg, "text", { x: CAMERA.x, y: CAMERA.y + 24, class: "mid small", text: "camera" });
    const aim = lab.handle(svg, { x: 330, y: 170, label: "Turn the camera", minX: 10, maxX: 750, minY: 10, maxY: 330 });
    const probe = lab.handle(svg, { x: 470, y: 70, r: 7, label: "Move the big sphere", minX: 10, maxX: 750, minY: 10, maxY: 330 });

    lab.draw(function () {
        const yaw = Math.atan2(aim.y - CAMERA.y, aim.x - CAMERA.x);
        const half = fov.value * Math.PI / 360;
        const forward = [Math.cos(yaw), Math.sin(yaw)];
        const left = [Math.cos(yaw - half), Math.sin(yaw - half)];
        const right = [Math.cos(yaw + half), Math.sin(yaw + half)];
        // signed distance to each plane, positive inside: left, right, near, far
        function distances(x, y) {
            const dx = x - CAMERA.x;
            const dy = y - CAMERA.y;
            const along = dx * forward[0] + dy * forward[1];
            return [-left[1] * dx + left[0] * dy, right[1] * dx - right[0] * dy, along - NEAR, far.value - along];
        }
        function visible(x, y, r) {
            return Math.min.apply(null, distances(x, y)) >= -r;
        }

        const nearScale = NEAR / Math.cos(half);
        const farScale = far.value / Math.cos(half);
        const corners = [
            [CAMERA.x + left[0] * nearScale, CAMERA.y + left[1] * nearScale],
            [CAMERA.x + left[0] * farScale, CAMERA.y + left[1] * farScale],
            [CAMERA.x + right[0] * farScale, CAMERA.y + right[1] * farScale],
            [CAMERA.x + right[0] * nearScale, CAMERA.y + right[1] * nearScale],
        ].map(function (p) { return `${p[0].toFixed(1)},${p[1].toFixed(1)}`; }).join(" ");
        lab.set(frustum, { points: corners });
        lab.set(outline, { points: corners });

        let drawn = 0;
        spheres.forEach(function (s, k) {
            const inside = visible(s.x, s.y, s.r);
            if (inside) drawn += 1;
            lab.set(circles[k], { class: inside ? "gpu" : "ghost" });
        });

        const d = distances(probe.x, probe.y);
        const names = ["left", "right", "near", "far"];
        let worst = 0;
        for (let k = 1; k < 4; k++) if (d[k] < d[worst]) worst = k;
        const probeIn = d[worst] >= -PROBE_R;
        lab.set(probeCircle, { cx: probe.x, cy: probe.y, class: probeIn ? "ok" : "bad" });
        // a line from the sphere's center to its nearest plane
        const normals = [[-left[1], left[0]], [right[1], -right[0]], forward, [-forward[0], -forward[1]]];
        lab.set(probeLine, { x1: probe.x, y1: probe.y, x2: probe.x - normals[worst][0] * d[worst], y2: probe.y - normals[worst][1] * d[worst] });

        drawnOut.set(`${drawn} / ${spheres.length}`, "gpu");
        culledOut.set(`${Math.round(100 * (spheres.length - drawn) / spheres.length)} %`);
        probeOut.set(`${d[worst].toFixed(0)} (${names[worst]})`, probeIn ? "ok" : "bad");
        if (d[worst] >= PROBE_R) note.set(`The big sphere is fully inside: every distance is larger than its radius (${PROBE_R}).`, "ok");
        else if (probeIn) note.set(`The big sphere crosses the ${names[worst]} plane: the distance is between −${PROBE_R} and ${PROBE_R}. It is kept.`, "ok");
        else note.set(`The big sphere is culled: its center is more than one radius (${PROBE_R}) outside the ${names[worst]} plane.`, "bad");
    });
});
