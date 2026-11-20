// Labs of docs/guides/coordinate-spaces.html.

// A number with a real minus sign, and no "−0.00".
function coordinateSpacesNum(v, digits) {
    const s = Math.abs(v).toFixed(digits);
    return (v < 0 && Number(s) !== 0 ? "−" : "") + s;
}

// Top views of one fixed world point: in world space with a camera to move, and in that camera's view space.
// The numbers follow the page's worked example: focal 1, aspect 2, near 1, an image 800 pixels wide.
RasmLab.register("world-to-view", function (lab) {
    const svg = lab.svg(760, 320, "Left: a top view of world space with a fixed point, and a camera that can be moved and turned. Right: the same point in the camera's view space, where the camera is at the origin and looks up the page, and the place of the point in the image.");
    const viewOut = lab.readout("the point in view space");
    const clipOut = lab.readout("clip space: x and w");
    const pixelOut = lab.readout("NDC x → pixel x, image 800 wide");
    const note = lab.note();

    const SCALE = 22;
    const WORLD = { x: 150, y: 210 };       // the world origin, in the left panel
    const VIEW = { x: 570, y: 226 };        // the camera, in the right panel
    const POINT = [0, -3];                  // world (x, z); y is 0
    const NEAR = 1;
    const ASPECT = 2;
    const FOCAL = 1;
    const WIDTH = 800;

    const defs = lab.el(svg, "defs");
    lab.el(lab.el(defs, "clipPath", { id: "lab-world-clip" }), "rect", { x: 11, y: 11, width: 358, height: 298 });
    lab.el(lab.el(defs, "clipPath", { id: "lab-view-clip" }), "rect", { x: 391, y: 11, width: 358, height: 244 });

    // world space
    lab.el(svg, "rect", { x: 10, y: 10, width: 360, height: 300, rx: 8, class: "zone" });
    const worldFrustum = lab.el(lab.el(svg, "g", { "clip-path": "url(#lab-world-clip)" }), "polygon", { class: "cpu o-50" });
    lab.el(svg, "text", { x: 24, y: 34, class: "title", text: "World space (from above)" });
    lab.el(svg, "line", { x1: 30, y1: WORLD.y, x2: 356, y2: WORLD.y, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: 352, y: WORLD.y + 18, class: "end small", text: "+X" });
    lab.el(svg, "line", { x1: WORLD.x, y1: 300, x2: WORLD.x, y2: 50, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: WORLD.x + 8, y: 60, class: "small", text: "−Z" });
    lab.el(svg, "circle", { cx: WORLD.x + POINT[0] * SCALE, cy: WORLD.y + POINT[1] * SCALE, r: 8, class: "gpu" });
    lab.el(svg, "text", { x: WORLD.x - 12, y: WORLD.y + POINT[1] * SCALE + 4, class: "end small halo", text: "point (0, 0, −3)" });
    const look = lab.el(svg, "line", { class: "arrow hot", "marker-end": "url(#ah-hot)" });
    const body = lab.el(svg, "circle", { r: 12, class: "cpu" });
    const eyeText = lab.el(svg, "text", { class: "mid small halo" });
    const aimText = lab.el(svg, "text", { class: "mid small halo", text: "looks at" });
    const aim = lab.handle(svg, { x: WORLD.x, y: WORLD.y, r: 7, label: "Where the camera looks", minX: 24, maxX: 356, minY: 50, maxY: 296 });
    const eye = lab.handle(svg, { x: WORLD.x + 5 * SCALE, y: WORLD.y, r: 7, label: "Move the camera", minX: 24, maxX: 356, minY: 50, maxY: 296 });

    lab.el(svg, "line", { x1: 372, y1: 150, x2: 386, y2: 150, class: "arrow", "marker-end": "url(#ah)" });

    // view space
    lab.el(svg, "rect", { x: 390, y: 10, width: 360, height: 300, rx: 8, class: "zone" });
    const half = (744 - VIEW.x) * FOCAL / ASPECT;       // how far ahead the frustum's edge meets the panel's side
    lab.el(svg, "polygon", { points: `${VIEW.x},${VIEW.y} 396,${VIEW.y - half} 396,44 744,44 744,${VIEW.y - half}`, class: "cpu o-50" });
    lab.el(svg, "text", { x: 404, y: 34, class: "title", text: "View space" });
    lab.el(svg, "line", { x1: 400, y1: VIEW.y, x2: 738, y2: VIEW.y, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: 736, y: VIEW.y + 16, class: "end small", text: "+X (camera right)" });
    lab.el(svg, "line", { x1: VIEW.x, y1: 252, x2: VIEW.x, y2: 50, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: VIEW.x + 8, y: 60, class: "small", text: "−Z (forward)" });
    lab.el(svg, "circle", { cx: VIEW.x, cy: VIEW.y, r: 7, class: "cpu" });
    lab.el(svg, "text", { x: VIEW.x - 12, y: VIEW.y + 16, class: "end small", text: "camera (0, 0, 0)" });
    const viewGroup = lab.el(svg, "g", { "clip-path": "url(#lab-view-clip)" });
    const across = lab.el(viewGroup, "line", { class: "arrow dashed" });
    const ahead = lab.el(viewGroup, "line", { class: "arrow dashed" });
    const viewDot = lab.el(viewGroup, "circle", { r: 8, class: "gpu" });

    // the image, left to right
    lab.el(svg, "text", { x: 404, y: 274, class: "small", text: "in the image:" });
    lab.el(svg, "line", { x1: 490, y1: 270, x2: 730, y2: 270, class: "arrow thick" });
    lab.el(svg, "path", { d: "M490,263v14M610,265v10M730,263v14", class: "arrow" });
    lab.el(svg, "text", { x: 490, y: 296, class: "mid small", text: "pixel 0" });
    lab.el(svg, "text", { x: 610, y: 296, class: "mid small", text: "400" });
    lab.el(svg, "text", { x: 730, y: 296, class: "mid small", text: "800" });
    const pixelDot = lab.el(svg, "circle", { cy: 270, r: 6, class: "gpu" });

    let forward = [-1, 0];
    lab.draw(function () {
        const e = [(eye.x - WORLD.x) / SCALE, (eye.y - WORLD.y) / SCALE];
        const target = [(aim.x - WORLD.x) / SCALE, (aim.y - WORLD.y) / SCALE];
        const length = Math.hypot(target[0] - e[0], target[1] - e[1]);
        if (length > 0.2) forward = [(target[0] - e[0]) / length, (target[1] - e[1]) / length];
        // lookAt with up = +Y: r = f × up. In (x, z): r = (−f.z, f.x)
        const right = [-forward[1], forward[0]];
        const d = [POINT[0] - e[0], POINT[1] - e[1]];
        const xv = right[0] * d[0] + right[1] * d[1];
        const zv = -(forward[0] * d[0] + forward[1] * d[1]);
        const xc = FOCAL / ASPECT * xv;
        const wc = -zv;
        const ndc = xc / wc;
        const pixel = (ndc + 1) / 2 * WIDTH;

        lab.set(body, { cx: eye.x, cy: eye.y });
        lab.set(eyeText, { x: eye.x, y: eye.y + 28, text: `camera (${coordinateSpacesNum(e[0], 1)}, 0, ${coordinateSpacesNum(e[1], 1)})` });
        lab.set(aimText, { x: aim.x, y: aim.y - 12 });
        const reach = Math.max(0, length * SCALE - 12);
        lab.set(look, { x1: eye.x, y1: eye.y, x2: eye.x + forward[0] * reach, y2: eye.y + forward[1] * reach, class: reach > 20 ? "arrow hot" : "hide" });
        const far = 40 * SCALE;
        const side = ASPECT / FOCAL;
        lab.set(worldFrustum, {
            points: `${eye.x},${eye.y} ` +
                    `${(eye.x + far * (forward[0] - side * right[0])).toFixed(1)},${(eye.y + far * (forward[1] - side * right[1])).toFixed(1)} ` +
                    `${(eye.x + far * (forward[0] + side * right[0])).toFixed(1)},${(eye.y + far * (forward[1] + side * right[1])).toFixed(1)}`,
        });

        const vx = VIEW.x + xv * SCALE;
        const vy = VIEW.y + zv * SCALE;
        lab.set(viewDot, { cx: vx, cy: vy });
        lab.set(across, { x1: VIEW.x, y1: vy, x2: vx, y2: vy });
        lab.set(ahead, { x1: vx, y1: VIEW.y, x2: vx, y2: vy });

        const inFront = wc >= NEAR;
        const inside = inFront && Math.abs(xc) <= wc;
        lab.set(pixelDot, { cx: 490 + 240 * lab.clamp((ndc + 1) / 2, 0, 1), class: inside ? "gpu" : "hide" });
        viewOut.set(`(${coordinateSpacesNum(xv, 2)}, 0, ${coordinateSpacesNum(zv, 2)})`, "gpu");
        clipOut.set(`${coordinateSpacesNum(xc, 2)}, ${coordinateSpacesNum(wc, 2)}`);
        pixelOut.set(inFront ? `${coordinateSpacesNum(ndc, 2)} → ${coordinateSpacesNum(pixel, 0)}` : "none", inside ? "ok" : "bad");

        const where = `${coordinateSpacesNum(wc, 1)} units ahead and ${coordinateSpacesNum(Math.abs(xv), 1)} to the ${xv >= 0 ? "right" : "left"}`;
        if (inside) note.set(`The camera sees the point: it is ${where}. The world position did not change, only the numbers in view space did.`, "ok");
        else if (wc <= 0) note.set("The point is behind the camera: its z in view space is positive, so w is negative, and it is clipped.", "bad");
        else if (!inFront) note.set(`The point is ${coordinateSpacesNum(wc, 2)} units ahead, closer than the near plane at ${NEAR}: it is clipped.`, "bad");
        else note.set(`The point is ${where}: outside the ${xv >= 0 ? "right" : "left"} edge of the image, because x in clip space is ${xv >= 0 ? "larger than w" : "smaller than −w"}. It is clipped.`, "bad");
    });
});
