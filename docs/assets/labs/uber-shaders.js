// Labs of docs/guides/uber-shaders.html.

// The page's eight material features. The reader removes features and puts them back; the lab multiplies their
// choices into the number of compile-time variants and draws one cell per variant, next to the one uber shader.
RasmLab.register("permutations", function (lab) {
    const svg = lab.svg(760, 306, "Eight material features that can be switched on and off. Below, a grid with one cell per shader combination, and one single cell for the uber shader.");
    const perPassOut = lab.readout("variants to compile, per pass");
    const allOut = lab.readout("with 3 passes (depth only, shadow, color)");
    const uberOut = lab.readout("uber shader: shaders, and tests at run time");
    const note = lab.note();

    const FEATURES = [
        { lines: ["base color", "texture"], choices: 2 },
        { lines: ["metal-rough", "texture"], choices: 2 },
        { lines: ["normal", "map"], choices: 2 },
        { lines: ["occlusion", "map"], choices: 2 },
        { lines: ["emissive", "map"], choices: 2 },
        { lines: ["alpha", "mode"], choices: 3 },
        { lines: ["double-", "sided"], choices: 2 },
        { lines: ["shading", "model"], choices: 2 },
    ];
    const COLUMNS = 48;
    const CELL = 14;

    lab.el(svg, "text", { x: 20, y: 16, class: "small", text: "the material model: click a feature to remove it, or to put it back" });
    const cards = FEATURES.map(function (feature, k) {
        feature.on = true;
        const x = 20 + k * 90;
        const g = lab.el(svg, "g");
        const rect = lab.el(g, "rect", { x: x, y: 26, width: 78, height: 70, rx: 6 });
        const name = [
            lab.el(g, "text", { x: x + 39, y: 46, text: feature.lines[0] }),
            lab.el(g, "text", { x: x + 39, y: 60, text: feature.lines[1] }),
        ];
        const factor = lab.el(g, "text", { x: x + 39, y: 84 });
        lab.clickable(g, `Feature: ${feature.lines.join(" ")}`, function () { feature.on = !feature.on; });
        return { rect: rect, name: name, factor: factor };
    });
    const product = lab.el(svg, "text", { x: 380, y: 122, class: "mid title" });
    lab.el(svg, "rect", { x: 44, y: 134, width: COLUMNS * CELL, height: 8 * CELL, class: "zone" });
    const grid = lab.el(svg, "path", { class: "shader thin" });
    lab.el(svg, "rect", { x: 44, y: 266, width: CELL, height: CELL, class: "accent" });
    const uberText = lab.el(svg, "text", { x: 68, y: 278 });
    lab.el(svg, "text", { x: 44, y: 298, class: "small", text: "Each purple cell is one shader to compile, and one pipeline. The orange cell is the uber shader." });

    lab.draw(function () {
        let variants = 1;
        let tests = 0;
        const factors = [];
        FEATURES.forEach(function (feature, k) {
            const on = feature.on;
            lab.set(cards[k].rect, { class: on ? "shader" : "ghost" });
            cards[k].name.forEach(function (line) { lab.set(line, { class: on ? "mid small" : "mid small t-faint" }); });
            lab.set(cards[k].factor, { class: on ? "mid title" : "mid title t-faint", text: on ? `× ${feature.choices}` : "off" });
            if (!on) return;
            variants *= feature.choices;
            tests += 1;
            factors.push(feature.choices);
        });
        lab.set(product, { text: factors.length ? `${factors.join(" × ")} = ${variants} combinations per pass` : "no features: 1 shader per pass" });
        let d = "";
        for (let k = 0; k < variants; k++) d += `M${44 + (k % COLUMNS) * CELL},${134 + Math.floor(k / COLUMNS) * CELL}h${CELL}v${CELL}h${-CELL}z`;
        lab.set(grid, { d: d });
        lab.set(uberText, { text: `1 uber shader, 1 pipeline, with ${tests} ${tests === 1 ? "test" : "tests"} at run time` });

        perPassOut.set(variants.toLocaleString("en-US"), variants > 1 ? "bad" : "ok");
        allOut.set((3 * variants).toLocaleString("en-US"), variants > 1 ? "bad" : "ok");
        uberOut.set(`1, ${tests}`, "ok");
        if (tests === 0) note.set("No features: both ways need one shader.");
        else if (tests === FEATURES.length) note.set(`All ${tests} features: ${variants} variants per pass against one uber shader with ${tests} tests. One more on/off feature doubles the grid, and adds one test.`);
        else note.set(`${tests} features: ${variants} variants per pass against one uber shader with ${tests} ${tests === 1 ? "test" : "tests"}. Each feature you put back multiplies the grid, and adds one test.`);
    });
});

