// Labs of docs/guides/local-light-shadows.html.

// A point light's cube, seen from above in the slice through the light. The point P picks its face by the biggest
// part of d = P - light, like the GPU's cube map lookup, and the lab shows the depth along that face's axis, the face
// texel P lands on (Vulkan's cube map face table) and the size of one texel in the world at P.
RasmLab.register("cube-face", function (lab) {
    const svg = lab.svg(760, 410, "A view from above of a point light and its four side faces as 90 degree wedges. The draggable point P is in one face; the depth along that face's axis is drawn, and on the right the face's image shows where P lands.");
    const height = lab.slider({ label: "Height of P (y)", min: -5, max: 5, step: 0.1, value: 0.4, format: function (v) { return v.toFixed(1); } });
    const size = lab.choice({ label: "Map size", options: [{ value: 256, label: "256" }, { value: 512, label: "512" }, { value: 1024, label: "1024" }, { value: 2048, label: "2048" }], value: 1024 });
    const faceOut = lab.readout("the face that holds P");
    const depthOut = lab.readout("depth along its axis (|d|)");
    const texelOut = lab.readout("one texel at P, in world units");
    const note = lab.note();

    // Vulkan's order and table: the axis a face looks along, and where its texel coordinates s and t grow.
    const NAMES = ["+X", "−X", "+Y", "−Y", "+Z", "−Z"];
    const FORWARD = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]];
    const S = [[0, 0, -1], [0, 0, 1], [1, 0, 0], [1, 0, 0], [1, 0, 0], [-1, 0, 0]];
    const T = [[0, -1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1], [0, -1, 0], [0, -1, 0]];
    const AXES = ["x", "y", "z"];

    const CX = 210;
    const CY = 200;
    const K = 34;           // pixels per world unit
    const R = 190;          // half the drawn square, in pixels
    function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    function getCubeFace(d) {
        const a = d.map(Math.abs);
        if (a[0] >= a[1] && a[0] >= a[2]) return d[0] >= 0 ? 0 : 1;
        if (a[1] >= a[2]) return d[1] >= 0 ? 2 : 3;
        return d[2] >= 0 ? 4 : 5;
    }
    function axisWord(v, prefix) {
        const k = v.findIndex(function (c) { return c !== 0; });
        return (v[k] > 0 ? "+" : "−") + prefix + AXES[k];
    }

    // the four side faces, as wedges between the square's corners
    const corners = { tl: [CX - R, CY - R], tr: [CX + R, CY - R], bl: [CX - R, CY + R], br: [CX + R, CY + R] };
    const wedgeCorners = { 0: ["tr", "br"], 1: ["tl", "bl"], 4: ["bl", "br"], 5: ["tl", "tr"] };
    const wedges = {};
    for (const face in wedgeCorners) {
        const a = corners[wedgeCorners[face][0]];
        const b = corners[wedgeCorners[face][1]];
        wedges[face] = lab.el(svg, "polygon", { points: `${CX},${CY} ${a[0]},${a[1]} ${b[0]},${b[1]}`, class: "zone" });
    }
    const fan = lab.el(svg, "path", { class: "grid" });
    lab.el(svg, "line", { x1: CX - R, y1: CY - R, x2: CX + R, y2: CY + R, class: "dashed" });
    lab.el(svg, "line", { x1: CX + R, y1: CY - R, x2: CX - R, y2: CY + R, class: "dashed" });
    lab.el(svg, "rect", { x: CX - 30, y: CY - 30, width: 60, height: 60, class: "hot" });
    const ring = lab.el(svg, "circle", { cx: CX, cy: CY, r: 20, class: "hot" });
    const foot = lab.el(svg, "line", { class: "dashed" });
    const depthLine = lab.el(svg, "line", { class: "arrow hot thick" });
    const ray = lab.el(svg, "line", { class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: CX + R - 8, y: CY + 4, class: "end title", text: "+X" });
    lab.el(svg, "text", { x: CX - R + 8, y: CY + 4, class: "title", text: "−X" });
    lab.el(svg, "text", { x: CX, y: CY - R + 20, class: "mid title", text: "−Z" });
    lab.el(svg, "text", { x: CX, y: CY + R - 8, class: "mid title", text: "+Z" });
    lab.el(svg, "circle", { cx: CX, cy: CY, r: 8, class: "light" });
    const depthText = lab.el(svg, "text", { class: "mid small t-hot halo" });

    // the face's image: s to the right, t down
    const IX = 470;
    const IY = 56;
    const IS = 220;
    const title = lab.el(svg, "text", { x: 440, y: 24, class: "title" });
    lab.el(svg, "rect", { x: IX, y: IY, width: IS, height: IS, class: "mem" });
    let grid = "";
    for (let k = 1; k < 8; k++) grid += `M${IX + k * IS / 8},${IY}V${IY + IS}M${IX},${IY + k * IS / 8}H${IX + IS}`;
    lab.el(svg, "path", { d: grid, class: "grid" });
    lab.el(svg, "line", { x1: IX, y1: 44, x2: IX + IS + 10, y2: 44, class: "arrow hot", "marker-end": "url(#ah-hot)" });
    lab.el(svg, "text", { x: IX + IS + 16, y: 48, class: "small t-hot", text: "s" });
    lab.el(svg, "line", { x1: 458, y1: IY, x2: 458, y2: IY + IS + 10, class: "arrow hot", "marker-end": "url(#ah-hot)" });
    lab.el(svg, "text", { x: 446, y: IY + IS + 14, class: "small t-hot", text: "t" });
    const marker = lab.el(svg, "circle", { r: 7, class: "hot" });
    const line1 = lab.el(svg, "text", { x: 440, y: 304, class: "small" });
    const line2 = lab.el(svg, "text", { x: 440, y: 324, class: "small" });
    const line3 = lab.el(svg, "text", { x: 440, y: 344, class: "small" });
    lab.el(svg, "line", { x1: 700, y1: 366, x2: 736, y2: 366, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "line", { x1: 700, y1: 366, x2: 700, y2: 398, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: 742, y: 370, class: "small", text: "x" });
    lab.el(svg, "text", { x: 708, y: 402, class: "small", text: "z" });
    lab.el(svg, "text", { x: 690, y: 370, class: "end small t-faint", text: "seen from above" });

    const p = lab.handle(svg, { x: CX + 3 * K, y: CY - 1.4 * K, label: "Point P", minX: CX - R + 4, maxX: CX + R - 4, minY: CY - R + 4, maxY: CY + R - 4 });

    lab.draw(function () {
        const d = [(p.x - CX) / K, height.value, (p.y - CY) / K];
        const a = d.map(Math.abs);
        const len = Math.hypot(d[0], d[1], d[2]);
        const px = p.x;
        const py = p.y;
        const flat = Math.hypot(px - CX, py - CY);
        if (flat > 22) {
            lab.set(ray, { x1: CX + (px - CX) * 9 / flat, y1: CY + (py - CY) * 9 / flat, x2: px - (px - CX) * 11 / flat, y2: py - (py - CY) * 11 / flat, class: "arrow" });
        } else {
            lab.set(ray, { class: "arrow hide" });
        }

        if (Math.max(a[0], a[1], a[2]) < 0.05) {
            for (const face in wedges) lab.set(wedges[face], { class: "zone" });
            lab.set(fan, { d: "M0,0" });
            lab.set(ring, { class: "hot hide" });
            lab.set(foot, { class: "dashed hide" });
            lab.set(depthLine, { class: "arrow hot thick hide" });
            lab.set(depthText, { text: "" });
            lab.set(marker, { class: "hot hide" });
            lab.set(title, { text: "No face" });
            lab.set(line1, { text: "" });
            lab.set(line2, { text: "" });
            lab.set(line3, { text: "" });
            faceOut.set("–");
            depthOut.set("–");
            texelOut.set("–");
            note.set("P is at the light: d has no direction, so no face holds it.", "bad");
            return;
        }

        const face = getCubeFace(d);
        const F = FORWARD[face];
        const depth = dot(d, F);
        const sc = dot(d, S[face]);
        const tc = dot(d, T[face]);
        const s = (sc / depth + 1) / 2;
        const t = (tc / depth + 1) / 2;
        const n = size.value;
        const texel = 2 * depth / n;
        const side = face !== 2 && face !== 3;

        for (const f in wedges) lab.set(wedges[f], { class: Number(f) === face ? "accent o-50" : "zone" });
        lab.set(ring, { class: side ? "hot hide" : "hot" });

        // the face's texel rays, every eighth of the face, in this slice (t = 0.5)
        let fanPath = "";
        if (side) {
            for (let k = 1; k < 8; k++) {
                const u = 2 * k / 8 - 1;
                const dir = [F[0] + S[face][0] * u, F[2] + S[face][2] * u];
                fanPath += `M${CX},${CY}L${(CX + dir[0] * R).toFixed(1)},${(CY + dir[1] * R).toFixed(1)}`;
            }
        }
        lab.set(fan, { d: fanPath || "M0,0" });

        if (side) {
            const fx = F[0] !== 0 ? px : CX;
            const fy = F[0] !== 0 ? CY : py;
            lab.set(foot, { x1: px, y1: py, x2: fx, y2: fy, class: "dashed" });
            lab.set(depthLine, { x1: CX + F[0] * 8, y1: CY + F[2] * 8, x2: fx, y2: fy, class: "arrow hot thick" });
            const tx = F[0] !== 0 ? (CX + fx) / 2 : CX + (px >= CX ? -26 : 26);
            const ty = F[0] !== 0 ? CY + (py >= CY ? -8 : 16) : (CY + fy) / 2 + 4;
            lab.set(depthText, { x: tx, y: ty, text: `depth ${depth.toFixed(2)}` });
        } else {
            lab.set(foot, { class: "dashed hide" });
            lab.set(depthLine, { class: "arrow hot thick hide" });
            lab.set(depthText, { x: CX, y: CY + 36, text: face === 2 ? "P is above" : "P is below" });
        }

        lab.set(title, { text: `The ${NAMES[face]} face's image · layer ${face}` });
        lab.set(marker, { cx: IX + s * IS, cy: IY + t * IS, class: "hot" });
        const scName = axisWord(S[face], "d.");
        const tcName = axisWord(T[face], "d.");
        lab.set(line1, { text: `s grows toward ${axisWord(S[face], "")}, t toward ${axisWord(T[face], "")}` });
        lab.set(line2, { text: `sc = ${scName} = ${sc.toFixed(2)} · tc = ${tcName} = ${tc.toFixed(2)} · |ma| = ${depth.toFixed(2)}` });
        lab.set(line3, { text: `s = ${s.toFixed(3)} · t = ${t.toFixed(3)} · texel (${Math.min(n - 1, Math.floor(s * n))}, ${Math.min(n - 1, Math.floor(t * n))}) of ${n}` });

        faceOut.set(NAMES[face]);
        depthOut.set(`${depth.toFixed(2)} (${len.toFixed(2)})`);
        texelOut.set(texel.toFixed(4));

        const axis = AXES[Math.floor(face / 2)];
        const sorted = a.slice().sort(function (x, y) { return y - x; });
        const parts = `(${a[0].toFixed(2)}, ${a[1].toFixed(2)}, ${a[2].toFixed(2)})`;
        const longer = 100 * (len / depth - 1);
        if (sorted[0] - sorted[1] < 0.02 * sorted[0]) {
            note.set(`Two parts of |d| ${parts} are equal: P is on the edge between two faces. Either face holds it, and a cube lookup filters across the edge.`);
        } else if (longer > 25) {
            note.set(`|d.${axis}| is the biggest of ${parts}: face ${NAMES[face]}. P is near the face's corner, so |d| is ${longer.toFixed(0)} % longer than the depth: comparing |d| would put P in its own shadow.`, "bad");
        } else {
            note.set(`|d.${axis}| is the biggest of ${parts}, and d.${axis} ${d[Math.floor(face / 2)] >= 0 ? ">" : "<"} 0: face ${NAMES[face]}. Compare the depth ${depth.toFixed(2)} along its axis, not |d| = ${len.toFixed(2)}.`, "ok");
        }
    });
});

