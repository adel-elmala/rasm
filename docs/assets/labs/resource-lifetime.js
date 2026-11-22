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