// One wave of 8 lanes at an if. Side A costs 100 steps and side B 300, as in the page's picture. The reader
// sets the side each lane takes; lanes that disagree make the wave run both sides.
RasmLab.register("wave-branch", function (lab) {
    const svg = lab.svg(760, 256, "A wave of eight lanes on a timeline. Each lane takes side A or side B. When the lanes disagree, the wave runs side A and then side B, with some lanes switched off in each part.");
    const LANES = 8;
    const COST = { A: 100, B: 300 };
    const takes = ["A", "A", "A", "B", "B", "B", "B", "B"];      // the page's picture: 3 lanes A, 5 lanes B
    lab.button("All lanes: A", function () { takes.fill("A"); });
    lab.button("All lanes: B", function () { takes.fill("B"); });
    const splitOut = lab.readout("lanes that take A / B");
    const stepsOut = lab.readout("steps the wave spends");
    const idleOut = lab.readout("lane-steps spent switched off");
    const note = lab.note();

    lab.el(svg, "text", { x: 20, y: 18, class: "small", text: "click a lane to change the side it takes · time goes from left to right" });
    const chips = takes.map(function (side, k) {
        const y = 32 + k * 22;
        const g = lab.el(svg, "g");
        const rect = lab.el(g, "rect", { x: 20, y: y, width: 124, height: 18, rx: 4, class: "box" });
        const text = lab.el(g, "text", { x: 82, y: y + 13, class: "mid small" });
        lab.clickable(g, `Lane ${k}: change its side`, function () { takes[k] = takes[k] === "A" ? "B" : "A"; });
        return { rect: rect, text: text };
    });
    const bars = lab.el(svg, "g");
    const labelA = lab.el(svg, "text", { y: 226, class: "mid small" });
    const labelB = lab.el(svg, "text", { y: 226, class: "mid small" });
    const total = lab.el(svg, "text", { x: 590, y: 114, class: "title" });
    const totalWhy = lab.el(svg, "text", { x: 590, y: 132, class: "small" });
    lab.el(svg, "line", { x1: 170, y1: 238, x2: 580, y2: 238, class: "arrow", "marker-end": "url(#ah)" });
    lab.el(svg, "text", { x: 170, y: 252, class: "mono small t-faint", text: "0" });

    lab.draw(function () {
        const onA = takes.filter(function (side) { return side === "A"; }).length;
        const onB = LANES - onA;
        const runs = [];                // the sides the wave runs, in order
        if (onA) runs.push("A");
        if (onB) runs.push("B");
        const divergent = runs.length === 2;

        lab.clear(bars);
        let x = 170;
        let idle = 0;
        runs.forEach(function (side) {
            takes.forEach(function (lane, k) {
                lab.el(bars, "rect", { x: x, y: 34 + k * 22, width: COST[side], height: 14, class: lane === side ? "gpu" : "ghost" });
                if (lane !== side) idle += COST[side];
            });
            const off = side === "A" ? onB : onA;
            lab.set(side === "A" ? labelA : labelB, { x: x + COST[side] / 2, text: off ? `${side}, ${off} ${off === 1 ? "lane" : "lanes"} off` : side });
            x += COST[side];
        });
        if (!onA) lab.set(labelA, { text: "" });
        if (!onB) lab.set(labelB, { text: "" });
        takes.forEach(function (side, k) {
            lab.set(chips[k].text, { text: `lane ${k}: takes ${side}` });
        });

        const steps = x - 170;
        lab.set(total, { text: `${steps} steps`, class: divergent ? "title t-bad" : "title t-ok" });
        lab.set(totalWhy, { text: divergent ? "both sides, for every lane" : "one side only" });
        splitOut.set(`${onA} / ${onB}`);
        stepsOut.set(String(steps), divergent ? "bad" : "ok");
        idleOut.set(String(idle), divergent ? "bad" : "ok");
        if (!divergent) note.set(`Uniform: every lane takes ${runs[0]}, so the wave runs only side ${runs[0]}: ${steps} steps. Only the test itself is extra.`, "ok");
        else if (onA === 1 || onB === 1) note.set(`Divergent: one lane disagrees, and that is enough. The wave runs A, then B: ${COST.A} + ${COST.B} = ${steps} steps.`, "bad");
        else note.set(`Divergent: the lanes disagree, so the wave runs A, then B: ${COST.A} + ${COST.B} = ${steps} steps. In each part, the other lanes wait.`, "bad");
    });
});
