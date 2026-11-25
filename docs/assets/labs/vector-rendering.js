// Labs of docs/guides/vector-rendering.html.

// Section 3: a line of three points, built as src/scene/vector.cpp builds it: a quad per segment, a join at the middle
// point on the outer side of the turn (miter up to MITER_LIMIT half widths, else bevel; or bevel; or a round fan) and a
// cap at each end. Each triangle is drawn; the offsets are in half widths, spread by the width.
RasmLab.register("vector-joins", function (lab) {
    const MITER_LIMIT = 2.0;
    const ROUND_STEP = Math.PI / 8;
    const svg = lab.svg(760, 400, "A line of three points with its triangles: two segment quads, the join at the middle point and a cap at each end.");
    const join = lab.choice({ label: "Join", options: ["miter", "bevel", "round"], value: "miter" });
    const cap = lab.choice({ label: "Cap", options: ["butt", "square", "round"], value: "round" });
    const width = lab.slider({ label: "Width", min: 10, max: 80, step: 1, value: 48, format: function (v) { return `${v} px`; } });
    const vertexOut = lab.readout("vertices");
    const triangleOut = lab.readout("triangles");
    const miterOut = lab.readout("miter length");
    const note = lab.note();

    const triangles = lab.el(svg, "g");
    const center = lab.el(svg, "polyline", { class: "arrow dashed" });
    const dots = lab.el(svg, "g");
    const p0 = { x: 150, y: 300 };
    const p2 = { x: 640, y: 290 };
    const middle = lab.handle(svg, { x: 400, y: 120, label: "Move the middle point", minX: 60, maxX: 700, minY: 40, maxY: 370 });

    function sub(a, b) { return { x: a.x - b.x, y: a.y - b.y }; }
    function add(a, b) { return { x: a.x + b.x, y: a.y + b.y }; }
    function scale(a, s) { return { x: a.x * s, y: a.y * s }; }
    function dot(a, b) { return a.x * b.x + a.y * b.y; }
    function unit(a) { const l = Math.hypot(a.x, a.y); return l > 1e-9 ? scale(a, 1 / l) : { x: 1, y: 0 }; }
    function across(along) { return { x: -along.y, y: along.x }; }

    lab.draw(function () {
        const half = width.value / 2;
        const p1 = { x: middle.x, y: middle.y };
        const verts = [];
        const tris = [];
        // A vertex: its anchor and its offset (x along, y across) in the frame of along.
        function vertex(anchor, along, x, y) {
            verts.push(add(anchor, scale(add(scale(along, x), scale(across(along), y)), half)));
            return verts.length - 1;
        }
        function fan(anchor, along, a0, a1) {
            const steps = Math.max(1, Math.ceil(Math.abs(a1 - a0) / ROUND_STEP));
            const c = vertex(anchor, along, 0, 0);
            let previous = vertex(anchor, along, Math.sin(a0), Math.cos(a0));
            for (let s = 1; s <= steps; s++) {
                const a = a0 + (a1 - a0) * s / steps;
                const next = vertex(anchor, along, Math.sin(a), Math.cos(a));
                tris.push([c, previous, next]);
                previous = next;
            }
        }
        function capAt(anchor, forward) {
            if (cap.value === "round") fan(anchor, forward, 0, Math.PI);
            else if (cap.value === "square") {
                const a = vertex(anchor, forward, 0, -1), b = vertex(anchor, forward, 0, 1);
                const c = vertex(anchor, forward, 1, -1), d = vertex(anchor, forward, 1, 1);
                tris.push([a, b, c], [c, b, d]);
            }
        }
        const alongA = unit(sub(p1, p0));
        const alongB = unit(sub(p2, p1));
        capAt(p0, scale(alongA, -1));
        for (const [a, b, along] of [[p0, p1, alongA], [p1, p2, alongB]]) {
            const i = vertex(a, along, 0, -1), j = vertex(a, along, 0, 1), k = vertex(b, along, 0, -1), l = vertex(b, along, 0, 1);
            tris.push([i, j, k], [k, j, l]);
        }
        capAt(p2, alongB);
        // The join, on the side the line turns away from.
        const turn = dot(alongB, across(alongA));
        const side = turn > 0 ? -1 : 1;
        const outerB = { x: dot(scale(across(alongB), side), alongA), y: dot(scale(across(alongB), side), across(alongA)) };
        const bisector = unit(scale(add(across(alongA), across(alongB)), side));
        const cosine = dot(bisector, scale(across(alongA), side));
        const miter = cosine > 1e-6 ? 1 / cosine : Infinity;
        let used = join.value;
        if (Math.abs(turn) < 1e-6 && dot(alongA, alongB) > 0) used = "none";
        else if (join.value === "round") {
            let a1 = Math.atan2(outerB.x, outerB.y);
            const a0 = Math.atan2(0, side);
            if (a1 - a0 > Math.PI) a1 -= 2 * Math.PI;
            if (a0 - a1 > Math.PI) a1 += 2 * Math.PI;
            fan(p1, alongA, a0, a1);
        } else if (join.value === "miter" && miter <= MITER_LIMIT) {
            const tip = scale(bisector, miter);
            const c = vertex(p1, alongA, 0, 0), e = vertex(p1, alongA, 0, side), t = vertex(p1, alongA, dot(tip, alongA), dot(tip, across(alongA)));
            const c2 = vertex(p1, alongB, 0, 0), t2 = vertex(p1, alongB, dot(tip, alongB), dot(tip, across(alongB))), e2 = vertex(p1, alongB, 0, side);
            tris.push([c, e, t], [c2, t2, e2]);
        } else {
            used = "bevel";
            const c = vertex(p1, alongA, 0, 0), e = vertex(p1, alongA, 0, side), o = vertex(p1, alongA, outerB.x, outerB.y);
            tris.push([c, e, o]);
        }

        lab.clear(triangles);
        tris.forEach(function (t) {
            const points = t.map(function (i) { return `${verts[i].x.toFixed(1)},${verts[i].y.toFixed(1)}`; }).join(" ");
            lab.el(triangles, "polygon", { points: points, class: "gpu o-50" });
            lab.el(triangles, "polygon", { points: points, class: "arrow" });
        });
        lab.set(center, { points: `${p0.x},${p0.y} ${p1.x.toFixed(1)},${p1.y.toFixed(1)} ${p2.x},${p2.y}` });
        lab.clear(dots);
        verts.forEach(function (v) { lab.el(dots, "circle", { cx: v.x.toFixed(1), cy: v.y.toFixed(1), r: 3, class: "hot" }); });
        vertexOut.set(`${verts.length}`);
        triangleOut.set(`${tris.length}`);
        miterOut.set(Number.isFinite(miter) ? `${miter.toFixed(2)} half widths` : "none", miter <= MITER_LIMIT ? "ok" : "bad");
        if (used === "none") note.set("The line is straight here: no join is needed.", "ok");
        else if (join.value === "miter" && used === "bevel") note.set(`The miter would reach ${miter.toFixed(2)} half widths, past the limit of ${MITER_LIMIT}: it is beveled instead.`, "bad");
        else note.set(`The ${used} join fills the gap on the outer side of the turn; the inner side is covered twice by the two quads.`, "ok");
    });
});
