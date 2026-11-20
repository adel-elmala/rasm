// Labs of docs/guides/mesh-simplification.html.

// Simplifies a 2D line with half-edge collapses and quadrics, the 2D form of the guide's algorithm: a point's
// quadric is the sum of the K = p pᵀ of the lines of its two segments, and the cost of moving point v onto its
// neighbour t is tᵀ (Q(v) + Q(t)) t. "closed" joins the last point to the first; an open line keeps its two ends.
// order: "cheapest" takes the cheapest collapse each time, "second" removes every second point, pass after pass.
// Returns the collapses in order: [{ removed, target, cost }], cost as a distance (the square root).
function rasmSimplifyLine(points, closed, order, keep) {
    const n = points.length;
    const prev = [];
    const next = [];
    const alive = [];
    const quadric = [];
    for (let i = 0; i < n; i++) {
        prev.push(closed ? (i + n - 1) % n : i - 1);
        next.push(closed ? (i + 1) % n : (i + 1 < n ? i + 1 : -1));
        alive.push(true);
        quadric.push([0, 0, 0, 0, 0, 0]);       // aa ab ac bb bc cc of the symmetric 3×3 matrix
    }
    for (let i = 0; i < n; i++) {
        const j = next[i];
        if (j < 0) continue;
        // the line through points i and j as (a, b, c) with a unit normal (a, b)
        const dx = points[j].x - points[i].x;
        const dy = points[j].y - points[i].y;
        const length = Math.hypot(dx, dy);
        const a = -dy / length;
        const b = dx / length;
        const c = -(a * points[i].x + b * points[i].y);
        const k = [a * a, a * b, a * c, b * b, b * c, c * c];
        for (let m = 0; m < 6; m++) {
            quadric[i][m] += k[m];
            quadric[j][m] += k[m];
        }
    }
    function cost(v, t) {
        const q = quadric[v].map(function (value, m) { return value + quadric[t][m]; });
        const x = points[t].x;
        const y = points[t].y;
        return Math.max(0, q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x + q[3] * y * y + 2 * q[4] * y + q[5]);
    }
    // the cheaper of the two collapses of point v, or null for a locked end
    function best(v) {
        if (prev[v] < 0 || next[v] < 0) return null;
        const toPrev = cost(v, prev[v]);
        const toNext = cost(v, next[v]);
        return toNext < toPrev ? { removed: v, target: next[v], cost: Math.sqrt(toNext) } : { removed: v, target: prev[v], cost: Math.sqrt(toPrev) };
    }

    const collapses = [];
    let left = n;
    let cursor = 0;         // for "second": the next point to keep
    while (left > keep) {
        let pick = null;
        if (order === "cheapest") {
            for (let v = 0; v < n; v++) {
                if (!alive[v]) continue;
                const candidate = best(v);
                if (candidate && (!pick || candidate.cost < pick.cost - 1e-12)) pick = candidate;
            }
        } else {
            const v = next[cursor];
            pick = { removed: v, target: cursor, cost: Math.sqrt(cost(v, cursor)) };
            cursor = next[v];
        }
        if (!pick) break;
        const v = pick.removed;
        for (let m = 0; m < 6; m++) quadric[pick.target][m] += quadric[v][m];
        alive[v] = false;
        next[prev[v]] = next[v];
        prev[next[v]] = prev[v];
        left -= 1;
        collapses.push(pick);
    }
    return collapses;
}

