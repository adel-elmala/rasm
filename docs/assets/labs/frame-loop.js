// Labs of docs/guides/frame-loop.html.

// The reader plays the CPU and the GPU. The CPU walks the steps of a frame in its slot; the GPU finishes submitted
// frames in order. The rule is beginFrame's: the fence wait passes only when the GPU reached the slot's fenceValue.
RasmLab.register("frame-slots", function (lab) {
    const svg = lab.svg(760, 300, "The steps of one CPU frame, the two frame slots with the fence value each one waits for, and the frames the GPU still has to finish.");

    const STEPS = ["1 · fence wait", "2 · rewrite the slot", "3 · record the passes", "4 · submit, present"];
    const SLOT_X = [10, 385];
    const SLOT_W = 365;

    // the state of the page's example: frame 7 starts in slot 0, which last signaled 5, and the GPU has reached 4
    let frame = 7;
    let frameCount = 0;
    let step = 0;
    let reached = 4;                    // the value frameFence has reached
    let frameFenceValue = 6;            // the last value a submit signals
    const fenceValue = [5, 6];          // frameResources[slot].fenceValue
    let idle = false;

    function inFlight(slot) {
        return fenceValue[slot] > reached;
    }

    function cpuStep() {
        idle = false;
        if (step === 0) {
            if (!inFlight(frameCount)) step = 1;
        } else if (step === 1) {
            step = 2;
        } else if (step === 2) {
            // the submit signals the next value, and the slot remembers it
            frameFenceValue += 1;
            fenceValue[frameCount] = frameFenceValue;
            step = 3;
        } else {
            frameCount = (frameCount + 1) % 2;
            frame += 1;
            step = 0;
        }
    }

    function gpuFinish() {
        idle = reached >= frameFenceValue;
        if (!idle) reached += 1;
    }

    // ---- the picture
    const frameLabel = lab.el(svg, "text", { x: 10, y: 30, class: "title" });
    const strip = STEPS.map(function (label, k) {
        const x = 150 + k * 150;
        return {
            box: lab.el(svg, "rect", { x: x, y: 10, width: 140, height: 30, rx: 5 }),
            ring: lab.el(svg, "rect", { x: x - 2, y: 8, width: 144, height: 34, rx: 7, class: "hot" }),
            text: lab.el(svg, "text", { x: x + 70, y: 29, class: "mid small", text: label }),
        };
    });
    const slots = SLOT_X.map(function (x, i) {
        lab.el(svg, "rect", { x: x, y: 58, width: SLOT_W, height: 120, rx: 8, class: "zone" });
        const ring = lab.el(svg, "rect", { x: x, y: 58, width: SLOT_W, height: 120, rx: 8, class: "hot" });
        lab.el(svg, "text", { x: x + 14, y: 80, class: "title", text: `slot ${i}` });
        lab.el(svg, "text", { x: x + 14, y: 166, class: "small", text: "command buffer, scene tables, draw lists, SceneData" });
        return {
            ring: ring,
            current: lab.el(svg, "text", { x: x + SLOT_W - 14, y: 80, class: "end small t-hot", text: `frameCount = ${i}: this frame's slot` }),
            value: lab.el(svg, "text", { x: x + 14, y: 102, class: "mono" }),
            box: lab.el(svg, "rect", { x: x + 14, y: 112, width: SLOT_W - 28, height: 34, rx: 5 }),
            state: lab.el(svg, "text", { x: x + SLOT_W / 2, y: 134, class: "mid" }),
        };
    });
    lab.el(svg, "rect", { x: 10, y: 192, width: 740, height: 98, rx: 8, class: "zone" });
    lab.el(svg, "text", { x: 24, y: 214, class: "title", text: "GPU, graphics queue" });
    const queue = lab.el(svg, "g");
    const reachedText = lab.el(svg, "text", { x: 736, y: 244, class: "end mono" });
    const signaledText = lab.el(svg, "text", { x: 736, y: 266, class: "end mono" });

    lab.button("CPU: next step", cpuStep);
    lab.button("GPU: finish a frame", gpuFinish);
    const frameOut = lab.readout("the frame the CPU works on");
    const fenceOut = lab.readout("frame fence: reached / last signaled");
    const flightOut = lab.readout("frames the GPU still has");
    const note = lab.note();

    lab.draw(function () {
        const blocked = step === 0 && inFlight(frameCount);
        const other = 1 - frameCount;

        lab.set(frameLabel, { text: `CPU · frame ${frame}` });
        strip.forEach(function (s, k) {
            const on = k === step;
            lab.set(s.box, { class: on ? (blocked ? "bad" : "cpu") : "ghost" });
            lab.set(s.ring, { class: on ? "hot" : "hide" });
            lab.set(s.text, { class: on ? "mid small title" : "mid small t-faint" });
        });

        slots.forEach(function (s, i) {
            const mine = i === frameCount;
            // a frame signals its own number here: frame N signals N
            const busy = inFlight(i);
            const running = fenceValue[i] === reached + 1;
            lab.set(s.ring, { class: mine ? "hot" : "hide" });
            lab.set(s.current, { class: mine ? "end small t-hot" : "hide" });
            lab.set(s.value, { text: `frameResources[${i}].fenceValue = ${fenceValue[i]}` });
            if (mine && blocked) {
                lab.set(s.box, { class: "bad" });
                lab.set(s.state, { text: `the CPU waits: the GPU still has frame ${fenceValue[i]}` });
            } else if (mine && step === 1) {
                lab.set(s.box, { class: "cpu" });
                lab.set(s.state, { text: "the CPU rewrites its buffers" });
            } else if (mine && step === 2) {
                lab.set(s.box, { class: "cpu" });
                lab.set(s.state, { text: "the CPU records its command buffer" });
            } else if (busy) {
                lab.set(s.box, { class: "gpu" });
                lab.set(s.state, { text: running ? `the GPU runs frame ${fenceValue[i]} from it` : `frame ${fenceValue[i]} waits in the queue with it` });
            } else {
                lab.set(s.box, { class: "box" });
                lab.set(s.state, { text: `free: the GPU finished frame ${fenceValue[i]}` });
            }
        });

        lab.clear(queue);
        for (let value = reached + 1; value <= frameFenceValue; value++) {
            const x = 24 + (value - reached - 1) * 200;
            const slot = fenceValue[0] === value ? 0 : 1;
            lab.el(queue, "rect", { x: x, y: 226, width: 190, height: 48, rx: 6, class: "gpu" });
            lab.el(queue, "text", { x: x + 95, y: 245, class: "mid title", text: `frame ${value} · slot ${slot}` });
            lab.el(queue, "text", { x: x + 95, y: 263, class: "mid small", text: value === reached + 1 ? `running: signals ${value} at its end` : `queued: signals ${value}` });
        }
        if (reached === frameFenceValue) lab.el(queue, "text", { x: 24, y: 254, class: "small", text: "idle: every submitted frame is finished" });
        lab.set(reachedText, { text: `frameFence reached ${reached}` });
        lab.set(signaledText, { text: `frameFenceValue = ${frameFenceValue}` });

        frameOut.set(`${frame} · slot ${frameCount}`, "cpu");
        fenceOut.set(`${reached} / ${frameFenceValue}`, "gpu");
        flightOut.set(String(frameFenceValue - reached), blocked ? "bad" : "");

        if (idle) note.set("The GPU has nothing to finish: no submitted frame is left. Let the CPU go on.");
        else if (blocked) note.set(`Blocked in beginFrame: slot ${frameCount} last signaled ${fenceValue[frameCount]}, and the GPU has reached ${reached}. The CPU cannot get further ahead. Let the GPU finish a frame.`, "bad");
        else if (step === 0) note.set(`The wait passes: slot ${frameCount} last signaled ${fenceValue[frameCount]}, and the GPU has reached ${reached}. Nothing on the GPU uses slot ${frameCount} now.`, "ok");
        else if (step === 1) note.set(`updateFrameStats, updateStatsOverlay and syncGpuScene write only slot ${frameCount}. ${inFlight(other) ? `The GPU still reads slot ${other}, and that is safe.` : `Slot ${other} is free too.`}`, "ok");
        else if (step === 2) note.set(`The passes go into slot ${frameCount}'s command buffer. They point the GPU at slot ${frameCount}'s tables. ${inFlight(other) ? `The GPU still works from slot ${other}.` : ""}`, "ok");
        else note.set(`Frame ${frame} is submitted. Its submit signals ${frameFenceValue} when the GPU finishes it, and slot ${frameCount} remembers ${frameFenceValue}. Next, frameCount flips to ${other}.`);
    });
});
