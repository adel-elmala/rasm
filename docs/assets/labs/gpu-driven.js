// Labs of docs/guides/gpu-driven.html.

// The worked example's draw list: 8 records in two runs. The reader hides and shows objects. The lab packs each
// run's visible records at the run's start and writes its count, like the culling step, and compares the calls
// a CPU loop would record with the two indirect calls.
RasmLab.register("indirect-count", function (lab) {
    const svg = lab.svg(760, 354, "Eight draw records in two runs. The visible ones are packed at the start of each run in the output buffer, with one count per run. Two indirect calls read them. Below, the draw calls a CPU loop would record.");
    const drawsOut = lab.readout("draw calls a CPU loop records");
    const callsOut = lab.readout("indirect calls the CPU records");
    const gpuOut = lab.readout("draws the GPU runs");
    const note = lab.note();

    const RUNS = [{ name: "A", first: 0, max: 5 }, { name: "B", first: 5, max: 3 }];
    const visible = [true, false, true, true, false, false, true, true];      // the page's worked example
    function slotX(k) { return (k < 5 ? 18 : 42) + k * 88; }

    lab.el(svg, "text", { x: 10, y: 16, class: "small", text: "the draw list, sorted by state: click a record to hide or show its object" });
    lab.el(svg, "rect", { x: 10, y: 24, width: 448, height: 74, rx: 6, class: "zone" });
    lab.el(svg, "rect", { x: 474, y: 24, width: 272, height: 74, rx: 6, class: "zone" });
    lab.el(svg, "text", { x: 234, y: 40, class: "mid small", text: "run A: records 0 to 4" });
    lab.el(svg, "text", { x: 610, y: 40, class: "mid small", text: "run B: records 5 to 7" });
    const arrows = lab.el(svg, "g");
    const records = visible.map(function (v, k) {
        const g = lab.el(svg, "g");
        const rect = lab.el(g, "rect", { x: slotX(k), y: 48, width: 80, height: 40, rx: 4 });
        const text = lab.el(g, "text", { x: slotX(k) + 40, y: 73, class: "mid" });
        lab.clickable(g, `Hide or show the object of record d${k}`, function () { visible[k] = !visible[k]; });
        return { rect: rect, text: text };
    });

    lab.el(svg, "text", { x: 10, y: 213, class: "small", text: "the output buffer, written by the GPU: each run's visible records, packed at the run's first slot" });
    const slots = visible.map(function (v, k) {
        lab.el(svg, "text", { x: slotX(k) + 40, y: 196, class: "mid mono small t-faint", text: String(k) });
        return {
            rect: lab.el(svg, "rect", { x: slotX(k), y: 140, width: 80, height: 40, rx: 4 }),
            text: lab.el(svg, "text", { x: slotX(k) + 40, y: 165, class: "mid" }),
        };
    });

    const counts = RUNS.map(function (run, r) {
        const x = slotX(run.first);
        const wide = r === 0 ? 150 : 110;
        lab.el(svg, "rect", { x: x, y: 224, width: wide, height: 44, rx: 4, class: "mem" });
        const callX = x + wide + 8;
        const callWide = slotX(run.first + run.max - 1) + 80 - callX;
        lab.el(svg, "rect", { x: callX, y: 224, width: callWide, height: 44, rx: 6, class: "cpu" });
        lab.el(svg, "text", { x: callX + callWide / 2, y: 242, class: "mid title", text: `indirect call ${run.name}` });
        lab.el(svg, "text", { x: callX + callWide / 2, y: 259, class: "mid small", text: `from slot ${run.first}, max ${run.max}` });
        return lab.el(svg, "text", { x: x + wide / 2, y: 251, class: "mid" });
    });

    const loopText = lab.el(svg, "text", { x: 10, y: 300, class: "small" });
    const loop = lab.el(svg, "g");

    lab.draw(function () {
        lab.clear(arrows);
        lab.clear(loop);
        slots.forEach(function (slot) {
            lab.set(slot.rect, { class: "ghost" });
            lab.set(slot.text, { text: "" });
        });
        let total = 0;
        RUNS.forEach(function (run, r) {
            let count = 0;
            for (let k = run.first; k < run.first + run.max; k++) {
                lab.set(records[k].rect, { class: visible[k] ? "gpu" : "ghost" });
                lab.set(records[k].text, { text: visible[k] ? `d${k} ✓` : `d${k} ✗`, class: visible[k] ? "mid" : "mid t-faint" });
                if (!visible[k]) continue;
                const slot = run.first + count;     // packed: the next free slot of this run
                lab.set(slots[slot].rect, { class: "mem" });
                lab.set(slots[slot].text, { text: `d${k}` });
                lab.el(arrows, "line", { x1: slotX(k) + 40, y1: 88, x2: slotX(slot) + 40, y2: 138, class: "arrow mem", "marker-end": "url(#ah-mem)" });
                lab.el(loop, "rect", { x: 18 + total * 88, y: 310, width: 80, height: 30, rx: 4, class: "cpu" });
                lab.el(loop, "text", { x: 58 + total * 88, y: 329, class: "mid small", text: `draw d${k}` });
                count += 1;
                total += 1;
            }
            lab.set(counts[r], { text: `count[${run.name}] = ${count}` });
        });
        lab.set(loopText, { text: total ? "a CPU loop records one draw call per visible object, with its binds:" : "a CPU loop records no draw call: nothing is visible" });

        drawsOut.set(String(total), "cpu");
        callsOut.set(String(RUNS.length), "ok");
        gpuOut.set(String(total), "gpu");
        if (total === 0) note.set("Nothing is visible: both counts are 0. The 2 indirect calls are still recorded, and they draw nothing.");
        else if (total === visible.length) note.set(`Everything is visible: the CPU loop records ${total} draw calls. The indirect path still records 2 calls; each count is at its maximum.`);
        else note.set(`${total} of ${visible.length} objects are visible: the CPU loop records ${total} draw calls. The indirect path records 2 calls, and the GPU reads the counts to stop at the right record.`);
    });
});
