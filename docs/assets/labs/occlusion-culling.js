// Labs of docs/guides/occlusion-culling.html.

// One row of 16 depth texels (reversed-Z: 1 = near, 0 = far) and its coarser levels. Each coarser texel is
// the farthest (min) or, as the bug, the nearest (max) of the two it covers.
RasmLab.register("hiz-pyramid", function (lab) {
    const svg = lab.svg(760, 300, "A row of 16 depth texels and four coarser levels below it. Each coarser texel holds the farthest of the two texels it covers.");
    const keep = lab.choice({ label: "A texel keeps", options: [{ value: "far", label: "the farthest (min)" }, { value: "near", label: "the nearest (max)" }], value: "far" });
    const topOut = lab.readout("the top level's value");
    const reachOut = lab.readout("levels that still hold the picked texel's value");
    const note = lab.note();

    const N = 16;
    const X0 = 68;
    const PITCH = 42;
    const ROW = 54;
    const tenths = [9, 8, 7, 6, 3, 3, 3, 2, 5, 5, 4, 5, 1, 0, 0, 0];      // depth × 10, so sums stay exact
    let picked = 7;

    function show(v) { return v === 10 ? "1" : v === 0 ? "0" : `0.${v}`; }

    const levels = [];
    for (let level = 0; level <= 4; level++) {
        const y = 22 + level * ROW;
        const count = N >> level;
        const size = 1 << level;
        lab.el(svg, "text", { x: 8, y: y + 20, class: "small", text: `level ${level}` });
        const row = [];
        for (let k = 0; k < count; k++) {
            const x = X0 + k * size * PITCH;
            const rect = lab.el(svg, "rect", { x: x, y: y, width: size * PITCH - 4, height: 30, rx: 3 });
            const text = lab.el(svg, "text", { x: x + (size * PITCH - 4) / 2, y: y + 20, class: "mid mono", "pointer-events": "none" });
            if (level === 0) {
                lab.clickable(rect, `Texel ${k}: move it nearer`, function () {
                    picked = k;
                    tenths[k] = tenths[k] === 10 ? 0 : tenths[k] + 1;
                });
            }
            row.push({ rect: rect, text: text });
        }
        levels.push(row);
    }
    const frame = lab.el(svg, "rect", { y: 22, width: PITCH - 4, height: 30, rx: 3, class: "hot", "pointer-events": "none" });
    lab.el(svg, "text", { x: X0, y: 14, class: "small", text: "the depth, one row of it: click a texel to move it 0.1 nearer (after 1 it goes back to 0, far)" });
    lab.el(svg, "text", { x: X0, y: 290, class: "small", text: "1 = near, 0 = far. The orange frame is the texel you picked; filled orange texels hold its value." });

    lab.draw(function () {
        const far = keep.value === "far";
        const values = [tenths.slice()];
        for (let level = 1; level <= 4; level++) {
            const below = values[level - 1];
            const row = [];
            for (let k = 0; k < below.length / 2; k++) {
                row.push(far ? Math.min(below[2 * k], below[2 * k + 1]) : Math.max(below[2 * k], below[2 * k + 1]));
            }
            values.push(row);
        }

        // how far up the picked texel's own value travels
        let reach = 0;
        while (reach < 4 && values[reach + 1][picked >> (reach + 1)] === tenths[picked]) reach += 1;

        for (let level = 0; level <= 4; level++) {
            levels[level].forEach(function (cell, k) {
                const mine = level > 0 && level <= reach && k === picked >> level;
                lab.set(cell.rect, { class: (mine ? "accent" : "mem") + (level === 0 ? " pick" : "") });
                lab.set(cell.text, { text: show(values[level][k]) });
            });
        }
        lab.set(frame, { x: X0 + picked * PITCH });

        topOut.set(show(values[4][0]), far ? "ok" : "bad");
        reachOut.set(`${reach} of 4`);
        const value = show(tenths[picked]);
        if (!far) note.set(`Wrong for culling: the top now holds ${show(values[4][0])}, the nearest depth of the row. An object behind that one texel would count as hidden everywhere.`, "bad");
        else if (reach === 4) note.set(`The picked texel (${value}) is the farthest of the whole row, so its value goes up to the top level.`, "ok");
        else if (reach === 0) note.set(`The picked texel (${value}) has a farther neighbour, so its value is already gone at level 1. A near texel cannot hide a far one.`, "ok");
        else note.set(`The picked texel (${value}) is the farthest of the ${1 << reach} texels around it, so it reaches level ${reach}. At level ${reach + 1} a farther texel replaces it.`, "ok");
    });
});

