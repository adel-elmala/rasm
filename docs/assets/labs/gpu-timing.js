// Labs of docs/guides/gpu-timing.html.

// Frames in flight, one query pool per slot. Each new frame waits for its slot, reads the pool that the slot's
// last frame wrote, resets it and records into it. So the number on screen is some frames old.
RasmLab.register("query-readback", function (lab) {
    const svg = lab.svg(760, 236, "The last six frames as boxes, and one query pool per frame slot. The frame the CPU records now reads the pool that an older frame of the same slot wrote.");
    const slots = lab.slider({ label: "Frames in flight", min: 1, max: 3, value: 2 });
    let frame = 7;
    lab.button("Next frame", function () { frame += 1; });
    let wait = 0;
    lab.animate(function (dt) {
        wait += dt;
        while (wait >= 1.2) {
            wait -= 1.2;
            frame += 1;
        }
    });
    const recordOut = lab.readout("frame the CPU records now");
    const shownOut = lab.readout("frame whose GPU time is on screen");
    const ageOut = lab.readout("age of that number at 60 frames per second");
    const note = lab.note();

    // the GPU time of a frame: about 4 ms, with a slow frame now and then
    function gpuMs(f) {
        return f % 6 === 4 ? 9.5 : 4 + ((f * 37) % 10) * 0.1;
    }

    lab.el(svg, "text", { x: 20, y: 18, class: "small", text: "the last frames, the oldest on the left" });
    lab.el(svg, "text", { x: 20, y: 136, class: "small", text: "one query pool per slot" });
    const frames = lab.el(svg, "g");
    const pools = lab.el(svg, "g");
    const link = lab.el(svg, "line", { class: "arrow hot", "marker-end": "url(#ah-hot)" });

    lab.draw(function () {
        const n = slots.value;
        const source = frame - n;       // the frame that wrote the pool this frame reads
        lab.clear(frames);
        lab.clear(pools);
        for (let k = 0; k < 6; k++) {
            const f = frame - 5 + k;
            if (f < 0) continue;
            const x = 20 + k * 122;
            const read = f <= source;
            lab.el(frames, "rect", { x: x, y: 28, width: 112, height: 68, rx: 6, class: f === frame ? "cpu" : (read ? "box" : "gpu") });
            if (f === source) lab.el(frames, "rect", { x: x - 3, y: 25, width: 118, height: 74, rx: 8, class: "hot" });
            lab.el(frames, "text", { x: x + 56, y: 48, class: "mid title", text: `frame ${f}` });
            lab.el(frames, "text", { x: x + 56, y: 66, class: "mid small", text: `slot ${"ABC"[f % n]}` });
            const state = f === frame ? "the CPU records it" : (read ? `GPU: ${gpuMs(f).toFixed(1)} ms` : "in flight: not read");
            lab.el(frames, "text", { x: x + 56, y: 84, class: "mid small", text: state });
        }
        const current = frame % n;
        for (let s = 0; s < n; s++) {
            const x = 20 + s * 244;
            // the newest earlier frame that used this slot
            let holds = frame - 1;
            while (holds >= 0 && holds % n !== s) holds -= 1;
            lab.el(pools, "rect", { x: x, y: 146, width: 232, height: 78, rx: 6, class: "mem" });
            lab.el(pools, "text", { x: x + 116, y: 166, class: "mid title", text: `pool ${"ABC"[s]}` });
            let first = "empty: no frame wrote it yet";
            let second = "";
            if (s === current && source >= 0) {
                first = `1. read frame ${source}: ${gpuMs(source).toFixed(1)} ms`;
                second = `2. reset, record frame ${frame} into it`;
            } else if (s === current) {
                first = "nothing to read yet";
                second = `record frame ${frame} into it`;
            } else if (holds >= 0) {
                first = `frame ${holds} writes its timestamps`;
                second = "in flight: do not touch";
            }
            lab.el(pools, "text", { x: x + 116, y: 188, class: "mid small", text: first });
            lab.el(pools, "text", { x: x + 116, y: 206, class: "mid small", text: second });
        }
        if (source >= 0) {
            const from = 20 + (5 - n) * 122 + 56;
            lab.set(link, { class: "arrow hot", x1: from, y1: 101, x2: lab.clamp(from, 20 + current * 244 + 20, 20 + current * 244 + 212), y2: 143 });
        } else {
            lab.set(link, { class: "hide" });
        }

        recordOut.set(String(frame), "cpu");
        if (source >= 0) {
            shownOut.set(`${source}: ${gpuMs(source).toFixed(1)} ms`);
            ageOut.set(`${(n * 1000 / 60).toFixed(0)} ms`);
            note.set(`Frame ${frame} uses slot ${"ABC"[current]}. The CPU waits for the fence of frame ${source}, the slot's last frame. Then pool ${"ABC"[current]} is ready: it reads ${gpuMs(source).toFixed(1)} ms, with no extra wait. The number is ${n} ${n === 1 ? "frame" : "frames"} old.`, "ok");
        } else {
            shownOut.set("none yet");
            ageOut.set("–");
            note.set(`Frame ${frame} is the first frame of slot ${"ABC"[current]}. No frame wrote pool ${"ABC"[current]} yet, so there is nothing to show.`);
        }
    });
});

