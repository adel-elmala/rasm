// Labs of docs/guides/bindless.html.

// Section 2: material records hold slot numbers, the shader indexes the one texture array with them. The reader
// picks the record a draw reads, and writes other slot numbers into it.
RasmLab.register("slot-index", function (lab) {
    const svg = lab.svg(760, 262, "Three material records with two slot numbers each, a shader that reads one record, and one texture array of eight slots. Arrows go from the shader to the two slots the record names.");
    const field = lab.choice({ label: "A click on a slot changes", options: [{ value: "color", label: "base color" }, { value: "normal", label: "normal" }], value: "color" });
    const colorOut = lab.readout("base color comes from");
    const normalOut = lab.readout("normal comes from");
    lab.readout("binds to change a material").set("0", "ok");
    const note = lab.note();

    const TEXTURES = [
        { name: "grass", kind: "color" }, { name: "rock", kind: "color" }, { name: "brick color", kind: "color" }, { name: "sky", kind: "color" },
        null, { name: "brick normal", kind: "normal" }, null, { name: "rock normal", kind: "normal" },
    ];
    const records = [{ name: "wall", color: 2, normal: 5 }, { name: "cliff", color: 1, normal: 7 }, { name: "ground", color: 0, normal: 7 }];
    let selected = 0;

    function slotX(k) { return 20 + k * 90; }

    lab.el(svg, "text", { x: 20, y: 18, class: "small t-mem", text: "material records (in a GPU buffer): click one" });
    lab.el(svg, "text", { x: 200, y: 40, class: "mid small", text: "base color" });
    lab.el(svg, "text", { x: 262, y: 40, class: "mid small", text: "normal" });
    const rows = records.map(function (record, k) {
        const g = lab.el(svg, "g");
        const row = {
            box: lab.el(g, "rect", { x: 20, y: 48 + k * 32, width: 270, height: 26, rx: 4, class: "mem" }),
            color: null,
            normal: null,
        };
        lab.el(g, "text", { x: 34, y: 66 + k * 32, text: record.name });
        row.color = lab.el(g, "text", { x: 200, y: 66 + k * 32, class: "mid title" });
        row.normal = lab.el(g, "text", { x: 262, y: 66 + k * 32, class: "mid title" });
        lab.clickable(g, `Draw with the record ${record.name}`, function () { selected = k; });
        return row;
    });
    const cell = lab.el(svg, "rect", { width: 44, height: 20, rx: 3, class: "hot thin" });
    const outline = lab.el(svg, "rect", { x: 16, width: 278, height: 34, rx: 6, class: "hot" });
    const toShader = lab.el(svg, "line", { x1: 294, x2: 338, y2: 86, class: "arrow mem", "marker-end": "url(#ah-mem)" });

    lab.el(svg, "rect", { x: 340, y: 46, width: 400, height: 80, rx: 40, class: "shader" });
    const shaderTitle = lab.el(svg, "text", { x: 540, y: 70, class: "mid title" });
    const colorLine = lab.el(svg, "text", { x: 540, y: 92, class: "mid mono" });
    const normalLine = lab.el(svg, "text", { x: 540, y: 110, class: "mid mono" });

    const slotBoxes = TEXTURES.map(function (texture, k) {
        const g = lab.el(svg, "g");
        const box = lab.el(g, "rect", { x: slotX(k), y: 182, width: 80, height: 50, rx: 4, class: texture ? "mem" : "ghost" });
        lab.el(g, "text", { x: slotX(k) + 40, y: 202, class: texture ? "mid small" : "mid small t-faint", text: `slot ${k}` });
        lab.el(g, "text", { x: slotX(k) + 40, y: 220, class: texture ? "mid small" : "mid small t-faint", text: texture ? texture.name : "empty" });
        lab.clickable(g, `Write slot ${k} into the record`, function () { records[selected][field.value] = k; });
        return box;
    });
    lab.el(svg, "text", { x: 20, y: 252, class: "small", text: "the one texture array, bound once: click a slot to write its number into the record" });
    const normalRing = lab.el(svg, "rect", { y: 176, width: 92, height: 62, rx: 7, class: "shader nofill thick" });
    const colorRing = lab.el(svg, "rect", { y: 178, width: 88, height: 58, rx: 6, class: "hot" });
    const colorArrow = lab.el(svg, "line", { x1: 440, y1: 126, y2: 174, class: "arrow hot", "marker-end": "url(#ah-hot)" });
    const normalArrow = lab.el(svg, "line", { x1: 640, y1: 126, y2: 172, class: "arrow shader", "marker-end": "url(#ah-shader)" });

    function describe(slot) {
        return TEXTURES[slot] ? `slot ${slot} · ${TEXTURES[slot].name}` : `slot ${slot} · empty`;
    }

    lab.draw(function () {
        records.forEach(function (record, k) {
            lab.set(rows[k].color, { text: record.color });
            lab.set(rows[k].normal, { text: record.normal });
        });
        const record = records[selected];
        const rowY = 48 + selected * 32;
        lab.set(outline, { y: rowY - 4 });
        lab.set(cell, { x: (field.value === "color" ? 200 : 262) - 22, y: rowY + 3 });
        lab.set(toShader, { y1: rowY + 13 });
        lab.set(shaderTitle, { text: `shader, in a draw that reads "${record.name}"` });
        lab.set(colorLine, { text: `color  = textures[${record.color}].Sample(uv)` });
        lab.set(normalLine, { text: `normal = textures[${record.normal}].Sample(uv)` });
        lab.set(colorRing, { x: slotX(record.color) - 4 });
        lab.set(normalRing, { x: slotX(record.normal) - 6 });
        lab.set(colorArrow, { x2: slotX(record.color) + 30 });
        lab.set(normalArrow, { x2: slotX(record.normal) + 50 });
        slotBoxes.forEach(function (box, k) {
            if (!TEXTURES[k]) lab.set(box, { class: k === record.color || k === record.normal ? "bad" : "ghost" });
        });

        const color = TEXTURES[record.color];
        const normal = TEXTURES[record.normal];
        colorOut.set(describe(record.color), !color || color.kind !== "color" ? "bad" : "");
        normalOut.set(describe(record.normal), !normal || normal.kind !== "normal" ? "bad" : "");
        if (!color || !normal) {
            note.set(`Broken: slot ${!color ? record.color : record.normal} is empty. Nothing checks the number, so the shader reads a slot that was never written. Empty slots are allowed only when no shader reads them.`, "bad");
        } else if (color.kind !== "color" || normal.kind !== "normal") {
            note.set("It runs, but the picture is wrong: a slot number has no type. Nothing stops a record from using a normal map as a color, or a color as a normal map.", "bad");
        } else {
            note.set(`The draw reads the record "${record.name}", finds ${record.color} and ${record.normal}, and samples those two slots. Another material is another row of numbers; nothing is bound again.`, "ok");
        }
    });
});