// A closed outline with straight sides, a round end, a spike and a few small bumps. The slider replays the
// collapses of the algorithm up to a point count.
RasmLab.register("collapse-outline", function (lab) {
    const svg = lab.svg(760, 330, "A closed outline with straight sides, a round end and a spike, and the simplified outline on top of it. The largest gap between the two is marked.");

    // the outline, in units (5 wide), counter-clockwise
    const points = [];
    for (let k = 0; k <= 8; k++) points.push({ x: k * 0.5, y: 0 });
    for (let k = 1; k < 12; k++) {
        const angle = (-90 + k * 15) * Math.PI / 180;
        points.push({ x: 4 + Math.cos(angle), y: 1 + Math.sin(angle) });
    }
    [[4, 2], [3.5, 2], [3, 2], [2.5, 2], [2.2, 2], [2, 2.7], [1.8, 2], [1.5, 2], [1.25, 2.06], [1, 2], [0.75, 2.06], [0.5, 2], [0.25, 2.05],
     [0, 2], [0, 1.5], [0, 1], [0, 0.5]].forEach(function (p) { points.push({ x: p[0], y: p[1] }); });
    const TOTAL = points.length;
    const runs = {
        cheapest: rasmSimplifyLine(points, true, "cheapest", 3),
        second: rasmSimplifyLine(points, true, "second", 3),
    };

    const keep = lab.slider({ label: "Points kept", min: 3, max: TOTAL, value: 16 });
    const order = lab.choice({
        label: "Order",
        options: [{ value: "cheapest", label: "cheapest collapse first" }, { value: "second", label: "every second point" }],
        value: "cheapest",
    });
    const errorOut = lab.readout("error of this level: its largest collapse cost");
    const gapOut = lab.readout("the real largest gap to the original");
    const nextOut = lab.readout("cost of the next collapse");
    const note = lab.note();

    const SCALE = 100;
    function sx(x) { return 40 + x * SCALE; }
    function sy(y) { return 300 - y * SCALE; }
    function pointList(indices) {
        return indices.map(function (i) { return `${sx(points[i].x).toFixed(1)},${sy(points[i].y).toFixed(1)}`; }).join(" ");
    }
    const all = points.map(function (unused, i) { return i; });

    const shape = lab.el(svg, "polygon", { class: "gpu" });
    lab.el(svg, "polygon", { points: pointList(all), class: "ghost" });
    const gapLine = lab.el(svg, "line", { class: "arrow bad thick" });
    const dots = lab.el(svg, "g");
    const nextRing = lab.el(svg, "circle", { r: 9, class: "hot" });
    lab.el(svg, "text", { x: 580, y: 60, class: "small", text: "dashed: the original, 37 points" });
    lab.el(svg, "text", { x: 580, y: 80, class: "small t-gpu", text: "green: the simplified outline" });
    lab.el(svg, "text", { x: 580, y: 100, class: "small t-hot", text: "ring: the next point to go" });
    lab.el(svg, "text", { x: 580, y: 120, class: "small t-bad", text: "red: the largest gap" });
    const countText = lab.el(svg, "text", { x: 580, y: 160, class: "big" });
    lab.el(svg, "text", { x: 580, y: 180, class: "small", text: "the outline is 5 units wide" });

    // the largest distance from the original outline to the outline through "kept": { gap, from, to }
    function largestGap(kept) {
        let worst = { gap: 0, from: null, to: null };
        for (let i = 0; i < TOTAL; i++) {
            const a = points[i];
            const b = points[(i + 1) % TOTAL];
            for (let s = 0; s < 8; s++) {
                const p = { x: a.x + (b.x - a.x) * s / 8, y: a.y + (b.y - a.y) * s / 8 };
                let nearest = { gap: Infinity, to: null };
                for (let k = 0; k < kept.length; k++) {
                    const c = points[kept[k]];
                    const d = points[kept[(k + 1) % kept.length]];
                    const ex = d.x - c.x;
                    const ey = d.y - c.y;
                    const t = lab.clamp(((p.x - c.x) * ex + (p.y - c.y) * ey) / (ex * ex + ey * ey), 0, 1);
                    const q = { x: c.x + ex * t, y: c.y + ey * t };
                    const distance = Math.hypot(p.x - q.x, p.y - q.y);
                    if (distance < nearest.gap) nearest = { gap: distance, to: q };
                }
                if (nearest.gap > worst.gap) worst = { gap: nearest.gap, from: p, to: nearest.to };
            }
        }
        return worst;
    }
    // the points left and the level's error after the first "count" collapses of a run
    function level(run, count) {
        const gone = {};
        let error = 0;
        for (let k = 0; k < count; k++) {
            gone[run[k].removed] = true;
            error = Math.max(error, run[k].cost);
        }
        return { kept: all.filter(function (i) { return !gone[i]; }), error: error };
    }

    lab.draw(function () {
        const run = runs[order.value];
        const count = TOTAL - keep.value;
        const now = level(run, count);
        const gap = largestGap(now.kept);
        const upcoming = run[count];

        lab.set(shape, { points: pointList(now.kept) });
        lab.clear(dots);
        now.kept.forEach(function (i) {
            lab.el(dots, "circle", { cx: sx(points[i].x), cy: sy(points[i].y), r: 3.5, class: "cpu" });
        });
        if (gap.gap > 0.005) lab.set(gapLine, { x1: sx(gap.from.x), y1: sy(gap.from.y), x2: sx(gap.to.x), y2: sy(gap.to.y), class: "arrow bad thick" });
        else lab.set(gapLine, { class: "hide" });
        if (upcoming) lab.set(nextRing, { cx: sx(points[upcoming.removed].x), cy: sy(points[upcoming.removed].y), class: "hot" });
        else lab.set(nextRing, { class: "hide" });
        lab.set(countText, { text: `${keep.value} of ${TOTAL} points` });

        errorOut.set(now.error.toFixed(2));
        gapOut.set(gap.gap.toFixed(2), gap.gap > 0.15 ? "bad" : "ok");
        nextOut.set(upcoming ? upcoming.cost.toFixed(2) : "–");
        if (count === 0) note.set("Nothing is removed yet. Lower the number of points.");
        else if (order.value === "second") {
            const other = largestGap(level(runs.cheapest, count).kept).gap;
            const worse = gap.gap > other + 0.005;
            note.set(`Without costs, ${keep.value} points leave a gap of ${gap.gap.toFixed(2)}. Cheapest first leaves ${other.toFixed(2)} with the same number of points.`, worse ? "bad" : "");
        } else if (gap.gap <= 0.005) note.set("Free so far: only points on straight sides are gone. Their cost is 0, and the outline did not move.", "ok");
        else if (gap.gap <= 0.15) note.set(`Small details went first. The outline moved by at most ${gap.gap.toFixed(2)} units, and the spike and the round end are still there.`, "ok");
        else note.set(`Too few points for this shape: the outline moved by ${gap.gap.toFixed(2)} units. The next collapse costs ${upcoming ? upcoming.cost.toFixed(2) : "even more"}.`, "bad");
    });
});
