// Labs of docs/guides/gpu-memory.html.

// The staging ring of section 4: 64 MiB, chunks of at most 16 MiB that never cross the end, head and tail
// counters that never wrap, and a fence value per submitted batch. It starts at step 3 of the page's stepper.
RasmLab.register("staging-ring", function (lab) {
    const svg = lab.svg(760, 196, "A 64 MiB staging ring drawn as a strip, with pending chunks, submitted batches, skipped space, and the head and tail positions.");
    const sizeIn = lab.slider({ label: "Upload size", min: 2, max: 40, step: 2, value: 16, format: function (v) { return `${v} MiB`; } });
    const usedOut = lab.readout("in use: head − tail");
    const copiesOut = lab.readout("pending copies, not yet submitted");
    const stallsOut = lab.readout("stalls: the CPU waited for the GPU");
    const note = lab.note();

    const RING = 64;
    const MAX_CHUNK = RING / 4;
    const X0 = 40;
    const PX = 10;

    let head = 0;               // MiB written since the start; never wraps
    let tail = 0;               // the oldest MiB the GPU may still read
    let chunks = [];            // { from, size, kind: "pending" | "batch" | "skip", batch }
    let copies = [];            // pending copy commands: { upload, from, size }
    let batches = [];           // submitted, not finished: { value, end }
    let submitted = 0;          // the last fence value handed out
    let uploads = 0;
    let stalls = 0;
    let message = { text: "", cls: "" };

    function drop() {
        chunks = chunks.filter(function (c) { return c.from + c.size > tail; });
    }
    function flush() {
        if (!copies.length) return false;
        submitted += 1;
        batches.push({ value: submitted, end: head });
        chunks.forEach(function (c) { if (c.kind === "pending") { c.kind = "batch"; c.batch = submitted; } });
        copies = [];
        return true;
    }
    // room for one chunk; returns what happened on the way
    function allocate(size, happened) {
        const position = head % RING;
        const padding = position + size > RING ? RING - position : 0;
        if (head + padding + size - tail > RING) {
            // full: submit what is pending and wait for every copy, which frees the whole ring
            flush();
            batches = [];
            tail = head;
            drop();
            stalls += 1;
            happened.stalled = true;
        }
        if (padding) {
            chunks.push({ from: head, size: padding, kind: "skip" });
            happened.skipped = padding;
        }
        head += padding;
        chunks.push({ from: head, size: size, kind: "pending" });
        head += size;
        return head - size;
    }
    function upload(size) {
        const happened = { stalled: false, skipped: 0, chunks: 0 };
        uploads += 1;
        while (size > 0) {
            const chunk = Math.min(size, MAX_CHUNK);
            const from = allocate(chunk, happened);
            const last = copies[copies.length - 1];
            // neighbours in the ring that go to neighbours in the same buffer are one copy
            if (last && last.upload === uploads && last.from + last.size === from) last.size += chunk;
            else copies.push({ upload: uploads, from: from, size: chunk });
            happened.chunks += 1;
            size -= chunk;
        }
        return happened;
    }

    upload(40);
    flush();
    message = { text: "Batch 1 (40 MiB) is submitted. Its bytes stay in use until the GPU finishes it. Upload more now, or let the GPU finish first.", cls: "" };

    lab.button("Upload", function () {
        const size = sizeIn.value;
        const happened = upload(size);
        let text = `Uploaded ${size} MiB in ${happened.chunks} ${happened.chunks === 1 ? "chunk" : "chunks"}.`;
        if (happened.skipped) text += ` A chunk did not fit before the end of the ring, so head skipped ${happened.skipped} MiB.`;
        if (happened.stalled) text = `The ring was full: the CPU submitted what was pending and waited for every copy. Then tail = head, and the ${size} MiB went in.`;
        message = { text: text, cls: happened.stalled ? "bad" : "" };
    });
    lab.button("Submit the batch", function () {
        const end = head;
        if (flush()) message = { text: `Batch ${submitted} is submitted. It remembers its fence value (${submitted}) and where it ends (${end} MiB).`, cls: "" };
        else message = { text: "Nothing is pending, so there is nothing to submit.", cls: "" };
    });

    lab.el(svg, "rect", { x: X0, y: 46, width: RING * PX, height: 40, rx: 4, class: "box" });
    const strip = lab.el(svg, "g");
    [0, 16, 32, 48, 64].forEach(function (m) {
        lab.el(svg, "text", { x: X0 + m * PX, y: 102, class: "mid small", text: m === 64 ? "64 MiB" : m });
    });
    const tailArrow = lab.el(svg, "line", { y1: 22, y2: 42, class: "arrow", "marker-end": "url(#ah)" });
    const tailText = lab.el(svg, "text", { y: 22, class: "small" });
    const headArrow = lab.el(svg, "line", { y1: 132, y2: 110, class: "arrow hot", "marker-end": "url(#ah-hot)" });
    const headText = lab.el(svg, "text", { y: 136, class: "small t-hot" });
    const gpu = lab.el(svg, "g");
    const gpuBox = lab.el(gpu, "rect", { x: 440, y: 156, width: 280, height: 34, rx: 6 });
    const gpuText = lab.el(gpu, "text", { x: 580, y: 178, class: "mid" });
    lab.clickable(gpu, "The GPU finishes its oldest batch", function () {
        const batch = batches.shift();
        if (!batch) {
            message = { text: "The GPU has no batch to finish.", cls: "" };
            return;
        }
        tail = batch.end;
        drop();
        message = { text: `Fence value ${batch.value} is reached, so tail moves to the end of batch ${batch.value} (${batch.end} MiB). That space is free again.`, cls: "ok" };
    });
    lab.el(svg, "text", { x: X0, y: 178, class: "small", text: "The GPU works at its own speed. Click it when a batch is done:" });

    lab.draw(function () {
        lab.clear(strip);
        chunks.forEach(function (c) {
            const x = X0 + (c.from % RING) * PX;
            const w = c.size * PX;
            lab.el(strip, "rect", { x: x, y: 46, width: w, height: 40, rx: 4, class: c.kind === "pending" ? "mem" : c.kind === "batch" ? "gpu" : "ghost" });
            const text = c.kind === "pending" ? `${c.size} MiB` : c.kind === "batch" ? `batch ${c.batch}` : "skipped";
            if (w >= 50) lab.el(strip, "text", { x: x + w / 2, y: 71, class: c.kind === "skip" ? "mid small t-faint" : "mid small", text: text });
        });

        const tx = X0 + (tail % RING) * PX;
        lab.set(tailArrow, { x1: tx, x2: tx });
        lab.set(tailText, { x: tx > 440 ? tx - 8 : tx + 8, class: tx > 440 ? "small end" : "small", text: `tail = ${tail} MiB` });
        const hx = X0 + (head % RING) * PX;
        lab.set(headArrow, { x1: hx, x2: hx });
        lab.set(headText, {
            x: hx > 440 ? hx - 8 : hx + 8,
            class: hx > 440 ? "small t-hot end" : "small t-hot",
            text: head >= RING ? `head = ${head} MiB (${head} mod ${RING} = ${head % RING})` : `head = ${head} MiB`,
        });

        lab.set(gpuBox, { class: batches.length ? "gpu" : "gpu off" });
        lab.set(gpuText, { text: batches.length ? `GPU: finish batch ${batches[0].value}` : "GPU: no batch to finish" });

        usedOut.set(`${head - tail} / ${RING} MiB`, head - tail > RING * 0.75 ? "bad" : "");
        copiesOut.set(String(copies.length), "mem");
        stallsOut.set(String(stalls), stalls ? "bad" : "ok");
        note.set(message.text, message.cls);
    });
});
