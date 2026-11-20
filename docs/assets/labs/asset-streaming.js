// Labs of docs/guides/asset-streaming.html.

// 30 textures are requested at once. The whole run is computed up front, frame by frame: workers take jobs
// from a queue in order, and each frame the main thread finishes ready results under the chosen budget.
// The picture then plays that run back: the state of every texture, and the time of every frame.
RasmLab.register("load-budget", function (lab) {
    const svg = lab.svg(760, 290, "Left: 30 textures as squares, colored by their state. Right: a bar for each frame, as tall as the time the frame takes, against the 16.7 millisecond line.");
    const mode = lab.choice({
        label: "Load",
        options: [
            { value: "main", label: "on the main thread" },
            { value: "workers", label: "workers, no budget" },
            { value: "budget", label: "workers + budget" },
        ],
        value: "budget",
    });
    const workers = lab.slider({ label: "Workers", min: 1, max: 8, value: 4 });
    const budget = lab.slider({ label: "Budget", min: 1, max: 12, value: 4, format: function (v) { return `${v} ms`; } });
    const longestOut = lab.readout("the longest frame");
    const missedOut = lab.readout("frames longer than 16.7 ms");
    const doneOut = lab.readout("all 30 in the scene after");
    const note = lab.note();

    const COUNT = 30;
    const DECODE = 100;         // ms to read and decode one texture
    const FINISH = 3;           // ms on the main thread to create it, copy it and build its mips
    const WORK = 10;            // ms of a frame's own work
    const FRAME = 1000 / 60;
    const REQUEST = 100;        // the textures are requested here
    const SPEED = 0.4;          // the picture runs slower than real time

    // ---- the run: { items: [{ start, ready, done }], frames: [{ start, load }], ... }
    function simulate() {
        const items = [];
        for (let i = 0; i < COUNT; i++) items.push({ start: Infinity, ready: Infinity, done: Infinity });
        const frames = [];
        let t = 0;
        let requested = false;
        let left = COUNT;
        let tail = 0;
        let mostPerFrame = 0;
        while (tail < 40) {
            let load = 0;
            if (!requested && t >= REQUEST) {
                requested = true;
                if (mode.value === "main") {
                    // read, decode and finish every texture right here
                    items.forEach(function (item) {
                        item.start = t + load;
                        load += DECODE + FINISH;
                        item.ready = item.done = t + load;
                    });
                    left = 0;
                } else {
                    // each job goes to the worker that is free first
                    const free = [];
                    for (let w = 0; w < workers.value; w++) free.push(t);
                    items.forEach(function (item) {
                        let w = 0;
                        for (let k = 1; k < free.length; k++) if (free[k] < free[w]) w = k;
                        item.start = free[w];
                        item.ready = free[w] = item.start + DECODE;
                    });
                }
            }
            let taken = 0;
            for (let i = 0; i < COUNT && left > 0; i++) {
                const item = items[i];
                if (item.done < Infinity || item.ready > t) continue;
                if (mode.value === "budget" && load >= budget.value) break;      // asked before each result
                load += FINISH;
                item.done = t + load;
                left -= 1;
                taken += 1;
            }
            mostPerFrame = Math.max(mostPerFrame, taken);
            frames.push({ start: t, load: load });
            if (left === 0) tail += 1;
            t += Math.max(1, Math.ceil((WORK + load) / FRAME - 1e-9)) * FRAME;
        }
        let longest = 0;
        let missed = 0;
        frames.forEach(function (frame) {
            longest = Math.max(longest, WORK + frame.load);
            if (WORK + frame.load > FRAME) missed += 1;
        });
        let last = 0;
        items.forEach(function (item) { last = Math.max(last, item.done); });
        return { items: items, frames: frames, end: t, longest: longest, missed: missed, loaded: last - REQUEST, mostPerFrame: mostPerFrame };
    }

    // ---- the picture
    lab.el(svg, "text", { x: 20, y: 22, class: "title", text: "30 textures" });
    const squares = [];
    for (let i = 0; i < COUNT; i++) {
        squares.push(lab.el(svg, "rect", { x: 20 + (i % 6) * 36, y: 38 + Math.floor(i / 6) * 36, width: 30, height: 30, rx: 4 }));
    }
    const sceneText = lab.el(svg, "text", { x: 20, y: 240, class: "small" });
    const clockText = lab.el(svg, "text", { x: 20, y: 258, class: "small" });

    const CHART = { x: 318, y: 40, width: 424, height: 200, top: 55, step: 8 };
    function barY(ms) { return CHART.y + CHART.height - Math.min(ms, CHART.top) / CHART.top * CHART.height; }
    const SHOWN = Math.floor(CHART.width / CHART.step);
    lab.el(svg, "text", { x: CHART.x, y: 22, class: "title", text: "The time of each frame" });
    lab.el(svg, "rect", { x: CHART.x, y: CHART.y, width: CHART.width, height: CHART.height, class: "zone" });
    lab.el(svg, "text", { x: CHART.x - 8, y: barY(0) + 4, class: "end small", text: "0" });
    lab.el(svg, "text", { x: CHART.x - 8, y: barY(FRAME) + 4, class: "end small t-hot", text: "16.7" });
    lab.el(svg, "text", { x: CHART.x - 8, y: barY(50) + 4, class: "end small", text: "50 ms" });
    lab.el(svg, "text", { x: CHART.x, y: 258, class: "small", text: "one bar per frame, oldest on the left" });
    const bars = lab.el(svg, "g");
    lab.el(svg, "path", { d: `M${CHART.x},${barY(FRAME).toFixed(1)}H${CHART.x + CHART.width}`, class: "arrow hot dashed" });
    lab.el(svg, "text", { x: CHART.x + CHART.width - 6, y: barY(FRAME) - 6, class: "end small t-hot halo", text: "60 Hz: a frame has 16.7 ms" });
    const overText = lab.el(svg, "text", { y: CHART.y + 16, class: "small t-bad halo" });

    let run = null;
    let key = "";
    let clock = 0;
    lab.animate(function (dt) {
        clock += dt * 1000 * SPEED;
        if (run && clock > run.end) clock = 0;
    });

    function seconds(ms) {
        return ms < 1000 ? `${ms.toFixed(0)} ms` : `${(ms / 1000).toFixed(1)} s`;
    }

    lab.draw(function () {
        const now = `${mode.value}/${workers.value}/${budget.value}`;
        if (now !== key) {
            key = now;
            run = simulate();
            clock = 0;
        }

        let inScene = 0;
        run.items.forEach(function (item, i) {
            let cls = "box";
            if (clock >= item.done) { cls = "gpu"; inScene += 1; }
            else if (clock >= item.ready) cls = "mem";
            else if (clock >= item.start) cls = mode.value === "main" ? "bad" : "cpu";
            lab.set(squares[i], { class: cls });
        });
        lab.set(sceneText, { text: `in the scene: ${inScene} / ${COUNT}` });
        lab.set(clockText, { text: `clock: ${(clock / 1000).toFixed(2)} s` });

        // the frames that have started, the newest on the right
        let count = 0;
        while (count < run.frames.length && run.frames[count].start <= clock) count += 1;
        const first = Math.max(0, count - SHOWN);
        lab.clear(bars);
        let over = null;
        for (let k = first; k < count; k++) {
            const frame = run.frames[k];
            const spent = Math.min(WORK + frame.load, clock - frame.start);     // the newest frame is still running
            const loadSpent = Math.min(frame.load, spent);
            const x = CHART.x + (k - first) * CHART.step + 1;
            const late = WORK + frame.load > FRAME;
            if (loadSpent > 0) {
                lab.el(bars, "rect", { x: x, y: barY(loadSpent), width: CHART.step - 2, height: barY(0) - barY(loadSpent), class: late ? "bad" : "accent" });
            }
            if (spent > loadSpent) {
                lab.el(bars, "rect", { x: x, y: barY(spent), width: CHART.step - 2, height: barY(loadSpent) - barY(spent), class: "cpu" });
            }
            if (spent > CHART.top) over = { x: x, ms: spent };
        }
        if (over) lab.set(overText, { x: over.x + CHART.step + 4, text: `${seconds(over.ms)}: off the chart`, class: "small t-bad halo" });
        else lab.set(overText, { text: "" });

        const late = run.longest > FRAME + 1e-6;
        longestOut.set(seconds(run.longest), late ? "bad" : "ok");
        missedOut.set(String(run.missed), run.missed ? "bad" : "ok");
        doneOut.set(seconds(run.loaded));
        const n = run.mostPerFrame;
        if (mode.value === "main") note.set(`Frozen: one frame does all the reading and decoding, 30 × ${DECODE + FINISH} ms. No new frame for ${seconds(run.longest)}.`, "bad");
        else if (mode.value === "workers" && late) note.set(`A hitch: up to ${n} results are ready in the same frame, and the main thread finishes them all. ${n} × ${FINISH} ms plus the frame's own ${WORK} ms is over 16.7 ms.`, "bad");
        else if (mode.value === "workers") note.set(`On time, by luck: with ${workers.value} ${workers.value === 1 ? "worker" : "workers"}, at most ${n} results are ready in the same frame. Add workers and see.`, "ok");
        else if (late) note.set(`The budget is too large: ${n} textures start before ${budget.value} ms are used. ${n} × ${FINISH} ms plus the frame's own ${WORK} ms is over 16.7 ms.`, "bad");
        else note.set(`Every frame is on time: at most ${n} ${n === 1 ? "texture" : "textures"} per frame, ${n * FINISH} ms. The rest wait, ready, for the next frame.`, "ok");
    });
});