// A frame-time signal with noise and one slow frame, and four ways to turn it into a number to show.
RasmLab.register("smoothing", function (lab) {
    const svg = lab.svg(760, 250, "A graph of 120 frame times with one spike of 40 milliseconds, and the value an overlay would show for it.");
    const method = lab.choice({
        label: "Show",
        options: [{ value: "hold", label: "Sample and hold" }, { value: "mean", label: "Mean of n" }, { value: "ema", label: "Moving average" }, { value: "max", label: "Max of n" }],
        value: "ema",
    });
    const span = lab.slider({ label: "n frames (α = 1 / n)", min: 2, max: 40, value: 10 });
    const noisy = lab.toggle({ label: "Noise", value: true });
    const peakOut = lab.readout("largest value shown");
    const staysOut = lab.readout("frames the spike changes the value by 0.5 ms or more");
    const wobbleOut = lab.readout("wobble before the spike (max − min)");
    const note = lab.note();

    const COUNT = 120;
    const SPIKE_AT = 40;
    const BASE = 16.7;
    const SPIKE = 40;
    let seed = 5;
    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }
    const noise = [];
    for (let i = 0; i < COUNT; i++) noise.push((random() - 0.5) * 3);

    function sx(i) { return 70 + i * 5.6; }
    function sy(ms) { return 216 - ms * 4.2; }

    lab.el(svg, "line", { x1: 70, y1: 216, x2: 742, y2: 216, class: "arrow" });
    lab.el(svg, "line", { x1: 70, y1: 216, x2: 70, y2: 24, class: "arrow" });
    lab.el(svg, "path", { d: `M66,${sy(BASE)}h8M66,${sy(SPIKE)}h8`, class: "arrow" });
    lab.el(svg, "text", { x: 60, y: sy(BASE) + 4, class: "end small", text: "16.7 ms" });
    lab.el(svg, "text", { x: 60, y: sy(SPIKE) + 4, class: "end small", text: "40 ms" });
    lab.el(svg, "text", { x: 60, y: 220, class: "end small", text: "0" });
    lab.el(svg, "text", { x: 406, y: 238, class: "mid small", text: "frames →" });
    lab.el(svg, "line", { x1: 520, y1: 34, x2: 550, y2: 34, class: "arrow o-50" });
    lab.el(svg, "text", { x: 558, y: 38, class: "small", text: "every frame's time" });
    lab.el(svg, "line", { x1: 520, y1: 54, x2: 550, y2: 54, class: "arrow hot thick" });
    lab.el(svg, "text", { x: 558, y: 58, class: "small", text: "the value shown" });
    const rawLine = lab.el(svg, "polyline", { class: "arrow o-50" });
    const shownLine = lab.el(svg, "polyline", { class: "arrow hot thick" });

    function shownValues(values, kind, n) {
        const out = [];
        let average = values[0];
        for (let i = 0; i < values.length; i++) {
            const from = Math.max(0, i - n + 1);
            if (kind === "hold") {
                out.push(values[Math.floor(i / n) * n]);
            } else if (kind === "mean") {
                let sum = 0;
                for (let k = from; k <= i; k++) sum += values[k];
                out.push(sum / (i - from + 1));
            } else if (kind === "max") {
                let most = values[from];
                for (let k = from; k <= i; k++) most = Math.max(most, values[k]);
                out.push(most);
            } else {
                average = average + (values[i] - average) / n;
                out.push(average);
            }
        }
        return out;
    }

    function points(values) {
        return values.map(function (v, i) { return `${sx(i).toFixed(1)},${sy(v).toFixed(1)}`; }).join(" ");
    }

    lab.draw(function () {
        const n = span.value;
        const calm = [];
        for (let i = 0; i < COUNT; i++) calm.push(BASE + (noisy.value ? noise[i] : 0));
        const raw = calm.slice();
        raw[SPIKE_AT] = SPIKE;
        const shown = shownValues(raw, method.value, n);
        const without = shownValues(calm, method.value, n);     // the same frames with no spike, to see what the spike changed
        lab.set(rawLine, { points: points(raw) });
        lab.set(shownLine, { points: points(shown) });

        let peak = 0;
        let stays = 0;
        let low = Infinity;
        let high = -Infinity;
        for (let i = 0; i < COUNT; i++) {
            peak = Math.max(peak, shown[i]);
            if (shown[i] - without[i] >= 0.5) stays += 1;
            if (i >= 10 && i < SPIKE_AT) {
                low = Math.min(low, shown[i]);
                high = Math.max(high, shown[i]);
            }
        }
        peakOut.set(`${peak.toFixed(1)} ms`);
        staysOut.set(String(stays), stays === 0 ? "bad" : "");
        wobbleOut.set(`${(high - low).toFixed(1)} ms`);

        const jump = (SPIKE - BASE) / n;
        if (method.value === "hold" && stays === 0) note.set(`Sample and hold shows one frame out of ${n}. The slow frame is not one of them, so the spike is missed.`, "bad");
        else if (method.value === "hold") note.set(`Sample and hold shows one frame out of ${n}. By luck the slow frame is one of them, so it shows 40 ms for ${n} frames.`);
        else if (method.value === "mean") note.set(`The mean spreads the spike over ${n} frames: a flat bump of about (40 − 16.7) / ${n} = ${jump.toFixed(1)} ms.`, "ok");
        else if (method.value === "max") note.set(`The max shows the full 40 ms for ${n} frames. It shows stutter well, but it hides the usual frame time.`);
        else note.set(`α = 1 / ${n} = ${(1 / n).toFixed(2)}. The value jumps by about α · (40 − 16.7) = ${jump.toFixed(1)} ms, then slides back slowly.`, "ok");
    });
});
