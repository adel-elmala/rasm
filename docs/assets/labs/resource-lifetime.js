// Labs of docs/guides/resource-lifetime.html.

// Two frames in flight and four bindless slots. The CPU records frame f; the GPU has finished frame f - 2 and runs
// f - 1. A frame's draws sample the slots it recorded, but a slot is read when the frame runs, so it reads whatever
// the slot holds then. A destroy releases the handle at once and puts "free the image and the slot" on the release
// queue, untagged; the frame end tags it with f; a frame start frees every entry whose frame the GPU has finished.
// With the queue off, the image is freed and the slot is free at the destroy.
RasmLab.register("release-queue", function (lab) {
    const svg = lab.svg(760, 290, "Three frames: done, running on the GPU, recorded by the CPU, each with the textures its draws sample in four slots. Below, what the four slots hold now, and the release queue.");
    const useQueue = lab.toggle({ label: "release queue", value: true });
    const slotOut = lab.readout("free slots");
    const waitOut = lab.readout("waiting in the queue");
    const note = lab.note();

    const SLOTS = 4;
    const NAMES = "TUVWXYZABCDEFGHIJKLMNOPQRS";
    let frame = 12;
    let owners = [];            // slot -> the live texture that holds it, "" for none
    let held = [];              // slot -> the texture its descriptor names, "" for never written
    let freedImage = [];        // slot -> its texture's image is already freed
    let freeSlots = [];
    let queue = [];             // { slot, name, tag }, tag -1 while untagged
    let recorded = {};          // frame -> the textures its draws sample, per slot
    let nextName = 0;
    let lastAction = "start";

    function reset() {
        frame = 12;
        owners = ["T", "U", "V", ""];
        held = owners.slice();
        freedImage = [false, false, false, false];
        freeSlots = [3];
        queue = [];
        recorded = {};
        for (let f = frame - 2; f <= frame; f++) recorded[f] = owners.slice();
        nextName = 3;
        lastAction = "start";
    }
    reset();

    function gpuDone() { return frame - 2; }

    function free(slot) {
        freedImage[slot] = true;
        freeSlots.push(slot);
    }

    function destroy() {
        const slot = owners.findIndex(function (name) { return name !== ""; });
        if (slot < 0) { lastAction = "nothing"; return; }
        const name = owners[slot];
        owners[slot] = "";
        if (useQueue.value) queue.push({ slot: slot, name: name, tag: -1 });
        else free(slot);
        lastAction = "destroy";
    }

    function create() {
        if (freeSlots.length === 0) { lastAction = "full"; return; }
        const slot = freeSlots.pop();
        const name = NAMES[nextName % NAMES.length];
        nextName += 1;
        owners[slot] = name;
        held[slot] = name;
        freedImage[slot] = false;
        recorded[frame][slot] = recorded[frame][slot] || name;
        lastAction = "create";
    }

    function nextFrame() {
        queue.forEach(function (entry) { if (entry.tag < 0) entry.tag = frame; });
        frame += 1;
        recorded[frame] = owners.slice();
        const done = gpuDone();
        while (queue.length && queue[0].tag >= 0 && queue[0].tag <= done) free(queue.shift().slot);
        lastAction = "frame";
    }

    lab.button("Destroy a texture", destroy);
    lab.button("Create a texture", create);
    lab.button("Next frame", nextFrame);
    lab.button("Start over", reset);

    const pictures = lab.el(svg, "g");

    lab.draw(function () {
        // Turning the queue off frees what waits in it at once.
        if (!useQueue.value) {
            while (queue.length) free(queue.shift().slot);
        }
        lab.clear(pictures);
        const done = gpuDone();
        const roles = ["done", "GPU runs it", "CPU records it"];
        const classes = ["ghost", "gpu", "cpu"];
        let hazard = "";
        for (let k = 0; k < 3; k++) {
            const f = frame - 2 + k;
            const x = 20 + k * 250;
            const uses = recorded[f];
            lab.el(pictures, "rect", { x: x, y: 20, width: 230, height: 112, rx: 6, class: classes[k] });
            lab.el(pictures, "text", { x: x + 115, y: 40, class: "mid small title", text: `frame ${f}: ${roles[k]}` });
            for (let s = 0; s < SLOTS; s++) {
                const sx = x + 15 + s * 52;
                const name = uses[s];
                const wrong = f > done && name !== "" && (held[s] !== name || freedImage[s]);
                lab.el(pictures, "rect", { x: sx, y: 54, width: 44, height: 40, rx: 3, class: wrong ? "bad" : name ? "mem" : "ghost" });
                lab.el(pictures, "text", { x: sx + 22, y: 79, class: "mid small", text: name || "-" });
                if (wrong && !hazard) {
                    hazard = held[s] !== name ? `Frame ${f} recorded ${name} in slot ${s}, but the slot now holds ${held[s]}: it draws the wrong texture.`
                                              : `Frame ${f} samples ${name} in slot ${s}, but its image is freed: garbage, or a lost device.`;
                }
            }
            lab.el(pictures, "text", { x: x + 115, y: 118, class: "mid small", text: k === 0 ? "nothing reads it any more" : "samples these slots" });
        }

        lab.el(pictures, "text", { x: 20, y: 162, class: "small title", text: "slots now" });
        for (let s = 0; s < SLOTS; s++) {
            const sx = 20 + s * 70;
            const waiting = queue.some(function (entry) { return entry.slot === s; });
            const cls = owners[s] ? "mem" : waiting ? "hot" : "ghost";
            lab.el(pictures, "rect", { x: sx, y: 172, width: 60, height: 44, rx: 3, class: cls });
            lab.el(pictures, "text", { x: sx + 30, y: 190, class: "mid small", text: `slot ${s}` });
            lab.el(pictures, "text", { x: sx + 30, y: 207, class: "mid small", text: owners[s] || (waiting ? "waits" : "free") });
        }

        lab.el(pictures, "text", { x: 330, y: 162, class: "small title", text: useQueue.value ? "release queue" : "release queue: off" });
        if (!useQueue.value) lab.el(pictures, "text", { x: 330, y: 198, class: "small t-bad", text: "a destroy frees the image and the slot at once" });
        else if (queue.length === 0) lab.el(pictures, "text", { x: 330, y: 198, class: "small t-faint", text: "empty" });
        queue.slice(0, 4).forEach(function (entry, i) {
            const qx = 330 + i * 105;
            lab.el(pictures, "rect", { x: qx, y: 172, width: 96, height: 44, rx: 3, class: "mem" });
            lab.el(pictures, "text", { x: qx + 48, y: 190, class: "mid small", text: `free ${entry.name}, slot ${entry.slot}` });
            lab.el(pictures, "text", { x: qx + 48, y: 207, class: "mid small", text: entry.tag < 0 ? "no value yet" : `after frame ${entry.tag}` });
        });
        lab.el(pictures, "text", { x: 20, y: 248, class: "small", text: `The GPU has finished frame ${done}. Each frame start frees the entries whose frame is done.` });
        lab.el(pictures, "text", { x: 20, y: 268, class: "small t-faint", text: "A frame reads its slots when it runs: it sees what each slot holds then." });

        slotOut.set(String(freeSlots.length));
        waitOut.set(String(queue.length), queue.length ? "mem" : "");
        if (hazard) note.set(hazard, "bad");
        else if (lastAction === "full") note.set("No free slot yet: the destroyed texture's slot waits for the frames that may sample it. Press Next frame.", "ok");
        else if (queue.length) note.set(`The handle is gone at once; the image and the slot wait until the GPU has finished frame ${queue[0].tag < 0 ? frame : queue[0].tag}.`, "ok");
        else note.set("Every frame in flight samples the textures it recorded. Destroy one, then create one.", "ok");
    });
});

