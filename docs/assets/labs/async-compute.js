// Labs of docs/guides/async-compute.html.

// The worked example as two timelines to the same scale: one queue, and two queues. The model is the page's:
// the compute pass overlaps the independent graphics pass, which runs a little slower meanwhile, and the main
// draw waits for the compute result through one handoff.
RasmLab.register("queue-overlap", function (lab) {
    const svg = lab.svg(760, 250, "Two timelines of one frame to the same scale. With one queue the compute pass, the shadow map and the main draw run in a row. With two queues the compute pass runs under the shadow map, and the main draw waits for it through a handoff.");
    const cull = lab.slider({ label: "Culling (compute)", min: 0.1, max: 2, step: 0.1, value: 0.4, format: function (v) { return `${v.toFixed(1)} ms`; } });
    const shadow = lab.slider({ label: "Shadow map", min: 0, max: 2, step: 0.1, value: 1.2, format: function (v) { return `${v.toFixed(1)} ms`; } });
    const handoff = lab.slider({ label: "Handoff", min: 0, max: 0.3, step: 0.01, value: 0.05, format: function (v) { return `${v.toFixed(2)} ms`; } });
    const oneOut = lab.readout("one queue");
    const twoOut = lab.readout("two queues");
    const savedOut = lab.readout("saved by the second queue");
    const note = lab.note();

    const MAIN = 2.0;           // the main draw, ms
    const SHARE = 0.875;        // speed of the graphics pass while the compute pass runs next to it
    const X0 = 130;
    const SCALE = 95;           // px per ms
    function tx(ms) { return X0 + ms * SCALE; }

    let grid = "";
    for (let ms = 0; ms <= 6; ms++) {
        grid += `M${tx(ms)},16V214`;
        lab.el(svg, "text", { x: tx(ms), y: 232, class: "mid small", text: ms === 0 ? "0" : `${ms} ms` });
    }
    lab.el(svg, "path", { d: grid, class: "grid" });
    lab.el(svg, "text", { x: 10, y: 48, class: "small", text: "One queue" });
    lab.el(svg, "text", { x: 10, y: 118, class: "small", text: "Two queues" });
    lab.el(svg, "text", { x: 10, y: 136, class: "small t-gpu", text: "graphics" });
    lab.el(svg, "text", { x: 10, y: 170, class: "small t-shader", text: "compute" });
    const bars = lab.el(svg, "g");

    function bar(from, to, y, height, cls, label) {
        const width = (to - from) * SCALE;
        if (width <= 0) return;
        lab.el(bars, "rect", { x: tx(from), y: y, width: width, height: height, rx: Math.min(4, width / 2), class: cls });
        if (label.length * 5.5 + 8 <= width) lab.el(bars, "text", { x: tx(from) + width / 2, y: y + height / 2 + 4, class: "mid small", text: label });
    }

    lab.draw(function () {
        const c = cull.value;
        const s = shadow.value;
        const h = handoff.value;

        // one queue: everything in a row
        const one = c + s + MAIN;

        // two queues: the shadow map runs at SHARE speed while the compute pass runs
        const shadowEnd = s <= SHARE * c ? s / SHARE : c + (s - SHARE * c);
        const ready = Math.max(shadowEnd, c);
        const two = ready + h + MAIN;
        const saved = one - two;

        lab.clear(bars);
        bar(0, c, 30, 28, "shader", "Cull");
        bar(c, c + s, 30, 28, "gpu", "Shadow map");
        bar(c + s, one, 30, 28, "gpu", "Main draw");
        lab.el(bars, "text", { x: tx(one) + 8, y: 49, class: "title", text: `${one.toFixed(2)} ms` });

        bar(0, shadowEnd, 108, 28, "gpu", "Shadow map");
        bar(ready, ready + h, 108, 28, "box", "");
        bar(ready + h, two, 108, 28, "gpu", "Main draw");
        bar(0, c, 154, 22, "shader", "Cull");
        lab.el(bars, "text", { x: tx(Math.max(one, two)) + 8, y: 127, class: saved > 0.005 ? "title t-ok" : "title t-bad", text: `${two.toFixed(2)} ms` });
        // the wait: the main draw needs the compute result
        lab.el(bars, "line", { x1: tx(c), y1: 154, x2: tx(ready + h), y2: 138, class: "arrow hot", "marker-end": "url(#ah-hot)" });
        // where one queue ends, for comparison
        lab.el(bars, "line", { x1: tx(one), y1: 62, x2: tx(one), y2: 200, class: "arrow dashed" });
        if (Math.abs(saved) * SCALE >= 14) {
            lab.el(bars, "line", {
                x1: tx(Math.min(one, two)) + 2, y1: 192, x2: tx(Math.max(one, two)) - 2, y2: 192,
                class: saved > 0 ? "arrow ok" : "arrow bad", "marker-start": saved > 0 ? "url(#ah-ok)" : "url(#ah-bad)", "marker-end": saved > 0 ? "url(#ah-ok)" : "url(#ah-bad)",
            });
        }
        lab.el(bars, "text", { x: tx(Math.max(one, two)) + 8, y: 196, class: saved > 0.005 ? "small t-ok" : "small t-bad", text: saved > 0.005 ? "time saved" : "time lost" });

        oneOut.set(`${one.toFixed(2)} ms`);
        twoOut.set(`${two.toFixed(2)} ms`, saved > 0.005 ? "ok" : "bad");
        savedOut.set(`${saved > 0 ? "" : "−"}${Math.abs(saved).toFixed(2)} ms`, saved > 0.005 ? "ok" : "bad");
        if (s === 0) note.set("A chain: nothing runs next to the compute pass, so the second queue only adds the handoff.", "bad");
        else if (saved > 0.005) note.set(`Two queues win: ${Math.min(c, shadowEnd).toFixed(2)} ms of compute work runs next to the shadow map. The saving is less than that, because the shadow map slows down and the handoff costs time.`, "ok");
        else note.set("Two queues lose: the handoff and the slower shadow map cost as much as the overlap saves, or more.", "bad");
    });
});
