// Labs of docs/guides/transfer-queue.html.

// Three frames in a row, twice: with the copies on the graphics queue, and on the copy engine. A frame takes as long
// as the slower of the CPU and the graphics queue. On one queue the graphics queue also runs the frame's copies; with a
// copy engine they run on their own queue, and a frame waits only for copies the CPU saw done.
RasmLab.register("copy-queue", function (lab) {
    const svg = lab.svg(760, 260, "Two timelines of three frames to the same scale. With one queue, each frame's copies run on the graphics queue before it. With a copy engine they run on their own queue under the frames.");
    const cpu = lab.slider({ label: "CPU frame", min: 1, max: 12, step: 0.5, value: 4, format: function (v) { return `${v.toFixed(1)} ms`; } });
    const gpu = lab.slider({ label: "GPU frame", min: 1, max: 12, step: 0.5, value: 6, format: function (v) { return `${v.toFixed(1)} ms`; } });
    const copy = lab.slider({ label: "Copies per frame", min: 0, max: 6, step: 0.25, value: 2, format: function (v) { return `${v.toFixed(2)} ms`; } });
    const oneOut = lab.readout("frame, copies on graphics");
    const twoOut = lab.readout("frame, copy engine");
    const busyOut = lab.readout("copy engine busy");
    const note = lab.note();

    const X0 = 110;
    const WIDTH = 640;
    const FRAMES = 3;
    let grid = "";
    for (let i = 0; i <= 4; i++) grid += `M${X0 + i * WIDTH / 4},18V226`;
    lab.el(svg, "path", { d: grid, class: "grid" });
    lab.el(svg, "text", { x: 10, y: 34, class: "small t-cpu", text: "CPU" });
    lab.el(svg, "text", { x: 10, y: 70, class: "small", text: "graphics" });
    lab.el(svg, "text", { x: 10, y: 120, class: "title", text: "Copy engine" });
    lab.el(svg, "text", { x: 10, y: 150, class: "small t-cpu", text: "CPU" });
    lab.el(svg, "text", { x: 10, y: 186, class: "small", text: "graphics" });
    lab.el(svg, "text", { x: 10, y: 220, class: "small", text: "transfer" });
    lab.el(svg, "text", { x: 10, y: 16, class: "title", text: "One queue" });
    const bars = lab.el(svg, "g");
    const scaleText = lab.el(svg, "text", { x: 750, y: 248, class: "end small t-faint", text: "" });

    function bar(x, w, y, h, cls, label, scale) {
        const width = w * scale;
        if (width <= 0.5) return;
        lab.el(bars, "rect", { x: X0 + x * scale, y: y, width: width, height: h, rx: Math.min(3, width / 2), class: cls });
        if (label.length * 5.5 + 6 <= width) lab.el(bars, "text", { x: X0 + x * scale + width / 2, y: y + h / 2 + 4, class: "mid small", text: label });
    }

    lab.draw(function () {
        const c = cpu.value;
        const g = gpu.value;
        const k = copy.value;
        // One queue: the graphics queue runs the copies and the frame; the slower of it and the CPU sets the pace.
        const one = Math.max(c, g + k);
        // Copy engine: the frames never wait for copies in flight; the copies only need to keep up to load at full speed.
        const two = Math.max(c, g);
        const busy = two > 0 ? k / two : 0;
        const span = Math.max(one, two) * FRAMES;
        const scale = WIDTH / span;

        lab.clear(bars);
        for (let f = 0; f < FRAMES; f++) {
            bar(f * one, c, 24, 16, "cpu", `CPU ${f + 1}`, scale);
            bar(f * one, k, 58, 22, "mem", "copies", scale);
            bar(f * one + k, g, 58, 22, "gpu", `frame ${f + 1}`, scale);
            bar(f * two, c, 140, 16, "cpu", `CPU ${f + 1}`, scale);
            bar(f * two, g, 174, 22, "gpu", `frame ${f + 1}`, scale);
            bar(f * two + 0.02 * two, Math.min(k, two * 0.96), 208, 18, busy > 1 ? "bad" : "mem", "copies", scale);
        }
        lab.set(scaleText, { text: `${(span / 4).toFixed(1)} ms per grid step` });

        oneOut.set(`${one.toFixed(2)} ms`);
        twoOut.set(`${two.toFixed(2)} ms`, two < one - 0.005 ? "ok" : "");
        busyOut.set(`${Math.round(busy * 100)} %`, busy > 1 ? "bad" : "");
        if (busy > 1) note.set("The copy engine cannot keep up: loads take longer, but the frames still do not wait for it.", "bad");
        else if (two < one - 0.005) note.set(`The GPU sets the pace, so the copies on the graphics queue add ${(one - two).toFixed(2)} ms to every frame. The copy engine saves them.`, "ok");
        else note.set("The CPU sets the pace: the GPU idles long enough for the copies, so both cases take the same time.");
    });
});