// A pool of 40 units with the allocator of section 6: free ranges sorted by offset, first fit, and a freed range joins
// its free neighbours. Ranges come and go like streamed meshes; a big request shows fragmentation.
RasmLab.register("range-reuse", function (lab) {
    const svg = lab.svg(760, 200, "A pool drawn as a bar of 40 units: ranges in use, labelled with their names, and free holes. Below, the free list by offset.");
    const usedOut = lab.readout("in use");
    const freeOut = lab.readout("free in all");
    const largestOut = lab.readout("largest free range");
    const note = lab.note();

    const CAPACITY = 40;
    const X0 = 20;
    const UNIT = 18;            // pixels per unit
    const BIG = 12;
    let free = [];              // { offset, size }, sorted by offset
    let live = [];              // { offset, size, name }, in the order they were made
    let made = 0;
    let last = "start";

    function reset() {
        free = [{ offset: 0, size: CAPACITY }];
        live = [];
        made = 0;
        last = "start";
    }
    reset();

    function allocate(size) {
        for (let i = 0; i < free.length; i++) {
            if (free[i].size < size) continue;
            const offset = free[i].offset;
            if (free[i].size === size) free.splice(i, 1);
            else free[i] = { offset: offset + size, size: free[i].size - size };
            return offset;
        }
        return -1;
    }

    function release(offset, size) {
        let i = 0;
        while (i < free.length && free[i].offset < offset) i++;
        let range = { offset: offset, size: size };
        if (i > 0 && free[i - 1].offset + free[i - 1].size === offset) {
            range = { offset: free[i - 1].offset, size: free[i - 1].size + size };
            free.splice(i - 1, 1);
            i -= 1;
        }
        if (i < free.length && range.offset + range.size === free[i].offset) {
            range.size += free[i].size;
            free.splice(i, 1);
        }
        free.splice(i, 0, range);
    }

    // Sizes from a fixed sequence, so every run is the same.
    function nextSize() {
        return 3 + ((made * 7 + 3) % 6);
    }

    lab.button("Add a range", function () {
        const size = nextSize();
        const offset = allocate(size);
        if (offset < 0) { last = "full"; return; }
        live.push({ offset: offset, size: size, name: String.fromCharCode(65 + (made % 26)) });
        made += 1;
        last = "add";
    });
    lab.button("Free the oldest", function () {
        if (!live.length) return;
        const range = live.shift();
        release(range.offset, range.size);
        last = "free";
    });
    lab.button("Free every other", function () {
        const kept = [];
        live.sort(function (a, b) { return a.offset - b.offset; });
        live.forEach(function (range, i) {
            if (i % 2 === 0) release(range.offset, range.size);
            else kept.push(range);
        });
        live = kept;
        last = "holes";
    });
    lab.button(`Request ${BIG}`, function () {
        const offset = allocate(BIG);
        if (offset < 0) { last = "big-failed"; return; }
        live.push({ offset: offset, size: BIG, name: "big" });
        last = "big";
    });
    lab.button("Start over", reset);

    const picture = lab.el(svg, "g");
    lab.draw(function () {
        lab.clear(picture);
        lab.el(picture, "text", { x: X0, y: 22, class: "small title", text: "the pool, 40 units" });
        lab.el(picture, "rect", { x: X0, y: 34, width: CAPACITY * UNIT, height: 44, class: "ghost" });
        live.forEach(function (range) {
            lab.el(picture, "rect", { x: X0 + range.offset * UNIT, y: 34, width: range.size * UNIT, height: 44, class: range.name === "big" ? "hot" : "mem" });
            lab.el(picture, "text", { x: X0 + (range.offset + range.size / 2) * UNIT, y: 61, class: "mid small", text: `${range.name} ${range.size}` });
        });
        for (let u = 0; u <= CAPACITY; u += 10) {
            lab.el(picture, "text", { x: X0 + u * UNIT, y: 96, class: "mid small t-faint", text: String(u) });
        }
        lab.el(picture, "text", { x: X0, y: 130, class: "small title", text: "free list, by offset" });
        free.slice(0, 8).forEach(function (range, i) {
            const x = X0 + i * 92;
            lab.el(picture, "rect", { x: x, y: 140, width: 84, height: 36, rx: 3, class: "ghost" });
            lab.el(picture, "text", { x: x + 42, y: 156, class: "mid small", text: `at ${range.offset}` });
            lab.el(picture, "text", { x: x + 42, y: 170, class: "mid small", text: `size ${range.size}` });
        });
        if (!free.length) lab.el(picture, "text", { x: X0, y: 162, class: "small t-faint", text: "empty: the pool is full" });

        let total = 0;
        let largest = 0;
        free.forEach(function (range) { total += range.size; largest = Math.max(largest, range.size); });
        usedOut.set(String(CAPACITY - total));
        freeOut.set(String(total));
        largestOut.set(String(largest), largest < BIG && total >= BIG ? "bad" : "");
        if (last === "big-failed") note.set(`No hole of ${BIG}: ${total} units are free, but the largest hole holds ${largest}. This is fragmentation.`, "bad");
        else if (last === "full") note.set("No hole is large enough for the next range. Free one first.", "bad");
        else if (last === "free") note.set("The freed range joined its free neighbours, so the free list stays short.", "ok");
        else if (last === "big") note.set(`The first hole of at least ${BIG} took the request.`, "ok");
        else if (last === "holes") note.set(`Every other range is free now: ${total} units free in ${free.length} holes. Request ${BIG}.`, "ok");
        else note.set("Add ranges until the pool fills, then free some and add again: freed holes are taken first.", "ok");
    });
});