// A spot light seen from the side, shining down. The cone follows the glTF rule (cone = saturate(cos * scale +
// offset), light times cone squared); its shadow map is one square perspective map with a field of view of twice the
// outer angle, so the cone is the circle inside the square, and a texel at P is 2 * depth * tan(outer) / size.
RasmLab.register("spot-cone", function (lab) {
    const svg = lab.svg(760, 360, "A side view of a spot light shining down, with its inner and outer cone and a draggable point P. On the right, the square shadow map as the light sees it, with the cone's light as rings and P's place in it.");
    const outerS = lab.slider({ label: "Outer angle", min: 5, max: 85, value: 22, format: function (v) { return `${v}°`; } });
    const innerS = lab.slider({ label: "Inner angle", min: 0, max: 85, value: 14, format: function (v) { return `${v}°`; } });
    const coneOut = lab.readout("of the light reaches P (cone²)");
    const fovOut = lab.readout("the map's field of view");
    const texelOut = lab.readout("one texel at P, 2048 map");
    const note = lab.note();

    const MAP = 2048;
    const LX = 250;
    const LY = 34;
    const K = 30;               // pixels per world unit
    const BOX = { left: 10, right: 490, bottom: 350 };
    function rad(v) { return v * Math.PI / 180; }
    // where a ray from the light at angle a from the axis (positive to the right) leaves the drawn box
    function rayEnd(a) {
        const dx = Math.sin(a);
        const dy = Math.cos(a);
        const toBottom = (BOX.bottom - LY) / dy;
        const toSide = dx > 0 ? (BOX.right - LX) / dx : dx < 0 ? (BOX.left - LX) / dx : Infinity;
        const k = Math.min(toBottom, toSide);
        return { x: LX + dx * k, y: LY + dy * k, side: toSide < toBottom };
    }
    function conePoints(a) {
        const l = rayEnd(-a);
        const r = rayEnd(a);
        let pts = `${LX},${LY} ${r.x.toFixed(1)},${r.y.toFixed(1)}`;
        if (r.side) pts += ` ${BOX.right},${BOX.bottom}`;
        if (l.side) pts += ` ${BOX.left},${BOX.bottom}`;
        return pts + ` ${l.x.toFixed(1)},${l.y.toFixed(1)}`;
    }

    const outerFill = lab.el(svg, "polygon", { class: "lit o-50" });
    const innerFill = lab.el(svg, "polygon", { class: "lit" });
    const outerL = lab.el(svg, "line", { class: "arrow light" });
    const outerR = lab.el(svg, "line", { class: "arrow light" });
    const innerL = lab.el(svg, "line", { class: "dashed" });
    const innerR = lab.el(svg, "line", { class: "dashed" });
    lab.el(svg, "line", { x1: LX, y1: LY, x2: LX, y2: BOX.bottom, class: "dashed" });
    const width = lab.el(svg, "line", { class: "arrow hot thick" });
    const widthText = lab.el(svg, "text", { class: "small t-hot halo" });
    const ray = lab.el(svg, "line", { class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "circle", { cx: LX, cy: LY - 2, r: 9, class: "light" });
    lab.el(svg, "text", { x: LX + 14, y: LY - 14, class: "small", text: "light" });

    // the map as the light sees it
    const MX = 630;
    const MY = 160;
    const MR = 100;
    lab.el(svg, "text", { x: 520, y: 30, class: "title", text: "The map, seen from the light" });
    lab.el(svg, "rect", { x: MX - MR, y: MY - MR, width: 2 * MR, height: 2 * MR, class: "paper" });
    const rings = lab.el(svg, "g");
    let grid = "";
    for (let k = 1; k < 8; k++) grid += `M${MX - MR + k * MR / 4},${MY - MR}V${MY + MR}M${MX - MR},${MY - MR + k * MR / 4}H${MX + MR}`;
    lab.el(svg, "path", { d: grid, class: "grid" });
    lab.el(svg, "rect", { x: MX - MR, y: MY - MR, width: 2 * MR, height: 2 * MR, class: "hot" });
    lab.el(svg, "circle", { cx: MX, cy: MY, r: MR, class: "arrow light" });
    const innerCircle = lab.el(svg, "circle", { cx: MX, cy: MY, class: "dashed" });
    const marker = lab.el(svg, "circle", { cy: MY, r: 6, class: "hot" });
    lab.el(svg, "text", { x: MX, y: MY + MR + 22, class: "mid small", text: "the cone: the circle inside the square" });
    const mapText = lab.el(svg, "text", { x: MX, y: MY + MR + 40, class: "mid small" });

    const p = lab.handle(svg, { x: LX + 10 * Math.tan(rad(18)) * K, y: LY + 10 * K, label: "Point P", minX: BOX.left + 4, maxX: BOX.right - 4, minY: LY + 24, maxY: BOX.bottom - 2 });

    lab.draw(function () {
        const outer = rad(outerS.value);
        const inner = rad(innerS.value);
        const scale = 1 / Math.max(Math.cos(inner) - Math.cos(outer), 0.001);
        const offset = -Math.cos(outer) * scale;
        function cone(theta) { return lab.clamp(Math.cos(theta) * scale + offset, 0, 1); }
        const tanOuter = Math.tan(outer);

        lab.set(outerFill, { points: conePoints(outer) });
        lab.set(innerFill, { points: conePoints(Math.min(inner, outer)) });
        const ol = rayEnd(-outer);
        const or = rayEnd(outer);
        const il = rayEnd(-inner);
        const ir = rayEnd(inner);
        lab.set(outerL, { x1: LX, y1: LY, x2: ol.x, y2: ol.y });
        lab.set(outerR, { x1: LX, y1: LY, x2: or.x, y2: or.y });
        lab.set(innerL, { x1: LX, y1: LY, x2: il.x, y2: il.y });
        lab.set(innerR, { x1: LX, y1: LY, x2: ir.x, y2: ir.y });

        // P, in the light's frame: h across the axis, z along it
        const h = (p.x - LX) / K;
        const z = (p.y - LY) / K;
        const theta = Math.atan2(Math.abs(h), z);
        const c = cone(theta);
        const u = h / (z * tanOuter);
        const inside = Math.abs(u) <= 1;
        const halfWidth = z * tanOuter;
        const texel = 2 * halfWidth / MAP;
        const flat = Math.hypot(p.x - LX, p.y - LY);
        lab.set(ray, { x1: LX, y1: LY + 8, x2: p.x - (p.x - LX) * 11 / flat, y2: p.y - (p.y - LY) * 11 / flat });

        const x0 = Math.max(BOX.left, LX - halfWidth * K);
        const x1 = Math.min(BOX.right, LX + halfWidth * K);
        lab.set(width, { x1: x0, y1: p.y, x2: x1, y2: p.y });
        const label = `map width here: ${(2 * halfWidth).toFixed(1)} units`;
        const labelX = p.x > LX ? Math.max(BOX.left + 4, x0) : Math.min(BOX.right - 4 - label.length * 5.5, x1 - label.length * 5.5);
        lab.set(widthText, { x: labelX.toFixed(1), y: (p.y - 10).toFixed(1), text: label });

        // the cone's light in the map: rings of cone squared, from the inner circle out to the outer one
        lab.clear(rings);
        const ri = inner < outer ? MR * Math.tan(inner) / tanOuter : MR;
        lab.el(rings, "circle", { cx: MX, cy: MY, r: ri.toFixed(1), class: "lit" });
        const RINGS = 12;
        for (let k = 0; k < RINGS && ri < MR; k++) {
            const r0 = ri + (MR - ri) * k / RINGS;
            const r1 = ri + (MR - ri) * (k + 1) / RINGS;
            const mid = Math.atan(((r0 + r1) / 2 / MR) * tanOuter);
            const v = cone(mid);
            const d = `M${MX - r1},${MY}a${r1},${r1} 0 1,0 ${2 * r1},0a${r1},${r1} 0 1,0 ${-2 * r1},0` +
                      `M${MX - r0},${MY}a${r0},${r0} 0 1,0 ${2 * r0},0a${r0},${r0} 0 1,0 ${-2 * r0},0`;
            lab.el(rings, "path", { d: d, "fill-rule": "evenodd", opacity: (v * v).toFixed(3), class: "lit" });
        }
        lab.set(innerCircle, { r: ri.toFixed(1) });
        lab.set(marker, { cx: (MX + lab.clamp(u, -1.15, 1.15) * MR).toFixed(1), class: Math.abs(u) <= 1.15 ? "hot" : "hot hide" });
        lab.set(mapText, { text: inside ? `P is in the map, ${(Math.abs(u) * 100).toFixed(0)} % of the way to its edge` : "P is outside the map: no shadow test" });

        coneOut.set((c * c).toFixed(2), c > 0 ? "ok" : "bad");
        fovOut.set(`${(2 * outerS.value).toFixed(0)}°`, outerS.value > 60 ? "bad" : "");
        texelOut.set(texel.toFixed(4), outerS.value > 60 ? "bad" : "");

        if (innerS.value >= outerS.value) {
            note.set("The inner angle is not below the outer one: the fade has no width, so the cone ends in a hard edge.", "bad");
        } else if (outerS.value > 60) {
            note.set(`A wide cone: the map's field of view is ${2 * outerS.value}°, so one texel at P covers ${texel.toFixed(4)} units, ${(tanOuter / Math.tan(rad(22))).toFixed(1)} times more than with an outer angle of 22°.`, "bad");
        } else if (!inside) {
            note.set("P is outside the outer cone: no light reaches it, and it is outside the map, so no shadow test runs either.");
        } else if (c >= 1) {
            note.set("P is inside the inner cone: full light. Its shadow test reads the map near the middle.", "ok");
        } else {
            note.set(`P is in the fade between the inner and the outer cone: cone = ${c.toFixed(2)}, so ${(c * c).toFixed(2)} of the light reaches it.`, "ok");
        }
    });
});
