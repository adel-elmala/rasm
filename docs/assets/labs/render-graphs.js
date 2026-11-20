// Labs of docs/guides/render-graphs.html.

// The page's small frame. The reader cuts or connects reads; the lab builds the edges, sorts with Kahn's
// algorithm (FIFO, in the order the passes were added), culls by walking back from the sink, and counts the
// layout transitions before each pass that runs.
RasmLab.register("graph-compile", function (lab) {
    const svg = lab.svg(760, 300, "Five passes and the resources between them. Each read has a dot that cuts or connects it. Passes that the backbuffer does not need are culled, and the rest are listed in the order they run.");
    const runOut = lab.readout("passes that run");
    const cullOut = lab.readout("passes culled");
    const barrierOut = lab.readout("layout transitions in the frame");
    const note = lab.note();

    // in the order they were added, like the page: Post, Lighting, Shadow, GBuffer, Debug
    const PASSES = [
        { name: "Post", x: 640, y: 30, writes: ["backbuffer"] },
        { name: "Lighting", x: 330, y: 30, writes: ["HDR"] },
        { name: "Shadow", x: 20, y: 30, writes: ["shadow map"] },
        { name: "GBuffer", x: 20, y: 140, writes: ["G-buffer"] },
        { name: "Debug", x: 330, y: 140, writes: ["debug view"] },
    ];
    const RESOURCES = [
        { name: "shadow map", x: 160, y: 37 },
        { name: "G-buffer", x: 160, y: 147 },
        { name: "HDR", x: 470, y: 37 },
        { name: "debug view", x: 470, y: 147 },
    ];
    // a read: resource -> pass, with the line it is drawn on
    const READS = [
        { resource: "shadow map", pass: "Lighting", on: true, line: [260, 52, 328, 52] },
        { resource: "G-buffer", pass: "Lighting", on: true, line: [260, 154, 330, 76] },
        { resource: "G-buffer", pass: "Debug", on: true, line: [260, 162, 328, 162] },
        { resource: "HDR", pass: "Post", on: true, line: [570, 52, 638, 52] },
        { resource: "debug view", pass: "Post", on: false, line: [570, 154, 640, 76] },
    ];
    const PASS_W = 110;
    const PASS_H = 44;

    function passIndex(name) {
        for (let k = 0; k < PASSES.length; k++) if (PASSES[k].name === name) return k;
        return -1;
    }
    function writerOf(resource) {
        for (let k = 0; k < PASSES.length; k++) if (PASSES[k].writes.indexOf(resource) >= 0) return k;
        return -1;
    }

    // write arrows never change
    PASSES.forEach(function (p) {
        if (p.name === "Post") return;
        lab.el(svg, "line", { x1: p.x + PASS_W, y1: p.y + 22, x2: p.x + PASS_W + 28, y2: p.y + 22, class: "arrow", "marker-end": "url(#ah)" });
    });
    lab.el(svg, "line", { x1: 695, y1: 74, x2: 695, y2: 138, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "rect", { x: 640, y: 140, width: 110, height: 44, rx: 6, class: "box" });
    lab.el(svg, "text", { x: 695, y: 160, class: "mid small", text: "backbuffer" });
    lab.el(svg, "text", { x: 695, y: 175, class: "mid small", text: "(imported)" });
    lab.el(svg, "text", { x: 750, y: 22, class: "end small t-gpu", text: "the goal: it writes the backbuffer" });

    const readLines = READS.map(function (r) {
        return lab.el(svg, "line", { x1: r.line[0], y1: r.line[1], x2: r.line[2], y2: r.line[3] });
    });
    const passRects = PASSES.map(function (p) {
        return lab.el(svg, "rect", { x: p.x, y: p.y, width: PASS_W, height: PASS_H, rx: 6 });
    });
    const passTitles = PASSES.map(function (p) {
        return lab.el(svg, "text", { x: p.x + PASS_W / 2, y: p.y + 27, text: p.name });
    });
    const passTags = PASSES.map(function (p) {
        return lab.el(svg, "text", { x: p.x + PASS_W / 2, y: p.y + PASS_H + 16, class: "mid small t-hot" });
    });
    const resourceRects = RESOURCES.map(function (r) {
        const rect = lab.el(svg, "rect", { x: r.x, y: r.y, width: 100, height: 30, rx: 15 });
        lab.el(svg, "text", { x: r.x + 50, y: r.y + 19, class: "mid small", text: r.name });
        return rect;
    });
    const dots = READS.map(function (r) {
        const dot = lab.el(svg, "circle", { cx: (r.line[0] + r.line[2]) / 2, cy: (r.line[1] + r.line[3]) / 2, r: 9 });
        lab.clickable(dot, `Cut or connect: ${r.pass} reads the ${r.resource}`, function () { r.on = !r.on; });
        return dot;
    });

    lab.el(svg, "text", { x: 20, y: 250, class: "title", text: "order" });
    const orderRow = lab.el(svg, "g");

    lab.draw(function () {
        const n = PASSES.length;
        // edges: for every read, writer -> reader
        const edges = [];
        READS.forEach(function (r) {
            if (r.on) edges.push([writerOf(r.resource), passIndex(r.pass)]);
        });

        // Kahn's algorithm, FIFO queue in the order added
        const inDegree = PASSES.map(function () { return 0; });
        edges.forEach(function (e) { inDegree[e[1]] += 1; });
        const queue = [];
        for (let k = 0; k < n; k++) if (inDegree[k] === 0) queue.push(k);
        const order = [];
        while (queue.length) {
            const p = queue.shift();
            order.push(p);
            edges.forEach(function (e) {
                if (e[0] !== p) return;
                inDegree[e[1]] -= 1;
                if (inDegree[e[1]] === 0) queue.push(e[1]);
            });
        }

        // cull: walk the edges backwards from the pass that writes the imported backbuffer
        const live = PASSES.map(function () { return false; });
        const stack = [writerOf("backbuffer")];
        while (stack.length) {
            const p = stack.pop();
            if (live[p]) continue;
            live[p] = true;
            edges.forEach(function (e) { if (e[1] === p) stack.push(e[0]); });
        }

        // layout transitions: one state per resource, compared with what the next pass needs
        const state = {};
        const transitions = PASSES.map(function () { return 0; });
        order.forEach(function (p) {
            if (!live[p]) return;
            READS.forEach(function (r) {
                if (!r.on || passIndex(r.pass) !== p) return;
                if (state[r.resource] !== "sampled") transitions[p] += 1;
                state[r.resource] = "sampled";
            });
            PASSES[p].writes.forEach(function (resource) {
                if (state[resource] !== "attachment") transitions[p] += 1;
                state[resource] = "attachment";
            });
        });

        // draw the graph
        PASSES.forEach(function (p, k) {
            lab.set(passRects[k], { class: live[k] ? "gpu" : "ghost" });
            lab.set(passTitles[k], { class: live[k] ? "mid title" : "mid title t-faint" });
            lab.set(passTags[k], { text: live[k] || p.name === "Post" ? "" : "culled" });
        });
        RESOURCES.forEach(function (r, k) {
            lab.set(resourceRects[k], { class: live[writerOf(r.name)] ? "mem" : "mem off" });
        });
        READS.forEach(function (r, k) {
            const used = r.on && live[passIndex(r.pass)];
            lab.set(readLines[k], {
                class: r.on ? (used ? "arrow hot" : "arrow") : "arrow dashed o-50",
                "marker-end": r.on ? (used ? "url(#ah-hot)" : "url(#ah)") : null,
            });
            lab.set(dots[k], { class: r.on ? "badge pick" : "box pick" });
        });

        // the order that runs
        lab.clear(orderRow);
        let x = 80;
        let step = 0;
        let total = 0;
        order.forEach(function (p) {
            if (!live[p]) return;
            step += 1;
            total += transitions[p];
            lab.el(orderRow, "rect", { x: x, y: 232, width: 100, height: 26, rx: 13, class: "gpu" });
            lab.el(orderRow, "text", { x: x + 50, y: 249, class: "mid small", text: `${step}. ${PASSES[p].name}` });
            lab.el(orderRow, "text", { x: x + 50, y: 276, class: "mid small t-hot", text: transitions[p] === 1 ? "1 transition" : `${transitions[p]} transitions` });
            x += 108;
        });
        const culled = [];
        order.forEach(function (p) { if (!live[p]) culled.push(PASSES[p].name); });
        if (culled.length) lab.el(orderRow, "text", { x: x + 6, y: 249, class: "small t-faint", text: `culled: ${culled.join(", ")}` });
        lab.el(orderRow, "text", { x: 20, y: 276, class: "small", text: "before it" });

        runOut.set(`${step} / ${n}`, "gpu");
        cullOut.set(String(culled.length), culled.length ? "hot" : "");
        barrierOut.set(String(total));
        if (culled.length === 0) note.set("Every pass runs: the walk back from Post reaches all five, so each one adds to the image.");
        else if (step === 1) note.set("Only Post runs. It reads nothing that another pass writes, so the walk back stops at once.", "bad");
        else note.set(`Culled: ${culled.join(", ")}. The walk back from Post never reaches ${culled.length === 1 ? "it" : "them"}, because no pass that runs reads what ${culled.length === 1 ? "it writes" : "they write"}.`, "ok");
    });
});
