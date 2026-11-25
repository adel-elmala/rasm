// Labs of docs/guides/tile-streaming.html.

// A square world seen from above, cut into a quadtree: level 0 is the whole square, each level halves the tiles. A
// tile's error is its size over GRID (one of its mesh's cells), a leaf's is 0. The camera hovers HEIGHT above the
// ground; a tile's screen error is error × pixels per unit / (distance − radius), the rule of the LOD selection guide.
const TileLabWorld = (function () {
    const WORLD = { x: 20, y: 40, size: 360 };   // the map in the picture
    const MAX_LEVEL = 5;
    const GRID = 16;
    const PIXELS_PER_UNIT = 260;                  // at distance 1: a 720 px high view with a 108° field of view

    function tile(level, x, y) {
        const size = WORLD.size / Math.pow(2, level);
        return {
            key: `${level}/${x}/${y}`,
            level: level,
            x: x,
            y: y,
            size: size,
            cx: (x + 0.5) * size,
            cy: (y + 0.5) * size,
            radius: size * Math.SQRT1_2,
            error: level === MAX_LEVEL ? 0 : size / GRID,
            parentError: level === 0 ? 0 : 2 * size / GRID,
        };
    }

    // The camera at (cx, cy) on the map, height h over it.
    function screenError(t, camera, error) {
        const dx = t.cx - camera.x;
        const dy = t.cy - camera.y;
        const distance = Math.sqrt(dx * dx + dy * dy + camera.h * camera.h) - t.radius;
        return error * PIXELS_PER_UNIT / Math.max(distance, 1e-3);
    }

    // The application's walk: refine over tau; a tile refined last time stays refined down to merge × tau.
    function cut(camera, tau, merge, refined) {
        const result = [];
        const next = {};
        function visit(level, x, y) {
            const t = tile(level, x, y);
            const limit = refined && refined[t.key] ? merge * tau : tau;
            if (level < MAX_LEVEL && screenError(t, camera, t.error) > limit) {
                next[t.key] = true;
                for (let c = 0; c < 4; c++) visit(level + 1, 2 * x + (c & 1), 2 * y + (c >> 1));
            } else {
                result.push(t);
            }
        }
        visit(0, 0, 0);
        return { tiles: result, refined: next };
    }

    function parentKey(t) {
        return t.level === 0 ? null : `${t.level - 1}/${t.x >> 1}/${t.y >> 1}`;
    }

    function parentOf(t) {
        return t.level === 0 ? null : tile(t.level - 1, t.x >> 1, t.y >> 1);
    }

    function rect(lab, parent, t, cls) {
        return lab.el(parent, "rect", {
            x: (WORLD.x + t.x * t.size + 1).toFixed(1),
            y: (WORLD.y + t.y * t.size + 1).toFixed(1),
            width: Math.max(t.size - 2, 1).toFixed(1),
            height: Math.max(t.size - 2, 1).toFixed(1),
            rx: 2,
            class: cls,
        });
    }

    return { WORLD: WORLD, MAX_LEVEL: MAX_LEVEL, tile: tile, screenError: screenError, cut: cut, parentKey: parentKey, parentOf: parentOf, rect: rect };
})();