// Sections 5 and 6: the slot allocator (a counter and a free list) with two frames in flight. A freed slot waits
// in the retire list until the GPU has finished the frame that freed it, or, with the list off, is reused at once.
RasmLab.register("slot-lifetime", function (lab) {
    const svg = lab.svg(760, 226, "Ten slots of a texture array, a counter, a retire list and a free list, with the frame the CPU records and the frame the GPU runs.");
    const retire = lab.toggle({ label: "Freed slots wait in the retire list", value: true });
    const usedOut = lab.readout("slots in use");
    const waitOut = lab.readout("slots that wait for the GPU");
    const wrongOut = lab.readout("slots reused while a frame still reads them");
    const note = lab.note();

    const CAPACITY = 10;
    const IN_FLIGHT = 2;
    // the state of the static picture above the lab: 4 and 8 are free, 9 was never used
    const slots = [];
    for (let k = 0; k < CAPACITY; k++) slots.push({ state: k === 4 || k === 8 ? "free" : k === 9 ? "never" : "used", texture: `T${k}`, freed: -100, wrong: false });
    let counter = 9;
    let freeList = [4, 8];
    let retireList = [];        // { slot, frame }
    let frame = 10;             // the frame the CPU records
    let textures = 10;
    let wrongs = 0;
    let message = { text: "Frame 10: seven textures are in the array. Free one, then add a new one, and count the frames until its slot is used again.", cls: "" };

    function slotX(k) { return 16 + k * 68; }

    lab.button("Add a texture", function () {
        let slot;
        if (freeList.length) slot = freeList.pop();
        else if (counter === CAPACITY) {
            message = { text: "The array is full: the free list is empty and the counter is at the capacity. The allocation fails.", cls: "bad" };
            return;
        } else {
            slot = counter;
            counter += 1;
        }
        const s = slots[slot];
        const early = frame < s.freed + IN_FLIGHT;
        s.state = "used";
        s.texture = `T${textures}`;
        s.wrong = early;
        textures += 1;
        if (early) {
            wrongs += 1;
            message = { text: `Wrong: slot ${slot} now holds ${s.texture}, but frame ${frame - 1} (on the GPU) and frame ${frame} may still read slot ${slot} for the old texture. They draw with the new image.`, cls: "bad" };
        } else {
            message = { text: `${s.texture} takes slot ${slot}. No frame in flight reads that slot, so writing it is safe.`, cls: "ok" };
        }
    });
    lab.button("Next frame", function () {
        frame += 1;
        const finished = frame - IN_FLIGHT;
        const moved = [];
        retireList = retireList.filter(function (entry) {
            if (entry.frame > finished) return true;
            slots[entry.slot].state = "free";
            freeList.push(entry.slot);
            moved.push(entry.slot);
            return false;
        });
        slots.forEach(function (s) { if (frame >= s.freed + IN_FLIGHT) s.wrong = false; });
        if (moved.length) message = { text: `Frame ${frame} starts: the GPU has finished frame ${finished}. Slot ${moved.join(", ")} was freed in a frame that is done, so it moves to the free list.`, cls: "ok" };
        else if (retireList.length) message = { text: `Frame ${frame} starts: the GPU has finished frame ${finished}. The slots in the retire list were freed later than that, so they keep waiting.`, cls: "" };
        else message = { text: `Frame ${frame} starts: the GPU has finished frame ${finished}. Nothing waits.`, cls: "" };
    });

    lab.el(svg, "rect", { x: 16, y: 8, width: 210, height: 26, rx: 4, class: "gpu" });
    const gpuText = lab.el(svg, "text", { x: 121, y: 25, class: "mid small" });
    lab.el(svg, "rect", { x: 236, y: 8, width: 210, height: 26, rx: 4, class: "cpu" });
    const cpuText = lab.el(svg, "text", { x: 341, y: 25, class: "mid small" });
    lab.el(svg, "text", { x: 460, y: 25, class: "small", text: "click a slot in use to free its texture" });

    const cells = slots.map(function (s, k) {
        lab.el(svg, "text", { x: slotX(k) + 29, y: 58, class: "mid small t-faint", text: k });
        const g = lab.el(svg, "g");
        const box = lab.el(g, "rect", { x: slotX(k), y: 66, width: 58, height: 40, rx: 4 });
        const text = lab.el(g, "text", { x: slotX(k) + 29, y: 90, class: "mid small" });
        lab.clickable(g, `Free the texture in slot ${k}`, function () {
            if (s.state !== "used") {
                message = { text: `Slot ${k} holds no texture. Only a slot in use can be freed.`, cls: "" };
                return;
            }
            s.freed = frame;
            s.wrong = false;
            if (retire.value) {
                s.state = "retired";
                retireList.push({ slot: k, frame: frame });
                message = { text: `${s.texture} is freed in frame ${frame}. Slot ${k} waits in the retire list: frames ${frame - 1} and ${frame} may still read it.`, cls: "" };
            } else {
                s.state = "free";
                freeList.push(k);
                message = { text: `${s.texture} is freed in frame ${frame}, and slot ${k} goes straight to the free list. Frames ${frame - 1} and ${frame} may still read it. Now add a texture.`, cls: "" };
            }
        });
        return { box: box, text: text };
    });
    const counterArrow = lab.el(svg, "line", { y1: 132, y2: 112, class: "arrow hot", "marker-end": "url(#ah-hot)" });
    const counterText = lab.el(svg, "text", { y: 136, class: "small t-hot" });
    lab.el(svg, "rect", { x: 16, y: 156, width: 424, height: 60, rx: 6, class: "cpu" });
    lab.el(svg, "text", { x: 228, y: 178, class: "mid title", text: "retire list (slot : frame that freed it)" });
    const retireText = lab.el(svg, "text", { x: 228, y: 200, class: "mid mono" });
    lab.el(svg, "rect", { x: 460, y: 156, width: 284, height: 60, rx: 6, class: "cpu" });
    lab.el(svg, "text", { x: 602, y: 178, class: "mid title", text: "free list" });
    const freeText = lab.el(svg, "text", { x: 602, y: 200, class: "mid mono" });

    const CLASSES = { used: "mem", retired: "accent", free: "box", never: "ghost" };
    const LABELS = { retired: "waits", free: "free", never: "never" };

    lab.draw(function () {
        lab.set(gpuText, { text: `GPU: frame ${frame - 1} runs` });
        lab.set(cpuText, { text: `CPU: frame ${frame} records` });
        let used = 0;
        slots.forEach(function (s, k) {
            if (s.state === "used") used += 1;
            lab.set(cells[k].box, { class: s.wrong ? "bad" : CLASSES[s.state] });
            lab.set(cells[k].text, { class: s.state === "never" ? "mid small t-faint" : "mid small", text: s.state === "used" ? s.texture : LABELS[s.state] });
        });
        const cx = counter < CAPACITY ? slotX(counter) + 29 : slotX(CAPACITY - 1) + 62;
        lab.set(counterArrow, { x1: cx, x2: cx, class: counter < CAPACITY ? "arrow hot" : "hide" });
        lab.set(counterText, {
            x: counter < 5 ? cx + 8 : (counter < CAPACITY ? cx - 8 : 744),
            class: counter < 5 ? "small t-hot" : "small t-hot end",
            text: counter < CAPACITY ? `counter = ${counter}: the first slot never used` : `counter = ${CAPACITY}: every slot was used at least once`,
        });
        const waiting = retireList.slice(0, 6).map(function (entry) { return `${entry.slot} : ${entry.frame}`; });
        if (retireList.length > 6) waiting.push("…");
        lab.set(retireText, { text: retire.value || retireList.length ? `[ ${waiting.join(",  ")} ]` : "not used" });
        lab.set(freeText, { text: `[ ${freeList.join(", ")} ]` });

        usedOut.set(`${used} / ${CAPACITY}`);
        waitOut.set(String(retireList.length));
        wrongOut.set(String(wrongs), wrongs ? "bad" : "ok");
        note.set(message.text, message.cls);
    });
});
