// Labs of docs/guides/job-system.html.

// 24 images are asked for at once. The run is computed up front in steps of 1 ms: free workers take the next job
// from a FIFO queue when its bytes fit the budget next to what is held (or nothing is held), a finished job's
// result waits, and every frame the main thread uploads waiting results in order, up to 48 MiB (at least one),
// which gives their bytes back. The picture plays the run back: a lane per worker, and the memory held over time.
RasmLab.register("pool-budget", function (lab) {
    const svg = lab.svg(760, 330, "Top: a lane per worker with the jobs it ran, colored by size. Bottom: the memory held over time, against the budget line.");
    const workers = lab.slider({ label: "Workers", min: 1, max: 8, value: 4 });
    const budget = lab.slider({ label: "Budget", min: 32, max: 512, step: 16, value: 192, format: function (v) { return `${v} MiB`; } });
    const useBudget = lab.toggle({ label: "budget on", value: true });
    const doneOut = lab.readout("all 24 uploaded after");
    const peakOut = lab.readout("most memory held");
    const idleOut = lab.readout("workers idle while jobs wait");
    const note = lab.note();

    const COUNT = 24;
    const FRAME = 1000 / 60;
    const UPLOAD = 32;            // MiB the main thread uploads per frame
    const MS_PER_MIB = 1.5;       // decode time: 1.5 ms per MiB of result

    // Sizes from a fixed seed: 8 to 64 MiB, like 1024² to 2048² images with their mips (and the decode buffer).
    let seed = 7;
    function random() {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        return seed / 2147483648;
    }
    const sizes = [];
    for (let i = 0; i < COUNT; i++) sizes.push(8 + Math.round(random() * 7) * 8);

    function simulate() {
        const n = workers.value;
        const limit = useBudget.value ? budget.value : Infinity;
        const jobs = sizes.map(function (size) { return { size: size, worker: -1, start: Infinity, end: Infinity, uploaded: Infinity }; });
        const busyUntil = [];
        for (let w = 0; w < n; w++) busyUntil.push(0);
        let next = 0;
        let held = 0;
        let peak = 0;
        let nextFrame = FRAME;
        let uploadedCount = 0;
        const memory = [];
        let t = 0;
        let idle = 0;
        let allStarted = 1;
        while (uploadedCount < COUNT && t < 20000) {
            // results of jobs that end now wait for the main thread (their bytes stay held)
            // the main thread uploads at each frame, oldest first
            if (t >= nextFrame) {
                let moved = 0;
                for (let i = 0; i < COUNT; i++) {
                    const job = jobs[i];
                    if (job.end > t || job.uploaded < Infinity) continue;
                    if (moved > 0 && moved + job.size > UPLOAD) break;
                    moved += job.size;
                    job.uploaded = t;
                    held -= job.size;
                    uploadedCount += 1;
                }
                nextFrame += FRAME;
            }
            // free workers take the front of the queue while it fits
            for (let w = 0; w < n && next < COUNT; w++) {
                if (busyUntil[w] > t) continue;
                const job = jobs[next];
                if (held > 0 && held + job.size > limit) break;
                job.worker = w;
                job.start = t;
                job.end = t + job.size * MS_PER_MIB;
                busyUntil[w] = job.end;
                held += job.size;
                next += 1;
            }
            if (next < COUNT) {
                for (let w = 0; w < n; w++) if (busyUntil[w] <= t) idle += 1;
                allStarted = t + 1;
            }
            peak = Math.max(peak, held);
            memory.push(held);
            t += 1;
        }
        return { jobs: jobs, memory: memory, end: t, peak: peak, idle: idle / (n * allStarted), limit: limit };
    }

    // ---- the picture
    const LANES = { x: 90, y: 30, width: 650, height: 150 };
    const MEM = { x: 90, y: 210, width: 650, height: 90 };
    lab.el(svg, "text", { x: 20, y: 20, class: "title", text: "Workers" });
    lab.el(svg, "text", { x: 20, y: 200, class: "title", text: "Memory held" });
    lab.el(svg, "rect", { x: LANES.x, y: LANES.y, width: LANES.width, height: LANES.height, class: "zone" });
    lab.el(svg, "rect", { x: MEM.x, y: MEM.y, width: MEM.width, height: MEM.height, class: "zone" });
    const laneLabels = lab.el(svg, "g");
    const bars = lab.el(svg, "g");
    const curve = lab.el(svg, "path", { class: "arrow mem thick" });
    const budgetLine = lab.el(svg, "path", { class: "arrow hot dashed" });
    const budgetText = lab.el(svg, "text", { class: "end small t-hot halo" });
    const peakText = lab.el(svg, "text", { x: MEM.x - 8, class: "end small" });
    lab.el(svg, "text", { x: MEM.x - 8, y: MEM.y + MEM.height + 4, class: "end small", text: "0" });
    const timeText = lab.el(svg, "text", { x: MEM.x + MEM.width, y: 320, class: "end small" });
    lab.el(svg, "text", { x: MEM.x, y: 320, class: "small", text: "0 ms" });

    lab.draw(function () {
        const run = simulate();
        const n = workers.value;
        const span = Math.max(run.end, 1);
        const xAt = function (ms) { return LANES.x + ms / span * LANES.width; };
        lab.clear(laneLabels);
        lab.clear(bars);
        const laneHeight = LANES.height / n;
        for (let w = 0; w < n; w++) {
            lab.el(laneLabels, "text", { x: LANES.x - 8, y: LANES.y + (w + 0.5) * laneHeight + 4, class: "end small", text: `worker ${w + 1}` });
        }
        run.jobs.forEach(function (job, i) {
            if (job.worker < 0) return;
            const x = xAt(job.start);
            const width = Math.max(1, xAt(job.end) - x - 1);
            const y = LANES.y + job.worker * laneHeight + 2;
            lab.el(bars, "rect", { x: x.toFixed(1), y: y.toFixed(1), width: width.toFixed(1), height: Math.max(2, laneHeight - 4).toFixed(1), rx: 2, class: job.size >= 48 ? "cpu" : "cpu o-50" });
            if (width > 18 && laneHeight > 14) {
                lab.el(bars, "text", { x: (x + width / 2).toFixed(1), y: (y + laneHeight / 2 + 2).toFixed(1), class: "mid small", text: `${i + 1}` });
            }
        });

        const top = Math.max(run.peak, useBudget.value ? budget.value : 0, 64) * 1.1;
        const yAt = function (mib) { return MEM.y + MEM.height - mib / top * MEM.height; };
        let d = "";
        const step = Math.max(1, Math.floor(run.memory.length / 650));
        for (let i = 0; i < run.memory.length; i += step) {
            d += `${i === 0 ? "M" : "L"}${xAt(i).toFixed(1)},${yAt(run.memory[i]).toFixed(1)}`;
        }
        lab.set(curve, { d: d });
        if (useBudget.value) {
            const y = yAt(budget.value).toFixed(1);
            lab.set(budgetLine, { d: `M${MEM.x},${y}H${MEM.x + MEM.width}` });
            lab.set(budgetText, { x: MEM.x + MEM.width - 6, y: Number(y) - 5, text: `budget ${budget.value} MiB` });
        } else {
            lab.set(budgetLine, { d: "" });
            lab.set(budgetText, { text: "" });
        }
        lab.set(peakText, { y: yAt(run.peak) + 4, text: `${run.peak}` });
        lab.set(timeText, { text: `${Math.round(run.end)} ms` });

        doneOut.set(`${Math.round(run.end)} ms`);
        const over = useBudget.value && run.peak > budget.value;
        peakOut.set(`${run.peak} MiB`, over ? "bad" : run.peak <= 128 ? "ok" : "");
        idleOut.set(`${Math.round(run.idle * 100)} %`, run.idle > 0.5 ? "bad" : "");
        if (!useBudget.value) {
            note.set(`No budget: every worker starts its next image at once, and results pile up while the main thread uploads ${UPLOAD} MiB a frame.`, "bad");
        } else if (over) {
            note.set("One image is bigger than what is left of the budget with nothing else held, so it runs alone, over the line.", "bad");
        } else if (run.idle > 0.5) {
            note.set("The budget is so small that workers wait for the main thread most of the time: memory is low, but the uploads end late.", "bad");
        } else {
            note.set("The memory stays under the budget: a job that does not fit waits in the queue, and starts once an upload gives bytes back.", "ok");
        }
    });
});
