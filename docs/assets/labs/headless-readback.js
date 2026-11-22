// Labs of docs/guides/headless-readback.html.

// A timeline of the CPU and the GPU over eight frames. Frame 2 records a copy; the program takes it at the start of
// a later frame, before that frame's slot wait. The rule is the frame loop's: frame n starts when the CPU is free and
// frame n - N is done on the GPU, and taking the copy first waits for frame 2's fence.
RasmLab.register("readback-timeline", function (lab) {
    const svg = lab.svg(760, 200, "A timeline with a CPU row and a GPU row for eight frames. Frame 2 carries a copy. A red box on the CPU row shows how long taking the copy waits.");
    const takeFrame = lab.slider({ label: "take in frame", min: 2, max: 6, value: 2 });
    const slots = lab.choice({ label: "frames in flight", options: ["1", "2", "3"], value: "2" });
    const gpuMs = lab.slider({ label: "GPU per frame", min: 4, max: 16, value: 10, format: function (v) { return `${v} ms`; } });
    const waitOut = lab.readout("the take waits");
    const lostOut = lab.readout("frame 7 ends later by");
    const note = lab.note();

    const CPU_MS = 6;
    const FRAMES = 8;
    const COPY = 2;
    const X0 = 70;
    const X1 = 740;

    lab.el(svg, "text", { x: 60, y: 49, class: "end small t-cpu", text: "CPU" });
    lab.el(svg, "text", { x: 60, y: 109, class: "end small t-gpu", text: "GPU" });
    const layer = lab.el(svg, "g");

    // take < 0: nobody takes the copy. A take in the copy's own frame comes after its submit, at the frame's end.
    function simulate(n, g, take) {
        const start = [];
        const cpuEnd = [];
        const gpuStart = [];
        const gpuEnd = [];
        const slotWait = [];
        let takeAt = 0;
        let takeWait = 0;
        for (let f = 0; f < FRAMES; f++) {
            let cpuFree = f > 0 ? cpuEnd[f - 1] : 0;
            if (f === take + 1 && take === COPY) {
                takeAt = cpuFree;
                takeWait = Math.max(0, gpuEnd[COPY] - cpuFree);
                cpuFree += takeWait;
            }
            if (f === take && take > COPY) {
                takeAt = cpuFree;
                takeWait = Math.max(0, gpuEnd[COPY] - cpuFree);
                cpuFree += takeWait;
            }
            const slotFree = f >= n ? gpuEnd[f - n] : 0;
            slotWait.push([cpuFree, Math.max(cpuFree, slotFree)]);
            start.push(Math.max(cpuFree, slotFree));
            cpuEnd.push(start[f] + CPU_MS);
            gpuStart.push(Math.max(cpuEnd[f], f > 0 ? gpuEnd[f - 1] : 0));
            gpuEnd.push(gpuStart[f] + g);
        }
        return { start: start, cpuEnd: cpuEnd, gpuStart: gpuStart, gpuEnd: gpuEnd, slotWait: slotWait, takeAt: takeAt, takeWait: takeWait };
    }

    lab.draw(function () {
        const n = Number(slots.value);
        const g = gpuMs.value;
        const take = takeFrame.value;
        const sim = simulate(n, g, take);
        const free = simulate(n, g, -1);
        const end = Math.max(sim.gpuEnd[FRAMES - 1], free.gpuEnd[FRAMES - 1]);
        const px = (X1 - X0) / end;
        function x(t) { return X0 + t * px; }
        function box(from, to, y, cls, label, labelClass) {
            if (to - from < 0.01) return;
            lab.el(layer, "rect", { x: x(from).toFixed(1), y: y, width: ((to - from) * px).toFixed(1), height: 24, rx: 3, class: cls });
            if (label && (to - from) * px >= label.length * 5.5 + 6) {
                lab.el(layer, "text", { x: ((x(from) + x(to)) / 2).toFixed(1), y: y + 16, class: labelClass || "mid small", text: label });
            }
        }

        lab.clear(layer);
        for (let f = 0; f < FRAMES; f++) {
            box(sim.slotWait[f][0], sim.slotWait[f][1], 32, "ghost", "slot", "mid small t-faint");
            box(sim.start[f], sim.cpuEnd[f], 32, f === COPY ? "mem" : "cpu", `F${f}`);
            if (f > 0) box(sim.gpuEnd[f - 1], sim.gpuStart[f], 92, "ghost", "idle", "mid small t-faint");
            box(sim.gpuStart[f], sim.gpuEnd[f], 92, f === COPY ? "mem" : "gpu", f === COPY ? "F2 + copy" : `F${f}`);
        }
        box(sim.takeAt, sim.takeAt + sim.takeWait, 32, "bad", "take waits");

        const done = x(sim.gpuEnd[COPY]);
        lab.el(layer, "line", { x1: done.toFixed(1), y1: 120, x2: done.toFixed(1), y2: 28, class: "arrow hot dashed" });
        lab.el(layer, "text", { x: Math.min(done, X1 - 60).toFixed(1), y: 140, class: "mid small t-hot halo", text: "copy done: F2's fence" });
        const takeX = x(sim.takeAt);
        lab.el(layer, "line", { x1: takeX.toFixed(1), y1: 14, x2: takeX.toFixed(1), y2: 30, class: "arrow cpu", "marker-end": "url(#ah-cpu)" });
        lab.el(layer, "text", { x: Math.max(takeX, X0 + 40).toFixed(1), y: 12, class: "mid small t-cpu", text: "take" });

        for (let ms = 0; ms <= end; ms += 10) {
            const tx = x(ms).toFixed(1);
            lab.el(layer, "line", { x1: tx, y1: 160, x2: tx, y2: 166, class: "arrow" });
            if (ms % 20 === 0) lab.el(layer, "text", { x: tx, y: 180, class: "mid small", text: `${ms}` });
        }
        lab.el(layer, "line", { x1: X0, y1: 160, x2: X1, y2: 160, class: "arrow" });
        lab.el(layer, "text", { x: X1, y: 196, class: "end small", text: "time in ms" });

        const lost = sim.gpuEnd[FRAMES - 1] - free.gpuEnd[FRAMES - 1];
        waitOut.set(`${sim.takeWait} ms`, sim.takeWait > 0 ? "bad" : "ok");
        lostOut.set(`${lost} ms`, lost > 0 ? "bad" : "ok");
        if (sim.takeWait === 0) note.set(`Frame 2's fence was reached before the take, so the pixels are ready and nobody waits. With ${n} frame${n > 1 ? "s" : ""} in flight that is ${take - COPY} frame${take - COPY === 1 ? "" : "s"} after the copy.`, "ok");
        else if (lost === 0) note.set(`The take waits ${sim.takeWait} ms for frame 2, but the frame would have waited for its slot as long anyway: nothing is lost.`, "ok");
        else note.set(`The take waits ${sim.takeWait} ms for the GPU to finish frame 2, and the frames after it start that much later. Take it later, or poll the fence first.`, "bad");
    });
});
