// Labs of docs/guides/shader-warmup.html.

// A run of 1 s with a cold cache. The program knows 14 shaders up front and queues them at time 0, in the order
// the frames will ask for them. Workers take the front of the queue; a worker's first compile also makes its own
// compiler session (60 ms), which holds 100 MB until the queue is empty. Frames run every 16 ms. A frame that asks
// for a shader: ready, it draws; a worker compiles it, it waits for that one; still queued, the main thread takes it
// off the queue and compiles it itself. With 0 workers nothing is queued: each shader compiles in the frame that
// first asks for it, as without a warm-up.
RasmLab.register("warmup-queue", function (lab) {
    const svg = lab.svg(760, 300, "Top: the main thread's frames over time, with the time each one waited for a shader. Below: a lane per worker with the shaders it compiled, and the session it made first.");
    const workers = lab.slider({ label: "Workers", min: 0, max: 8, value: 4 });
    const order = lab.choice({ label: "Queue", options: [{ value: "need", label: "in the order of need" }, { value: "reverse", label: "reversed" }], value: "need" });
    const longestOut = lab.readout("longest frame after the first");
    const memoryOut = lab.readout("most memory in sessions");
    const doneOut = lab.readout("all compiled at");
    const note = lab.note();

    const FRAME = 16;
    const SESSION = 60;
    const SESSION_MB = 100;
    const END = 1000;
    // name, compile ms, first asked at (ms), or Infinity for a shader this run never asks for
    const SHADERS = [
        ["opaque vert", 90, 0], ["opaque frag", 70, 0], ["culling", 60, 0], ["tone map", 30, 0], ["sky", 40, 0],
        ["shadow", 50, 300], ["environment", 80, 300], ["cut-out", 70, 500], ["see-through", 70, 500], ["sort", 40, 500],
        ["animate", 60, 800], ["skin", 50, 800], ["pick", 80, Infinity], ["debug tone map", 30, Infinity],
    ];

    function simulate() {
        const n = workers.value;
        const shaders = SHADERS.map(function (s) { return { name: s[0], cost: s[1], asked: s[2], worker: -2, start: Infinity, end: Infinity }; });
        const queue = [];
        if (n > 0) {
            for (let i = 0; i < shaders.length; i++) queue.push(i);
            if (order.value === "reverse") queue.reverse();
        }
        const busyUntil = [];
        const session = [];
        for (let w = 0; w < n; w++) { busyUntil.push(0); session.push(false); }
        const frames = [];
        let sessions = 0;
        let peakSessions = 0;
        let t = 0;
        function startWorkers(now) {
            for (let w = 0; w < n; w++) {
                while (busyUntil[w] <= now && queue.length > 0) {
                    const s = shaders[queue.shift()];
                    s.worker = w;
                    s.start = Math.max(busyUntil[w], 0);
                    s.session = !session[w];
                    if (!session[w]) { session[w] = true; sessions += 1; peakSessions = Math.max(peakSessions, sessions); }
                    s.end = s.start + s.cost + (s.session ? SESSION : 0);
                    busyUntil[w] = s.end;
                }
            }
        }
        while (t < END) {
            startWorkers(t);
            const start = t;
            let wait = 0;
            shaders.forEach(function (s, i) {
                if (s.asked > start || s.asked === Infinity) return;
                if (s.end === Infinity) {
                    // still queued: off the queue, compiled here
                    const q = queue.indexOf(i);
                    if (q >= 0) queue.splice(q, 1);
                    s.worker = -1;
                    s.start = t;
                    s.end = t + s.cost;
                }
                if (s.end > t) { wait += s.end - t; t = s.end; startWorkers(t); }
            });
            frames.push({ start: start, wait: wait, length: wait + 1 });
            t += FRAME;
        }
        let done = 0;
        shaders.forEach(function (s) { if (s.end !== Infinity) done = Math.max(done, s.end); });
        return { shaders: shaders, frames: frames, peak: peakSessions, done: done };
    }

    const TOP = { x: 110, y: 34, width: 630, height: 46 };
    const LANES = { x: 110, y: 104, width: 630, height: 160 };
    lab.el(svg, "text", { x: 20, y: 20, class: "title", text: "Main thread: frames and waits" });
    lab.el(svg, "text", { x: 20, y: 98, class: "title", text: "Workers" });
    lab.el(svg, "rect", { x: TOP.x, y: TOP.y, width: TOP.width, height: TOP.height, class: "zone" });
    lab.el(svg, "rect", { x: LANES.x, y: LANES.y, width: LANES.width, height: LANES.height, class: "zone" });
    lab.el(svg, "text", { x: TOP.x - 8, y: TOP.y + 28, class: "end small", text: "main" });
    const marks = lab.el(svg, "g");
    const lanes = lab.el(svg, "g");
    lab.el(svg, "text", { x: LANES.x, y: 290, class: "small", text: "0 ms" });
    lab.el(svg, "text", { x: LANES.x + LANES.width, y: 290, class: "end small", text: `${END} ms` });
    const xAt = function (ms) { return LANES.x + Math.min(ms, END) / END * LANES.width; };

    lab.draw(function () {
        const run = simulate();
        const n = workers.value;
        lab.clear(marks);
        lab.clear(lanes);
        let longest = 0;
        run.frames.forEach(function (f, i) {
            if (i > 0) longest = Math.max(longest, f.length);
            if (f.wait <= 0) return;
            const x = xAt(f.start);
            const h = Math.min(TOP.height - 4, 6 + f.wait / 6);
            lab.el(marks, "rect", { x: x.toFixed(1), y: (TOP.y + TOP.height - 2 - h).toFixed(1), width: Math.max(2, xAt(f.start + f.wait) - x).toFixed(1), height: h.toFixed(1), class: "bad" });
            if (f.wait >= 30) lab.el(marks, "text", { x: (x + 2).toFixed(1), y: TOP.y + 12, class: "small t-bad", text: `${Math.round(f.wait)} ms` });
        });
        // shaders the main thread compiled itself
        run.shaders.forEach(function (s) {
            if (s.worker !== -1) return;
            const x = xAt(s.start);
            lab.el(marks, "rect", { x: x.toFixed(1), y: TOP.y + TOP.height - 8, width: Math.max(2, xAt(s.end) - x).toFixed(1), height: 6, class: "shader" });
        });
        const laneHeight = n > 0 ? LANES.height / n : LANES.height;
        if (n === 0) lab.el(lanes, "text", { x: LANES.x + LANES.width / 2, y: LANES.y + LANES.height / 2, class: "mid t-faint", text: "no workers: each shader compiles in the frame that asks for it" });
        for (let w = 0; w < n; w++) lab.el(lanes, "text", { x: LANES.x - 8, y: LANES.y + (w + 0.5) * laneHeight + 4, class: "end small", text: `worker ${w + 1}` });
        run.shaders.forEach(function (s) {
            if (s.worker < 0) return;
            const y = LANES.y + s.worker * laneHeight + 2;
            const h = Math.max(2, laneHeight - 4);
            let x = xAt(s.start);
            if (s.session) {
                lab.el(lanes, "rect", { x: x.toFixed(1), y: y.toFixed(1), width: (xAt(s.start + SESSION) - x).toFixed(1), height: h.toFixed(1), rx: 2, class: "mem" });
                x = xAt(s.start + SESSION);
            }
            const width = Math.max(1, xAt(s.end) - x - 1);
            lab.el(lanes, "rect", { x: x.toFixed(1), y: y.toFixed(1), width: width.toFixed(1), height: h.toFixed(1), rx: 2, class: s.asked === Infinity ? "shader o-50" : "shader" });
        });
        longestOut.set(`${Math.round(longest)} ms`, longest > 50 ? "bad" : "ok");
        memoryOut.set(`${run.peak * SESSION_MB} MB`, run.peak > 4 ? "bad" : "mem");
        doneOut.set(`${Math.round(run.done)} ms`, "gpu");
        if (n === 0) note.set("Every first use stalls its frame by the whole compile, even 0.8 s in, when the animated model arrives.", "bad");
        else if (longest > 50) note.set("Frames still wait: a shader is asked for before a worker reaches it. More workers, or a queue in the order of need, shorten the wait.", "bad");
        else note.set("After the first frame no frame waits long: each shader is ready before a frame asks for it. The sessions cost memory only until the queue is empty.", "ok");
    });
});
