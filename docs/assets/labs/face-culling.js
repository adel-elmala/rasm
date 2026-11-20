// Labs of docs/guides/face-culling.html.

// A number with a real minus sign, and no "−0.00".
function faceCullingNum(v, digits) {
    const s = Math.abs(v).toFixed(digits);
    return (v < 0 && Number(s) !== 0 ? "−" : "") + s;
}

// The shoelace formula of the page, y pointing up: positive is counter-clockwise.
function faceCullingArea(p) {
    return 0.5 * ((p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]));
}

// Three boxes in a row: winding -> face -> result. Returns a function that fills them.
function faceCullingChain(lab, svg, y) {
    const boxes = [{ x: 380, w: 150, label: "winding on screen" }, { x: 556, w: 84, label: "face" }, { x: 666, w: 84, label: "result" }];
    const parts = boxes.map(function (b) {
        lab.el(svg, "text", { x: b.x + b.w / 2, y: y - 8, class: "mid small", text: b.label });
        const rect = lab.el(svg, "rect", { x: b.x, y: y, width: b.w, height: 34, rx: 6, class: "box" });
        const text = lab.el(svg, "text", { x: b.x + b.w / 2, y: y + 22, class: "mid" });
        return { rect: rect, text: text };
    });
    lab.el(svg, "line", { x1: 532, y1: y + 17, x2: 553, y2: y + 17, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "line", { x1: 642, y1: y + 17, x2: 663, y2: y + 17, class: "arrow", "marker-end": "url(#ah)" });
    return function (winding, face, result, drawn) {
        lab.set(parts[0].text, { text: winding });
        lab.set(parts[1].text, { text: face });
        lab.set(parts[1].rect, { class: face.indexOf("front") >= 0 ? "ok" : "box" });
        lab.set(parts[2].text, { text: result });
        lab.set(parts[2].rect, { class: drawn ? "ok" : "bad" });
    };
}

// The arc with an arrowhead that shows the winding, around a center.
function faceCullingArc(cx, cy, ccw) {
    const x0 = (cx - 7).toFixed(1);
    const x1 = (cx + 7).toFixed(1);
    const y = (cy - 12).toFixed(1);
    return ccw ? `M${x0},${y} A14,14 0 1,0 ${x1},${y}` : `M${x1},${y} A14,14 0 1,1 ${x0},${y}`;
}

// A triangle with three corners to drag. The signed area gives the winding; the two settings give the result.
RasmLab.register("winding-test", function (lab) {
    const svg = lab.svg(760, 330, "A triangle in a plane with y up. Its three corners can be dragged. The signed area decides the winding, the front-face setting decides front or back, and the cull mode decides drawn or culled.");
    const front = lab.choice({ label: "Front face", options: ["counter-clockwise", "clockwise"] });
    const cull = lab.choice({ label: "Cull", options: ["none", "back", "front"], value: "back" });
    const areaOut = lab.readout("signed area a");
    const faceOut = lab.readout("the face, by the front-face setting");
    const resultOut = lab.readout("the result, by the cull mode");
    const note = lab.note();

    const CX = 180;
    const CY = 170;
    const SCALE = 150;

    lab.el(svg, "rect", { x: 30, y: 20, width: 300, height: 300, class: "paper" });
    lab.el(svg, "path", { d: "M105,20V320M255,20V320M30,95H330M30,245H330", class: "grid" });
    lab.el(svg, "line", { x1: 30, y1: CY, x2: 326, y2: CY, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "line", { x1: CX, y1: 320, x2: CX, y2: 24, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: 324, y: CY + 16, class: "end small", text: "+x" });
    lab.el(svg, "text", { x: CX + 8, y: 34, class: "small", text: "+y" });
    lab.el(svg, "text", { x: 34, y: 314, class: "small t-faint", text: "(−1, −1)" });
    const triangle = lab.el(svg, "polygon", { class: "ok" });
    const arc = lab.el(svg, "path", { class: "arrow hot", "marker-end": "url(#ah-hot)" });
    const names = [0, 1, 2].map(function (k) { return lab.el(svg, "text", { class: "mid mono halo", text: String(k) }); });
    const limits = { minX: 40, maxX: 320, minY: 30, maxY: 310 };
    const handles = [
        lab.handle(svg, Object.assign({ x: 180, y: 95, r: 7, label: "Corner 0" }, limits)),
        lab.handle(svg, Object.assign({ x: 105, y: 245, r: 7, label: "Corner 1" }, limits)),
        lab.handle(svg, Object.assign({ x: 255, y: 245, r: 7, label: "Corner 2" }, limits)),
    ];

    const rows = [0, 1, 2].map(function (k) { return lab.el(svg, "text", { x: 380, y: 40 + k * 20, class: "mono" }); });
    lab.el(svg, "text", { x: 380, y: 116, class: "mono", text: "a = ½ · ((x1 − x0)·(y2 − y0) − (x2 − x0)·(y1 − y0))" });
    const filled = lab.el(svg, "text", { x: 380, y: 138, class: "mono" });
    const value = lab.el(svg, "text", { x: 380, y: 160, class: "mono" });
    const chain = faceCullingChain(lab, svg, 216);
    lab.el(svg, "text", { x: 380, y: 290, class: "small", text: "Drag a corner across the opposite edge: the order 0, 1, 2" });
    lab.el(svg, "text", { x: 380, y: 306, class: "small", text: "stays the same, but its direction on screen reverses." });

    lab.draw(function () {
        const p = handles.map(function (h) { return [(h.x - CX) / SCALE, (CY - h.y) / SCALE]; });
        const a = faceCullingArea(p);
        const flat = Math.abs(a) < 0.005;
        const ccw = a > 0;
        const isFront = ccw === (front.value === "counter-clockwise");
        const drawn = !flat && cull.value !== (isFront ? "front" : "back");

        const mx = (handles[0].x + handles[1].x + handles[2].x) / 3;
        const my = (handles[0].y + handles[1].y + handles[2].y) / 3;
        lab.set(triangle, {
            points: handles.map(function (h) { return `${h.x.toFixed(1)},${h.y.toFixed(1)}`; }).join(" "),
            class: !drawn ? "ghost" : (isFront ? "ok" : "box"),
        });
        lab.set(arc, { d: faceCullingArc(mx, my, ccw), class: Math.abs(a) < 0.1 ? "hide" : "arrow hot" });
        handles.forEach(function (h, k) {
            const dx = h.x - mx;
            const dy = h.y - my;
            const len = Math.max(1, Math.hypot(dx, dy));
            lab.set(names[k], { x: (h.x + 17 * dx / len).toFixed(1), y: (h.y + 17 * dy / len + 4).toFixed(1) });
            lab.set(rows[k], { text: `p${k} = (${faceCullingNum(p[k][0], 2)}, ${faceCullingNum(p[k][1], 2)})` });
        });
        const e = [p[1][0] - p[0][0], p[2][1] - p[0][1], p[2][0] - p[0][0], p[1][1] - p[0][1]].map(function (v) { return faceCullingNum(v, 2); });
        lab.set(filled, { text: `  = ½ · ((${e[0]})·(${e[1]}) − (${e[2]})·(${e[3]}))` });
        lab.set(value, { text: `  = ${a > 0 && !flat ? "+" : ""}${faceCullingNum(a, 2)}` });

        const winding = flat ? "a line" : (ccw ? "counter-clockwise" : "clockwise");
        const face = flat ? "none" : (isFront ? "front" : "back");
        chain(winding, face, drawn ? "drawn" : (flat ? "not drawn" : "culled"), drawn);
        areaOut.set(`${a > 0 && !flat ? "+" : ""}${faceCullingNum(a, 2)}`, "");
        faceOut.set(face, isFront && !flat ? "ok" : "");
        resultOut.set(drawn ? "drawn" : (flat ? "not drawn" : "culled"), drawn ? "ok" : "bad");

        if (flat) note.set("The three corners are on one line: the area is 0, and a triangle with no area is never drawn.", "bad");
        else if (drawn && cull.value === "none") note.set(`Drawn: the cull mode "none" keeps both faces. This one is a ${face} face, because it is ${winding}.`, "ok");
        else if (drawn) note.set(`Drawn: a is ${ccw ? "positive" : "negative"}, so the corners go ${winding}. That is a ${face} face here, and only ${cull.value} faces are culled.`, "ok");
        else note.set(`Culled: a is ${ccw ? "positive" : "negative"}, so the corners go ${winding}. That is a ${face} face here, and ${cull.value} faces are dropped before any pixel is shaded.`, "bad");
    });
});

// A triangle under a scale. A negative determinant mirrors it, and the cull mode must follow.
RasmLab.register("mirror-cull", function (lab) {
    const svg = lab.svg(760, 330, "A triangle as modeled, and the same triangle after a scale along x and y. When the determinant of the scale is negative the triangle is mirrored and its corners go clockwise on screen.");
    const format = function (v) { return faceCullingNum(v, 1); };
    const scaleX = lab.slider({ label: "scale.x", min: -1.5, max: 1.5, step: 0.1, value: 1, format: format });
    const scaleY = lab.slider({ label: "scale.y", min: -1.5, max: 1.5, step: 0.1, value: 1, format: format });
    const mode = lab.choice({ label: "Cull mode", options: [{ value: "back", label: "always back" }, { value: "det", label: "from the determinant" }] });
    const detOut = lab.readout("determinant: scale.x · scale.y · scale.z");
    const cullOut = lab.readout("cull mode used for this object");
    const resultOut = lab.readout("the outside face, toward the camera");
    const note = lab.note();

    const CX = 190;
    const CY = 170;
    const SCALE = 120;
    const MODEL = [[0.2, 0.7], [0.2, 0.1], [0.8, 0.1]];
    function sx(x) { return CX + x * SCALE; }
    function sy(y) { return CY - y * SCALE; }
    function points(p) { return p.map(function (q) { return `${sx(q[0]).toFixed(1)},${sy(q[1]).toFixed(1)}`; }).join(" "); }

    lab.el(svg, "rect", { x: 30, y: 20, width: 320, height: 300, class: "paper" });
    lab.el(svg, "path", { d: `M${CX},20V320M30,${CY}H350`, class: "grid" });
    lab.el(svg, "text", { x: 344, y: CY - 6, class: "end small", text: "+x" });
    lab.el(svg, "text", { x: CX + 6, y: 34, class: "small", text: "+y" });
    lab.el(svg, "polygon", { points: points(MODEL), class: "ghost" });
    const triangle = lab.el(svg, "polygon", { class: "ok" });
    const arc = lab.el(svg, "path", { class: "arrow hot", "marker-end": "url(#ah-hot)" });
    const names = [0, 1, 2].map(function (k) { return lab.el(svg, "text", { class: "mid mono halo", text: String(k) }); });
    lab.el(svg, "text", { x: 40, y: 312, class: "small t-faint", text: "dashed: as modeled" });

    const scaleText = lab.el(svg, "text", { x: 380, y: 40, class: "mono" });
    const detText = lab.el(svg, "text", { x: 380, y: 62, class: "mono" });
    const areaText = lab.el(svg, "text", { x: 380, y: 84, class: "mono" });
    lab.el(svg, "text", { x: 380, y: 124, class: "small", text: "Front face: counter-clockwise. The side toward the camera is" });
    lab.el(svg, "text", { x: 380, y: 140, class: "small", text: "the outside of the object: it should be drawn." });
    const chain = faceCullingChain(lab, svg, 196);
    const ruleText = lab.el(svg, "text", { x: 380, y: 270, class: "small" });

    lab.draw(function () {
        const s = [scaleX.value, scaleY.value];
        const det = s[0] * s[1];
        const p = MODEL.map(function (q) { return [q[0] * s[0], q[1] * s[1]]; });
        const a = faceCullingArea(p);
        const flat = Math.abs(det) < 1e-6;
        const ccw = a > 0;
        // the rule of the page: det >= 0 culls back faces, det < 0 culls front faces
        const culls = mode.value === "det" && det < 0 ? "front" : "back";
        const face = ccw ? "front" : "back";
        const drawn = !flat && face !== culls;

        lab.set(triangle, { points: points(p), class: flat ? "hide" : (drawn ? "ok" : "bad") });
        const mx = (sx(p[0][0]) + sx(p[1][0]) + sx(p[2][0])) / 3;
        const my = (sy(p[0][1]) + sy(p[1][1]) + sy(p[2][1])) / 3;
        lab.set(arc, { d: faceCullingArc(mx, my, ccw), class: Math.abs(a) < 0.06 ? "hide" : "arrow hot" });
        p.forEach(function (q, k) {
            const dx = sx(q[0]) - mx;
            const dy = sy(q[1]) - my;
            const len = Math.max(1, Math.hypot(dx, dy));
            lab.set(names[k], { x: (sx(q[0]) + 12 * dx / len).toFixed(1), y: (sy(q[1]) + 12 * dy / len + 4).toFixed(1), class: flat ? "hide" : "mid mono halo" });
        });

        lab.set(scaleText, { text: `scale = (${faceCullingNum(s[0], 1)}, ${faceCullingNum(s[1], 1)}, 1)` });
        lab.set(detText, { text: `det = ${faceCullingNum(s[0], 1)} · ${faceCullingNum(s[1], 1)} · 1 = ${faceCullingNum(det, 2)}` });
        lab.set(areaText, { text: `a = ${faceCullingNum(a, 3)}   (as modeled: ${faceCullingNum(faceCullingArea(MODEL), 3)})` });
        lab.set(ruleText, { text: mode.value === "det" ? "det ≥ 0 → cull back.   det < 0 → cull front." : "Every object culls back faces, mirrored or not." });
        chain(flat ? "a line" : (ccw ? "counter-clockwise" : "clockwise"), flat ? "none" : `"${face}"`, drawn ? "drawn" : (flat ? "not drawn" : "culled"), drawn);

        detOut.set(faceCullingNum(det, 2), "");
        cullOut.set(culls);
        resultOut.set(drawn ? "drawn" : (flat ? "not drawn" : "culled"), drawn ? "ok" : "bad");
        if (flat) note.set("A scale of 0 makes the triangle a line: the determinant and the area are 0, and nothing is drawn.", "bad");
        else if (det > 0 && s[0] < 0) note.set("Two mirrors cancel: the determinant is positive, the triangle is turned by 180° and is counter-clockwise again. Back culling is right.", "ok");
        else if (det > 0) note.set("No mirror: the determinant is positive and the corners still go counter-clockwise. Back culling keeps the outside.", "ok");
        else if (drawn) note.set("Mirrored: the corners go clockwise, so the rasterizer calls the outside a back face. The determinant is negative, so this object culls front faces, and the outside is drawn.", "ok");
        else note.set("Mirrored: the corners go clockwise, so the rasterizer calls the outside a back face, and back culling removes it. The object looks inside out.", "bad");
    });
});
