// Labs of docs/guides/frames-in-flight.html.

// A scrolling timeline of the CPU, the queue and the GPU. It steps the rule of section 7: frame n may start when
// the CPU is free and frame n - N is done on the GPU.
RasmLab.register("slots-timeline", function (lab) {
    const svg = lab.svg(760, 214, "A timeline with three rows: the CPU records frames, recorded frames wait in a queue, the GPU runs them. An arrow shows the latency of one frame.");
    const cpuMs = lab.slider({ label: "CPU per frame", min: 2, max: 16, value: 6, format: function (v) { return `${v} ms`; } });
    const gpuMs = lab.slider({ label: "GPU per frame", min: 2, max: 16, value: 10, format: function (v) { return `${v} ms`; } });
    const slotCount = lab.slider({ label: "Slots", min: 1, max: 4, value: 2 });
    const frameOut = lab.readout("time per frame");
    const fpsOut = lab.readout("frames per second");
    const latencyOut = lab.readout("latency: CPU start to GPU end");
    const note = lab.note();

    const X0 = 80;
    const X1 = 740;
    const PX = 10;              // pixels per millisecond, like the page's figures
    const BEFORE = 22;          // milliseconds of the past that stay in view
    const FRAMES = 400;
    const SPEED = 8;            // simulated milliseconds per second
    let now = 200;

    lab.el(svg, "text", { x: 70, y: 45, class: "end small t-cpu", text: "CPU" });
    lab.el(svg, "text", { x: 70, y: 77, class: "end small", text: "queue" });
    lab.el(svg, "text", { x: 70, y: 109, class: "end small t-gpu", text: "GPU" });
    lab.el(svg, "line", { x1: X0, y1: 176, x2: X1, y2: 176, class: "arrow" });
    const playX = X0 + BEFORE * PX;
    lab.el(svg, "line", { x1: playX, y1: 20, x2: playX, y2: 176, class: "arrow dashed" });
    const moving = lab.el(svg, "g");
    lab.el(svg, "text", { x: playX, y: 14, class: "mid small", text: "now" });

    let key = "";
    let sim = null;
    function simulate(c, g, n) {
        const start = [];
        const cpuEnd = [];
        const gpuStart = [];
        const gpuEnd = [];
        for (let f = 0; f < FRAMES; f++) {
            const cpuFree = f > 0 ? cpuEnd[f - 1] : 0;
            const slotFree = f >= n ? gpuEnd[f - n] : 0;
            start.push(Math.max(cpuFree, slotFree));
            cpuEnd.push(start[f] + c);
            gpuStart.push(Math.max(cpuEnd[f], f > 0 ? gpuEnd[f - 1] : 0));
            gpuEnd.push(gpuStart[f] + g);
        }
        return { start: start, cpuEnd: cpuEnd, gpuStart: gpuStart, gpuEnd: gpuEnd };
    }

    // one box of a row, cut at the edges of the view
    function box(from, to, y, cls, label, labelClass) {
        const x0 = Math.max(X0, playX + (from - now) * PX);
        const x1 = Math.min(X1, playX + (to - now) * PX);
        if (x1 - x0 < 1) return;
        lab.el(moving, "rect", { x: x0.toFixed(1), y: y, width: (x1 - x0).toFixed(1), height: 22, rx: 3, class: cls });
        if (label && x1 - x0 >= label.length * 5.5 + 8) {
            lab.el(moving, "text", { x: ((x0 + x1) / 2).toFixed(1), y: y + 15, class: labelClass || "mid small", text: label });
        }
    }

    lab.animate(function (dt) {
        now += dt * SPEED;
    });

    lab.draw(function () {
        const c = cpuMs.value;
        const g = gpuMs.value;
        const n = slotCount.value;
        if (key !== `${c}/${g}/${n}`) {
            key = `${c}/${g}/${n}`;
            sim = simulate(c, g, n);
        }
        if (now > sim.gpuEnd[FRAMES - 40]) now = sim.start[20];

        lab.clear(moving);
        const from = now - BEFORE;
        const to = now + (X1 - playX) / PX;
        let running = 0;        // the newest frame the GPU has started
        for (let f = 0; f < FRAMES; f++) {
            if (sim.start[f] > to) break;
            if (sim.gpuStart[f] <= now) running = f;
            if (sim.gpuEnd[f] < from) continue;
            const name = `F${f}`;
            if (f > 0 && sim.start[f] - sim.cpuEnd[f - 1] > 0.01) box(sim.cpuEnd[f - 1], sim.start[f], 30, "ghost", "waits", "mid small t-faint");
            box(sim.start[f], sim.cpuEnd[f], 30, "cpu", name);
            if (f > 0 && sim.gpuStart[f] - sim.gpuEnd[f - 1] > 0.01) box(sim.gpuEnd[f - 1], sim.gpuStart[f], 94, "ghost", "idle", "mid small t-faint");
            box(sim.gpuStart[f], sim.gpuEnd[f], 94, "gpu", name);
        }

        // the queue: the frames that are recorded and not yet on the GPU, between every two events
        const times = [];
        for (let f = 0; f < FRAMES && sim.start[f] <= to; f++) {
            if (sim.gpuStart[f] >= from) times.push(sim.cpuEnd[f], sim.gpuStart[f]);
        }
        times.sort(function (p, q) { return p - q; });
        let first = 0;
        for (let k = 0; k + 1 < times.length; k++) {
            if (times[k + 1] - times[k] < 0.01) continue;
            while (sim.gpuStart[first] <= times[k] + 0.001) first += 1;
            let names = "";
            let count = 0;
            for (let f = first; sim.cpuEnd[f] <= times[k] + 0.001; f++) {
                names += `${count ? " " : ""}F${f}`;
                count += 1;
            }
            if (count) box(times[k], times[k + 1], 62, "accent", names);
        }

        // the latency of the frame the GPU runs now
        const a = playX + (sim.start[running] - now) * PX;
        const b = playX + (sim.gpuEnd[running] - now) * PX;
        const a1 = Math.max(X0, a);
        const b1 = Math.min(X1, b);
        if (b1 - a1 > 4) {
            lab.el(moving, "line", {
                x1: a1.toFixed(1), y1: 132, x2: b1.toFixed(1), y2: 132, class: "arrow hot",
                "marker-start": a >= X0 ? "url(#ah-hot)" : null, "marker-end": b <= X1 ? "url(#ah-hot)" : null,
            });
            const mid = lab.clamp((a1 + b1) / 2, X0 + 70, X1 - 70);
            lab.el(moving, "text", { x: mid.toFixed(1), y: 150, class: "mid small t-hot halo", text: `latency of F${running}: ${sim.gpuEnd[running] - sim.start[running]} ms` });
        }

        for (let ms = Math.ceil(from / 10) * 10; ms <= to; ms += 10) {
            if (ms < 0) continue;
            const x = (playX + (ms - now) * PX).toFixed(1);
            lab.el(moving, "line", { x1: x, y1: 172, x2: x, y2: 180, class: "arrow" });
            lab.el(moving, "text", { x: x, y: 196, class: "mid small", text: `${ms}` });
        }
        lab.el(moving, "text", { x: X1, y: 210, class: "end small", text: "time in ms" });

        const late = FRAMES - 100;
        const perFrame = (sim.gpuEnd[late + 50] - sim.gpuEnd[late]) / 50;
        const latency = sim.gpuEnd[late] - sim.start[late];
        frameOut.set(`${perFrame} ms`);
        fpsOut.set(1000 % perFrame === 0 ? String(1000 / perFrame) : (1000 / perFrame).toFixed(1));
        latencyOut.set(`${latency} ms`, g > c && n > 2 ? "bad" : "");
        if (n === 1) note.set(`One slot: the CPU waits for the GPU in every frame, so they never work at the same time. A frame takes CPU + GPU = ${c + g} ms.`, "bad");
        else if (g > c && n === 2) note.set(`GPU-bound: the CPU runs ahead, then waits for its slot. A frame takes ${g} ms, and the latency is 2 × ${g} ms.`, "ok");
        else if (g > c) note.set(`GPU-bound with ${n} slots: no faster than with 2, but each frame waits longer in the queue. The latency is ${n} × ${g} ms.`, "bad");
        else note.set(`CPU-bound: the GPU waits for the CPU and the queue stays empty. More slots change nothing: the latency stays CPU + GPU = ${c + g} ms.`, "ok");
    });
});
