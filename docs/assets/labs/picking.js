// Labs of docs/guides/picking.html.

// A top view: a camera, a row of 32 pixels on its image plane, and a field of objects. The reader picks a rect of the
// row; the frustum through those pixels culls the objects, and the ray through each pixel's centre finds what it shows.
RasmLab.register("pick-frustum", function (lab) {
    const svg = lab.svg(760, 360, "A top view of a camera and its image as a row of 32 pixels. A rect of pixels is picked: the narrow frustum through it keeps only the objects that touch it, and each pixel's ray finds the nearest one.");
    const start = lab.slider({ label: "First pixel of the rect", min: 0, max: 31, step: 1, value: 14 });
    const width = lab.slider({ label: "Rect width (pixels)", min: 1, max: 32, step: 1, value: 3 });
    const keptOut = lab.readout("objects the pick draws");
    const hitOut = lab.readout("the centre pixel shows");
    const note = lab.note();

    const CAMERA = { x: 70, y: 180 };
    const PLANE = 60;           // the image plane, this far in front of the camera
    const PIXELS = 32;
    const HALF = 0.62;          // tan of half the field of view: the image plane spans y +- PLANE * HALF
    const FAR = 690;

    let seed = 11;
    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }
    const objects = [];
    for (let k = 0; k < 26; k++) objects.push({ x: 220 + random() * 500, y: 40 + random() * 280, r: 10 + random() * 16, id: k + 1 });

    // A pixel's edge as the y of the image plane, from the top of the row.
    function edge(i) {
        return CAMERA.y - PLANE * HALF + (2 * PLANE * HALF) * i / PIXELS;
    }

    const full = lab.el(svg, "polygon", { class: "zone" });
    const narrow = lab.el(svg, "polygon", { class: "gpu o-50" });
    const rays = lab.el(svg, "g", {});
    const circles = objects.map(function (o) { return lab.el(svg, "circle", { cx: o.x, cy: o.y, r: o.r }); });
    const labels = objects.map(function (o) { return lab.el(svg, "text", { x: o.x, y: o.y + 4, class: "mid small", text: String(o.id) }); });
    const cells = [];
    for (let i = 0; i < PIXELS; i++)
        cells.push(lab.el(svg, "rect", { x: CAMERA.x + PLANE - 4, y: edge(i), width: 8, height: edge(i + 1) - edge(i), class: "box" }));
    lab.el(svg, "circle", { cx: CAMERA.x, cy: CAMERA.y, r: 7, class: "solid" });
    lab.el(svg, "text", { x: CAMERA.x - 8, y: CAMERA.y + 26, class: "mid small", text: "camera" });
    lab.el(svg, "text", { x: CAMERA.x + PLANE, y: edge(0) - 8, class: "mid small", text: "the image: 32 pixels" });

    // The nearest object a ray from the camera through (CAMERA.x + PLANE, y) meets, and how far along it is.
    function trace(y) {
        const dx = PLANE;
        const dy = y - CAMERA.y;
        const length = Math.hypot(dx, dy);
        const ux = dx / length;
        const uy = dy / length;
        let best = null;
        objects.forEach(function (o) {
            const ox = o.x - CAMERA.x;
            const oy = o.y - CAMERA.y;
            const along = ox * ux + oy * uy;
            const off2 = ox * ox + oy * oy - along * along;
            if (off2 > o.r * o.r) return;
            const t = along - Math.sqrt(o.r * o.r - off2);
            if (t > 0 && (best === null || t < best.t)) best = { o: o, t: t };
        });
        return { ux: ux, uy: uy, best: best };
    }

    lab.draw(function () {
        const first = Math.min(start.value, PIXELS - 1);
        const count = Math.min(width.value, PIXELS - first);
        const scale = FAR / PLANE;
        const top = edge(first);
        const bottom = edge(first + count);
        function point(y, s) { return `${(CAMERA.x + PLANE * s).toFixed(1)},${(CAMERA.y + (y - CAMERA.y) * s).toFixed(1)}`; }
        lab.set(full, { points: [point(edge(0), 1), point(edge(0), scale), point(edge(PIXELS), scale), point(edge(PIXELS), 1)].join(" ") });
        lab.set(narrow, { points: [`${CAMERA.x},${CAMERA.y}`, point(top, scale), point(bottom, scale)].join(" ") });

        // The rect's two side planes, through the camera and the rect's edges: a sphere is kept unless it is more
        // than its radius outside one of them.
        function side(y) {
            const dx = PLANE;
            const dy = y - CAMERA.y;
            const length = Math.hypot(dx, dy);
            return [-dy / length, dx / length];
        }
        const topNormal = side(top);
        const bottomNormal = side(bottom);
        let kept = 0;
        objects.forEach(function (o, k) {
            const ox = o.x - CAMERA.x;
            const oy = o.y - CAMERA.y;
            const below = ox * topNormal[0] + oy * topNormal[1];
            const above = -(ox * bottomNormal[0] + oy * bottomNormal[1]);
            const inside = below >= -o.r && above >= -o.r;
            if (inside) kept += 1;
            lab.set(circles[k], { class: inside ? "gpu" : "ghost" });
            lab.set(labels[k], { class: inside ? "mid small" : "mid small t-faint" });
        });

        lab.clear(rays);
        let centreHit = null;
        const centre = first + Math.floor(count / 2);
        for (let i = 0; i < PIXELS; i++) lab.set(cells[i], { class: i >= first && i < first + count ? "mem" : "box" });
        for (let i = first; i < first + count; i++) {
            const y = 0.5 * (edge(i) + edge(i + 1));
            const ray = trace(y);
            const t = ray.best ? ray.best.t : FAR;
            lab.el(rays, "line", { x1: CAMERA.x, y1: CAMERA.y, x2: CAMERA.x + ray.ux * t, y2: CAMERA.y + ray.uy * t, class: i === centre ? "arrow hot" : "arrow o-50" });
            if (i === centre) centreHit = ray.best;
        }

        keptOut.set(`${kept} / ${objects.length}`, kept < objects.length / 2 ? "ok" : "bad");
        hitOut.set(centreHit ? `object ${centreHit.o.id}` : "nothing", centreHit ? "gpu" : "");
        if (count >= PIXELS)
            note.set("The rect is the whole image: the pick's frustum is the view's, and it draws every object in view, as the frame did.", "bad");
        else if (kept === 0)
            note.set("No object touches the narrow frustum: the pick draws nothing, and every pixel shows the background.", "ok");
        else
            note.set(`Only ${kept} of ${objects.length} objects touch the frustum through ${count} pixel${count === 1 ? "" : "s"}: the pick draws those, and the depth test keeps the nearest under each pixel.`, "ok");
    });
});
