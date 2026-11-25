// Labs of docs/guides/point-octree.html.

// A seeded random number generator, so a lab draws the same points every time.
function makeOctreeRandom(seed) {
    let state = seed >>> 0;
    return function () {
        state = (state + 0x6D2B79F5) >>> 0;
        let t = state;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// The page's octree in two dimensions (a quadtree): each node keeps the point nearest the centre of each cell of an
// 8 x 8 grid over its square, and passes the others to its four children; a node with 40 points or fewer left keeps
// them all. The cut wants a node's children while its spacing (its square over 8) on screen passes the error.
RasmLab.register("octree-cut", function (lab) {
    const W = 760;
    const H = 380;
    const SIDE = 340;
    const LEFT = 400;
    const TOP = 20;
    const GRID = 8;
    const LEAF = 40;
    const FOCAL = 60;
    const svg = lab.svg(W, H, "A square of scattered points seen from above, cut into a quadtree. The eye on the left is draggable. The squares of the nodes drawn are outlined and their points shown, finer near the eye.");
    const error = lab.slider({ label: "Error ε (pixels on screen)", min: 0.5, max: 6, step: 0.25, value: 1.5, format: function (v) { return v.toFixed(2) + " px"; } });
    const nodesOut = lab.readout("nodes drawn");
    const pointsOut = lab.readout("points drawn");
    const deepOut = lab.readout("deepest level drawn");
    const note = lab.note();

    // Points of a ground with a few dense patches, in the unit square.
    const random = makeOctreeRandom(5);
    const all = [];
    for (let i = 0; i < 5200; ++i)
        all.push({ x: random(), y: random() });
    for (let p = 0; p < 6; ++p) {
        const cx = 0.15 + 0.7 * random();
        const cy = 0.15 + 0.7 * random();
        for (let i = 0; i < 500; ++i) {
            const a = 6.2832 * random();
            const r = 0.06 * Math.sqrt(random());
            all.push({ x: Math.min(0.999, Math.max(0, cx + r * Math.cos(a))), y: Math.min(0.999, Math.max(0, cy + r * Math.sin(a))) });
        }
    }

    // The tree: nodes in an array, each with its square, its points and its children.
    const nodes = [];
    function build(x, y, size, level, parent, points) {
        const node = { x: x, y: y, size: size, level: level, parent: parent, children: [], points: [] };
        nodes.push(node);
        if (points.length <= LEAF) {
            node.points = points;
            return node;
        }
        const cell = size / GRID;
        const best = {};
        for (let i = 0; i < points.length; ++i) {
            const p = points[i];
            const cx = Math.min(GRID - 1, Math.floor((p.x - x) / cell));
            const cy = Math.min(GRID - 1, Math.floor((p.y - y) / cell));
            const key = cy * GRID + cx;
            const dx = p.x - (x + (cx + 0.5) * cell);
            const dy = p.y - (y + (cy + 0.5) * cell);
            const d = dx * dx + dy * dy;
            if (!(key in best) || d < best[key].d)
                best[key] = { i: i, d: d };
        }
        const kept = {};
        for (const key in best) {
            kept[best[key].i] = true;
            node.points.push(points[best[key].i]);
        }
        const quarters = [[], [], [], []];
        const half = size / 2;
        for (let i = 0; i < points.length; ++i) {
            if (kept[i])
                continue;
            const p = points[i];
            quarters[(p.x >= x + half ? 1 : 0) + (p.y >= y + half ? 2 : 0)].push(p);
        }
        for (let q = 0; q < 4; ++q) {
            if (quarters[q].length > 0)
                node.children.push(build(x + (q & 1 ? half : 0), y + (q & 2 ? half : 0), half, level + 1, node, quarters[q]));
        }
        return node;
    }
    build(0, 0, 1, 0, null, all);

    const levelClass = ["mem", "gpu", "cpu", "shader", "hot", "ok"];
    lab.el(svg, "rect", { x: LEFT, y: TOP, width: SIDE, height: SIDE, class: "paper" });
    const squares = lab.el(svg, "g", {});
    const dots = lab.el(svg, "g", {});
    lab.el(svg, "text", { x: 20, y: 30, class: "small t-faint", text: "Drag the eye. Each square drawn is a node;" });
    lab.el(svg, "text", { x: 20, y: 46, class: "small t-faint", text: "its color is its level, 0 the root." });
    const legend = lab.el(svg, "g", {});
    for (let l = 0; l < 6; ++l) {
        lab.el(legend, "circle", { cx: 28 + l * 46, cy: 345, r: 5, class: levelClass[l] });
        lab.el(legend, "text", { x: 38 + l * 46, y: 349, class: "small", text: "L" + l });
    }
    const eye = lab.handle(svg, { x: 150, y: 190, label: "eye", minX: 20, maxX: LEFT + SIDE - 10, minY: TOP + 10, maxY: TOP + SIDE - 10 });

    // The node's square on screen and the eye's distance to its nearest point, in pixels of this picture.
    function screenError(node) {
        const x0 = LEFT + node.x * SIDE;
        const y0 = TOP + node.y * SIDE;
        const s = node.size * SIDE;
        const dx = Math.max(x0 - eye.x, 0, eye.x - (x0 + s));
        const dy = Math.max(y0 - eye.y, 0, eye.y - (y0 + s));
        const distance = Math.max(Math.sqrt(dx * dx + dy * dy), 1);
        return (s / GRID) * FOCAL / distance;
    }

    lab.draw(function () {
        lab.clear(squares);
        lab.clear(dots);
        let drawnNodes = 0;
        let drawnPoints = 0;
        let deepest = 0;
        const stack = [nodes[0]];
        while (stack.length > 0) {
            const node = stack.pop();
            ++drawnNodes;
            drawnPoints += node.points.length;
            deepest = Math.max(deepest, node.level);
            const cls = levelClass[Math.min(node.level, levelClass.length - 1)];
            lab.el(squares, "rect", { x: LEFT + node.x * SIDE, y: TOP + node.y * SIDE, width: node.size * SIDE, height: node.size * SIDE, class: cls + " nofill o-50" });
            for (let i = 0; i < node.points.length; ++i)
                lab.el(dots, "circle", { cx: LEFT + node.points[i].x * SIDE, cy: TOP + node.points[i].y * SIDE, r: 1.2, class: cls });
            if (node.children.length > 0 && screenError(node) > error.value) {
                for (let c = 0; c < node.children.length; ++c)
                    stack.push(node.children[c]);
            }
        }
        nodesOut.set(drawnNodes + " of " + nodes.length, "gpu");
        pointsOut.set(drawnPoints + " of " + all.length, "gpu");
        deepOut.set(String(deepest), "cpu");
        const share = drawnPoints / all.length;
        if (share > 0.9)
            note.set("Almost every point is drawn: the eye is close, or the error is small. A real cloud would not fit in memory like this.", "bad");
        else
            note.set("Near the eye the cut goes down to level " + deepest + "; far away the coarse nodes alone stand for the ground: " + Math.round(share * 100) + " % of the points.", "ok");
    });
});

// How much of a sparse ground a sample covers with points of a fixed size, or grown by the page's adaptive rule: from
// where the points stop covering it density (2) times at their size, sqrt(4 / density) times faster, so near the
// camera they cover it about 4 times. Round points of 2 pixels, at most 32. The spacing the rule reads is measured as the
// page says, 1.2 times a random sample's mean spacing. The open share is counted pixel by pixel.
RasmLab.register("adaptive-size", function (lab) {
    const W = 760;
    const H = 220;
    const SIZE = 2;
    const MAX_SIZE = 32;
    const DENSITY = 2;
    const COVER = 4;
    const AREA = Math.PI / 4;
    const MEASURED = 1.2;
    const context = lab.canvas(W, H, "A strip of ground seen from above, with random points drawn as discs over a dark background. Dark pixels are ground no point covers.");
    const spacing = lab.slider({ label: "Mean spacing on screen (closer: larger)", min: 0.5, max: 14, step: 0.25, value: 5, format: function (v) { return v.toFixed(2) + " px"; } });
    const mode = lab.choice({ label: "Size", options: [{ value: "fixed", label: "fixed, 2 px" }, { value: "adaptive", label: "adaptive" }], value: "adaptive" });
    const sizeOut = lab.readout("point diameter");
    const openOut = lab.readout("ground left open");
    const note = lab.note();

    function diameter(s) {
        if (mode.value === "fixed")
            return SIZE;
        const covering = MEASURED * s * Math.sqrt(DENSITY / AREA);
        const faster = Math.sqrt(Math.max(COVER / DENSITY, 1));
        return Math.min(Math.max(SIZE + faster * Math.max(covering - SIZE, 0), SIZE), MAX_SIZE);
    }

    const covered = new Uint8Array(W * H);
    lab.draw(function () {
        const s = spacing.value;
        const d = diameter(s);
        const r = d / 2;
        const random = makeOctreeRandom(3);
        const count = Math.round(W * H / (s * s));
        covered.fill(0);
        context.fillStyle = lab.css("--bg-raised");
        context.fillRect(0, 0, W, H);
        context.fillStyle = lab.css("--gpu");
        for (let i = 0; i < count; ++i) {
            const px = random() * W;
            const py = random() * H;
            context.beginPath();
            context.arc(px, py, r, 0, 2 * Math.PI);
            context.fill();
            const x0 = Math.max(0, Math.floor(px - r));
            const x1 = Math.min(W - 1, Math.ceil(px + r));
            const y0 = Math.max(0, Math.floor(py - r));
            const y1 = Math.min(H - 1, Math.ceil(py + r));
            for (let y = y0; y <= y1; ++y) {
                for (let x = x0; x <= x1; ++x) {
                    const dx = x + 0.5 - px;
                    const dy = y + 0.5 - py;
                    if (dx * dx + dy * dy <= r * r)
                        covered[y * W + x] = 1;
                }
            }
        }
        // The open share away from the strip's edges, where points outside would have covered too.
        const margin = Math.ceil(MAX_SIZE / 2);
        let open = 0;
        let total = 0;
        for (let y = margin; y < H - margin; ++y) {
            for (let x = margin; x < W - margin; ++x) {
                ++total;
                open += covered[y * W + x] ? 0 : 1;
            }
        }
        const share = 100 * open / total;
        sizeOut.set(d.toFixed(1) + " px", d > SIZE ? "gpu" : "cpu");
        openOut.set(share.toFixed(1) + " %", share < 1 ? "ok" : "bad");
        if (d >= MAX_SIZE && share >= 1)
            note.set("The points reached their largest size: the sample is too sparse for it here, and the ground opens.", "bad");
        else if (share >= 1)
            note.set("Points of " + d.toFixed(1) + " pixels leave " + share.toFixed(1) + " % of the ground open: the specks of background.", "bad");
        else
            note.set("The points close the ground: " + share.toFixed(2) + " % open, at " + d.toFixed(1) + " pixels.", "ok");
    });
});