// Section 3: the cut. Drag the camera and change the pixel limit; the tiles where the walk stops are drawn, with their
// level.
RasmLab.register("tile-cut", function (lab) {
    const W = TileLabWorld;
    const svg = lab.svg(760, 420, "A square map cut into tiles from above. Near the camera the tiles are small, far from it they are large.");
    const tau = lab.slider({ label: "Pixel limit τ", min: 4, max: 64, step: 1, value: 16, format: function (v) { return `${v} px`; } });
    const height = lab.slider({ label: "Camera height", min: 2, max: 200, step: 1, value: 20, format: function (v) { return `${v} units`; } });
    const countOut = lab.readout("tiles in the cut");
    const deepOut = lab.readout("the finest level");
    const allOut = lab.readout("tiles at the finest level everywhere");
    const note = lab.note();

    lab.el(svg, "text", { x: W.WORLD.x, y: 26, class: "title", text: "The cut, seen from above" });
    lab.el(svg, "rect", { x: W.WORLD.x, y: W.WORLD.y, width: W.WORLD.size, height: W.WORLD.size, class: "zone" });
    const tiles = lab.el(svg, "g");
    const camera = lab.handle(svg, { x: W.WORLD.x + 120, y: W.WORLD.y + 250, r: 8, label: "Move the camera", minX: W.WORLD.x, maxX: W.WORLD.x + W.WORLD.size, minY: W.WORLD.y, maxY: W.WORLD.y + W.WORLD.size });

    const LEGEND_X = 420;
    lab.el(svg, "text", { x: LEGEND_X, y: 26, class: "title", text: "Tiles per level" });
    const bars = [];
    for (let level = 0; level <= W.MAX_LEVEL; level++) {
        const y = 48 + level * 34;
        lab.el(svg, "text", { x: LEGEND_X, y: y + 15, class: "small", text: `level ${level} · error ${level === W.MAX_LEVEL ? 0 : (W.WORLD.size / Math.pow(2, level) / 16).toFixed(2)}` });
        bars.push({ bar: lab.el(svg, "rect", { x: LEGEND_X + 140, y: y, height: 20, rx: 2, class: "gpu" }), value: lab.el(svg, "text", { y: y + 15, class: "small" }) });
    }
    lab.el(svg, "text", { x: LEGEND_X, y: 280, class: "small", text: "A tile refines when its error, seen from the camera," });
    lab.el(svg, "text", { x: LEGEND_X, y: 296, class: "small", text: "is more than τ pixels; its four children replace it." });
    lab.el(svg, "text", { x: LEGEND_X, y: 320, class: "small t-faint", text: "Drag the camera (the handle on the map)." });

    lab.draw(function () {
        const cam = { x: camera.x - W.WORLD.x, y: camera.y - W.WORLD.y, h: height.value };
        const result = W.cut(cam, tau.value, 1, null);
        lab.clear(tiles);
        const perLevel = [];
        for (let level = 0; level <= W.MAX_LEVEL; level++) perLevel.push(0);
        let finest = 0;
        result.tiles.forEach(function (t) {
            W.rect(lab, tiles, t, `gpu o-${t.level % 2 === 0 ? 50 : 75}`);
            if (t.size >= 22) lab.el(tiles, "text", { x: (W.WORLD.x + t.cx).toFixed(1), y: (W.WORLD.y + t.cy + 4).toFixed(1), class: "mid small", text: `${t.level}` });
            perLevel[t.level] += 1;
            finest = Math.max(finest, t.level);
        });
        perLevel.forEach(function (n, level) {
            const width = Math.min(160, n * 4);
            lab.set(bars[level].bar, { width: Math.max(width, 0.5).toFixed(1) });
            lab.set(bars[level].value, { x: LEGEND_X + 148 + width, text: `${n}` });
        });
        countOut.set(`${result.tiles.length}`, "gpu");
        deepOut.set(`level ${finest}`);
        allOut.set(`${Math.pow(4, W.MAX_LEVEL)}`);
        if (finest === W.MAX_LEVEL) note.set(`The finest tiles sit under the camera; ${result.tiles.length} tiles cover the whole map, against ${Math.pow(4, W.MAX_LEVEL)} at full detail everywhere.`, "ok");
        else note.set(`From this height even level ${finest}'s error is under τ near the camera: ${result.tiles.length} tiles cover the map.`, "ok");
    });
});