// A top view: a camera, a wall, a thin pole, a far wall and a round object. The view is drawn into a depth row
// of 32 pixels, the row gets a pyramid, and the object is tested like in sections 4 to 6: its pixel range, the
// finest level where the range touches at most 2 texels, the farthest of them against the nearest depth.
RasmLab.register("hiz-test", function (lab) {
    const svg = lab.svg(760, 340, "A top view of a camera, a wall, a thin pole, a far wall and a round object. The stored depths of one pyramid level are drawn as steps, and the object is colored by the result of the occlusion test.");
    const radius = lab.slider({ label: "Object radius", min: 8, max: 60, step: 2, value: 22 });
    const store = lab.choice({ label: "A texel stores", options: [{ value: "far", label: "the farthest" }, { value: "near", label: "the nearest" }], value: "far" });
    const coverOut = lab.readout("pixels the object can cover, of 32");
    const levelOut = lab.readout("level that is read (pixels per texel)");
    const compareOut = lab.readout("nearest depth against f");
    const note = lab.note();

    const CAMERA = { x: 40, y: 170 };
    const SPREAD = 0.235;       // half of the view: sideways distance per unit of depth
    const PIXELS = 32;
    const FAR = 680;
    const NEAR = 30;
    const WALLS = [{ depth: 290, y0: 60, y1: 160 }, { depth: 160, y0: 182, y1: 186 }];

    function slope(u) { return (u / PIXELS * 2 - 1) * SPREAD; }         // pixel coordinate to sideways slope
    function pixelOf(t) { return (t / SPREAD * 0.5 + 0.5) * PIXELS; }

    // level 0: the depth of the first thing each pixel's center ray hits (the object is not drawn yet)
    const depth0 = [];
    for (let p = 0; p < PIXELS; p++) {
        const t = slope(p + 0.5);
        let d = FAR;
        WALLS.forEach(function (wall) {
            const y = CAMERA.y + t * wall.depth;
            if (y >= wall.y0 && y <= wall.y1 && wall.depth < d) d = wall.depth;
        });
        depth0.push(d);
    }

    lab.el(svg, "line", { x1: CAMERA.x, y1: CAMERA.y, x2: CAMERA.x + FAR, y2: CAMERA.y - SPREAD * FAR, class: "dashed" });
    lab.el(svg, "line", { x1: CAMERA.x, y1: CAMERA.y, x2: CAMERA.x + FAR, y2: CAMERA.y + SPREAD * FAR, class: "dashed" });
    lab.el(svg, "line", { x1: CAMERA.x + FAR, y1: 8, x2: CAMERA.x + FAR, y2: 332, class: "surface" });
    WALLS.forEach(function (wall) {
        lab.el(svg, "rect", { x: CAMERA.x + wall.depth, y: wall.y0, width: 7, height: wall.y1 - wall.y0, class: "solid" });
    });
    lab.el(svg, "text", { x: CAMERA.x + 304, y: 54, class: "small", text: "wall" });
    lab.el(svg, "text", { x: CAMERA.x + 164, y: 204, class: "mid small", text: "pole" });
    lab.el(svg, "text", { x: 704, y: 176, class: "end small halo", text: "far wall" });
    lab.el(svg, "circle", { cx: CAMERA.x, cy: CAMERA.y, r: 7, class: "cpu" });
    lab.el(svg, "text", { x: CAMERA.x, y: CAMERA.y + 24, class: "mid small", text: "camera" });
    const wedgeA = lab.el(svg, "line", { class: "arrow dashed" });
    const wedgeB = lab.el(svg, "line", { class: "arrow dashed" });
    const steps = lab.el(svg, "g");
    const body = lab.el(svg, "circle", {});
    const nearestTick = lab.el(svg, "line", { class: "arrow gpu thick" });
    const fLabel = lab.el(svg, "text", { class: "end small t-hot halo", text: "f" });
    const object = lab.handle(svg, { x: 500, y: 116, label: "Move the object", minX: 80, maxX: 690, minY: 20, maxY: 320 });

    lab.draw(function () {
        const far = store.value === "far";
        const r = radius.value;
        const dx = object.x - CAMERA.x;
        const dy = object.y - CAMERA.y;
        const nearest = dx - r;

        // the pyramid of the depth row
        const pyramid = [depth0];
        for (let level = 1; (PIXELS >> level) >= 1; level++) {
            const below = pyramid[level - 1];
            const row = [];
            for (let k = 0; k < below.length / 2; k++) {
                row.push(far ? Math.max(below[2 * k], below[2 * k + 1]) : Math.min(below[2 * k], below[2 * k + 1]));
            }
            pyramid.push(row);
        }

        lab.set(body, { cx: object.x, cy: object.y, r: r });
        lab.set(nearestTick, { x1: object.x - r, y1: object.y - r - 5, x2: object.x - r, y2: object.y + r + 5 });

        // the object's range of pixels: the two lines from the camera that touch the circle
        let state = "near";
        let pMin = 0;
        let pMax = PIXELS - 1;
        let level = 0;
        let f = 0;
        let fTexel = -1;
        if (nearest > NEAR) {
            const angle = Math.atan2(dy, dx);
            const half = Math.asin(r / Math.sqrt(dx * dx + dy * dy));
            const uMin = pixelOf(Math.tan(angle - half));
            const uMax = pixelOf(Math.tan(angle + half));
            if (uMax < 0 || uMin >= PIXELS) state = "outside";
            else {
                pMin = lab.clamp(Math.floor(uMin), 0, PIXELS - 1);
                pMax = lab.clamp(Math.floor(uMax), 0, PIXELS - 1);
                // the finest level where the range touches at most 2 texels
                while ((pMax >> level) - (pMin >> level) > 1) level += 1;
                for (let k = pMin >> level; k <= pMax >> level; k++) {
                    if (fTexel < 0 || pyramid[level][k] > f) {
                        f = pyramid[level][k];
                        fTexel = k;
                    }
                }
                const occluded = nearest > f;
                // the truth: does any pixel's center ray reach the object before a wall?
                let seen = false;
                for (let p = pMin; p <= pMax; p++) {
                    const t = slope(p + 0.5);
                    const a = 1 + t * t;
                    const b = dx + t * dy;
                    const disc = b * b - a * (dx * dx + dy * dy - r * r);
                    if (disc >= 0 && (b - Math.sqrt(disc)) / a < depth0[p]) seen = true;
                }
                state = occluded ? (seen ? "wrong" : "culled") : (seen ? "seen" : "kept");
            }
        }

        // the stored depths of the level that is read, as steps in the view
        lab.clear(steps);
        const tested = state !== "near" && state !== "outside";
        const size = 1 << level;
        pyramid[level].forEach(function (d, k) {
            const read = tested && k >= pMin >> level && k <= pMax >> level;
            const x = CAMERA.x + d - 5;
            const y0 = CAMERA.y + slope(k * size) * d + 1;
            const y1 = CAMERA.y + slope((k + 1) * size) * d - 1;
            // a texel the test reads is a filled bar, the others are plain lines
            if (read) lab.el(steps, "rect", { x: x - 3, y: y0.toFixed(1), width: 6, height: (y1 - y0).toFixed(1), class: "accent" });
            else lab.el(steps, "line", { x1: x, y1: y0.toFixed(1), x2: x, y2: y1.toFixed(1), class: "arrow mem thick" });
        });
        if (tested) {
            const mid = CAMERA.y + slope((fTexel + 0.5) * size) * f;
            lab.set(fLabel, { x: CAMERA.x + f - 13, y: lab.clamp(mid + 4, 16, 320), class: "end small t-hot halo" });
            lab.set(wedgeA, { x1: CAMERA.x, y1: CAMERA.y, x2: CAMERA.x + FAR, y2: CAMERA.y + slope(pMin) * FAR, class: "arrow dashed" });
            lab.set(wedgeB, { x1: CAMERA.x, y1: CAMERA.y, x2: CAMERA.x + FAR, y2: CAMERA.y + slope(pMax + 1) * FAR, class: "arrow dashed" });
        } else {
            lab.set(fLabel, { class: "hide" });
            lab.set(wedgeA, { class: "hide" });
            lab.set(wedgeB, { class: "hide" });
        }

        const BODY = { near: "ok", outside: "ghost", seen: "ok", kept: "box", culled: "ghost", wrong: "bad" };
        lab.set(body, { class: BODY[state] });
        if (!tested) {
            coverOut.set("–");
            levelOut.set("–");
            compareOut.set("no test");
        } else {
            coverOut.set(String(pMax - pMin + 1));
            levelOut.set(`${level} (${size})`);
            compareOut.set(`${nearest.toFixed(0)} ${nearest > f ? ">" : "≤"} ${f}`, state === "wrong" ? "bad" : state === "seen" || state === "culled" ? "ok" : "");
        }
        const count = tested ? (pMax >> level) - (pMin >> level) + 1 : 0;
        const texels = count === 1 ? "the one texel" : "the 2 texels";
        if (state === "near") note.set("The object reaches the near plane: the projection breaks down there, so the answer is \"visible\" with no test.");
        else if (state === "outside") note.set("The object is outside the view. The frustum test removes it before this test runs.");
        else if (state === "seen") note.set(`Visible, and kept: the object's nearest point is in front of f, the farthest depth in ${texels} it touches.`, "ok");
        else if (state === "culled") note.set(`Occluded, and culled: the object's nearest point is behind f, so it is behind everything drawn in ${texels} it touches.`, "ok");
        else if (state === "kept") note.set(`A false visible: the object is hidden, but ${texels} also cover something farther away, so f is behind the object. It is kept, which costs time only.`);
        else note.set("The bug: the texels hold the nearest depth, so one small thing in front counts for the whole texel. The object is culled, but it can be seen.", "bad");
    });
});