// Section 6: the budget. The camera flies a circle; every frame the cut is computed (with hysteresis), its tiles and
// their parents are wanted, and a residency cache of BUDGET tiles decides what loads (by screen error, at most a few
// per frame, each arriving LATENCY frames later), what is dropped (unwanted tiles first, the oldest first; then wanted
// ones of a lower error) and what is drawn (a tile once ready, else its nearest ready parent).
RasmLab.register("tile-budget", function (lab) {
    const W = TileLabWorld;
    const LATENCY = 6;      // frames from a request to its data
    const TAU = 16;
    const MERGE = 0.8;
    const svg = lab.svg(760, 420, "The same map with a camera flying a circle. Drawn tiles, parents drawn in place of children that are not ready, and tiles on their way.");
    const budget = lab.slider({ label: "Budget", min: 12, max: 160, step: 4, value: 96, format: function (v) { return `${v} tiles`; } });
    const perFrame = lab.slider({ label: "Loads per frame", min: 1, max: 8, step: 1, value: 3, format: function (v) { return `${v}`; } });
    const usedOut = lab.readout("tiles in memory, of the budget");
    const drawnOut = lab.readout("drawn");
    const standOut = lab.readout("parents in place of children");
    const note = lab.note();

    lab.el(svg, "text", { x: W.WORLD.x, y: 26, class: "title", text: "What is drawn" });
    lab.el(svg, "rect", { x: W.WORLD.x, y: W.WORLD.y, width: W.WORLD.size, height: W.WORLD.size, class: "zone" });
    const tiles = lab.el(svg, "g");
    const cameraDot = lab.el(svg, "circle", { r: 6, class: "cpu" });

    const LEGEND_X = 420;
    lab.el(svg, "text", { x: LEGEND_X, y: 26, class: "title", text: "Every frame" });
    const legend = [
        ["gpu o-75", "a wanted tile, in memory: drawn"],
        ["accent", "a parent, drawn in place of a child not ready"],
        ["ghost", "on its way (asked for, not here yet)"],
    ];
    legend.forEach(function (row, i) {
        lab.el(svg, "rect", { x: LEGEND_X, y: 44 + i * 26, width: 18, height: 18, rx: 2, class: row[0] });
        lab.el(svg, "text", { x: LEGEND_X + 28, y: 58 + i * 26, class: "small", text: row[1] });
    });
    const MEM = { x: LEGEND_X, y: 150, width: 320, height: 22 };
    lab.el(svg, "text", { x: LEGEND_X, y: MEM.y - 8, class: "small", text: "memory: the budget, and who holds it" });
    lab.el(svg, "rect", { x: MEM.x, y: MEM.y, width: MEM.width, height: MEM.height, rx: 3, class: "zone" });
    const memWanted = lab.el(svg, "rect", { x: MEM.x, y: MEM.y, height: MEM.height, rx: 3, class: "gpu" });
    const memCache = lab.el(svg, "rect", { y: MEM.y, height: MEM.height, class: "mem" });
    const memLoading = lab.el(svg, "rect", { y: MEM.y, height: MEM.height, class: "ghost" });
    lab.el(svg, "text", { x: MEM.x, y: MEM.y + 40, class: "small t-gpu", text: "wanted" });
    lab.el(svg, "text", { x: MEM.x + 70, y: MEM.y + 40, class: "small t-mem", text: "kept, not wanted (the cache)" });
    lab.el(svg, "text", { x: MEM.x + 260, y: MEM.y + 40, class: "small t-faint", text: "loading" });
    const orderTitle = lab.el(svg, "text", { x: LEGEND_X, y: 236, class: "small", text: "To make room it drops, in this order:" });
    lab.el(svg, "text", { x: LEGEND_X, y: 256, class: "small", text: "1. tiles nobody wants, the oldest first" });
    lab.el(svg, "text", { x: LEGEND_X, y: 274, class: "small", text: "2. wanted tiles of a lower error than the new one" });
    lab.el(svg, "text", { x: LEGEND_X, y: 300, class: "small", text: "A tile that cannot fit waits; its parent stands in." });
    void orderTitle;

    const state = {};       // key -> { t, state: "loading" | "ready", arrive, lastWanted }
    let refined = {};
    let frame = 0;
    let angle = 0;
    let lastStats = { used: 0, drawn: 0, standIns: 0, waiting: 0, wanted: 0, cached: 0, loading: 0, drawnTiles: [], standInTiles: [] };

    function step() {
        frame += 1;
        angle += 0.012;
        const cam = { x: 180 + 110 * Math.cos(angle), y: 180 + 110 * Math.sin(angle), h: 20 };
        const result = W.cut(cam, TAU, MERGE, refined);
        refined = result.refined;

        // wanted: the cut and its ancestors, each ranked by its parent's error seen at its own bounds
        const wanted = {};
        result.tiles.forEach(function (t) {
            for (let a = t; a; a = W.parentOf(a)) {
                if (wanted[a.key]) break;
                wanted[a.key] = { t: a, priority: W.screenError(a, cam, Math.max(a.error, a.parentError)) };
            }
        });
        // a parent ranks at least as high as its wanted children
        Object.keys(wanted).sort(function (a, b) { return wanted[b].t.level - wanted[a].t.level; }).forEach(function (key) {
            const p = W.parentKey(wanted[key].t);
            if (p && wanted[p]) wanted[p].priority = Math.max(wanted[p].priority, wanted[key].priority);
        });
        Object.keys(wanted).forEach(function (key) { if (state[key]) state[key].lastWanted = frame; });

        // arrivals
        Object.keys(state).forEach(function (key) {
            if (state[key].state === "loading" && frame >= state[key].arrive) state[key].state = "ready";
        });

        function used() { return Object.keys(state).length; }
        // victims: unwanted first (oldest first), then wanted by priority (lowest first)
        function victims() {
            return Object.keys(state).sort(function (a, b) {
                const wa = !!wanted[a];
                const wb = !!wanted[b];
                if (wa !== wb) return wa ? 1 : -1;
                if (!wa) return state[a].lastWanted - state[b].lastWanted;
                return wanted[a].priority - wanted[b].priority;
            });
        }
        let order = victims();
        while (used() > budget.value && order.length > 0) delete state[order.shift()];

        const candidates = Object.keys(wanted).filter(function (key) { return !state[key]; }).sort(function (a, b) { return wanted[b].priority - wanted[a].priority; });
        let started = 0;
        let waiting = 0;
        candidates.forEach(function (key) {
            if (started >= perFrame.value) return;
            if (used() >= budget.value) {
                order = victims();
                const victim = order.find(function (v) { return !wanted[v] || wanted[v].priority < wanted[key].priority; });
                if (victim === undefined) { waiting += 1; return; }
                delete state[victim];
            }
            state[key] = { t: wanted[key].t, state: "loading", arrive: frame + LATENCY, lastWanted: frame };
            started += 1;
        });

        // drawn: each wanted tile once ready, else its nearest ready ancestor in its place (replace refinement)
        function ready(key) { return state[key] && state[key].state === "ready"; }
        const children = {};
        Object.keys(wanted).forEach(function (key) {
            const p = W.parentKey(wanted[key].t);
            if (p && wanted[p]) (children[p] = children[p] || []).push(key);
        });
        const covered = {};
        function cover(key) {
            const kids = children[key] || [];
            let all = kids.length > 0;
            kids.forEach(function (k) { all = cover(k) && all; });
            covered[key] = ready(key) || all;
            return covered[key];
        }
        const drawnTiles = [];
        const standInTiles = [];
        function draw(key) {
            const kids = children[key] || [];
            if (kids.length === 0) {
                if (ready(key)) drawnTiles.push(state[key].t);
                return;
            }
            const all = kids.every(function (k) { return covered[k]; });
            if (!all && ready(key)) {
                standInTiles.push(state[key].t);
                return;
            }
            kids.forEach(draw);
        }
        cover("0/0/0");
        draw("0/0/0");

        let loading = 0;
        let cached = 0;
        Object.keys(state).forEach(function (key) {
            if (state[key].state === "loading") loading += 1;
            else if (!wanted[key]) cached += 1;
        });
        lastStats = { cam: cam, used: used(), drawn: drawnTiles.length + standInTiles.length, standIns: standInTiles.length, waiting: waiting, wanted: Object.keys(wanted).length,
                      cached: cached, loading: loading, drawnTiles: drawnTiles, standInTiles: standInTiles };
    }

    // Start in flight, not empty: the first 120 frames of the flight.
    for (let k = 0; k < 120; k++) step();

    let clock = 0;
    lab.animate(function (dt) {
        clock += dt;
        while (clock > 0.05) {
            clock -= 0.05;
            step();
        }
    });
    lab.draw(function () {
        const s = lastStats;
        lab.clear(tiles);
        s.drawnTiles.forEach(function (t) { W.rect(lab, tiles, t, "gpu o-75"); });
        s.standInTiles.forEach(function (t) { W.rect(lab, tiles, t, "accent"); });
        Object.keys(state).forEach(function (key) {
            if (state[key].state === "loading") W.rect(lab, tiles, state[key].t, "ghost");
        });
        lab.set(cameraDot, { cx: (W.WORLD.x + s.cam.x).toFixed(1), cy: (W.WORLD.y + s.cam.y).toFixed(1) });

        const unit = MEM.width / budget.value;
        const wantedHeld = s.used - s.cached - s.loading;
        lab.set(memWanted, { width: Math.max(0.5, wantedHeld * unit).toFixed(1) });
        lab.set(memCache, { x: (MEM.x + wantedHeld * unit).toFixed(1), width: Math.max(0.5, s.cached * unit).toFixed(1) });
        lab.set(memLoading, { x: (MEM.x + (wantedHeld + s.cached) * unit).toFixed(1), width: Math.max(0.5, s.loading * unit).toFixed(1) });

        usedOut.set(`${s.used} / ${budget.value}`, s.used <= budget.value ? "ok" : "bad");
        drawnOut.set(`${s.drawn}`, "gpu");
        standOut.set(`${s.standIns}`, s.standIns > 0 ? "mem" : "ok");
        if (s.waiting > 0) note.set(`${s.wanted} tiles are wanted (the cut and its parents), more than the budget holds: ${s.waiting} wait, and their parents stand in. The budget still holds.`);
        else if (s.loading > 0 && s.standIns === 0) note.set(`${s.loading} tile${s.loading > 1 ? "s are" : " is"} on the way; what is drawn is what is in memory.`);
        else if (s.standIns > 0) note.set(`Children on their way: ${s.standIns} parent${s.standIns > 1 ? "s" : ""} drawn in their place until all four are ready.`);
        else note.set(`Everything wanted is in memory; ${s.cached} tiles the camera left are kept as a cache until the room is needed.`, "ok");
    });
});
