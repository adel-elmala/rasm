// Labs of docs/guides/generational-handles.html.

// A table of slots with a free list (a stack), and every handle that was ever given out. Each handle is looked up
// all the time, with or without the generation compare.
RasmLab.register("slot-reuse", function (lab) {
    const svg = lab.svg(760, 246, "A table of up to six slots, its free list, and the handles given out so far. Each handle shows what a lookup returns now.");
    const MAX_SLOTS = 6;
    const MAX_CHIPS = 8;
    const SLOT_X = 130;
    const SLOT_W = 96;
    const SLOT_STEP = 102;
    const CHIP_STEP = 90;

    const slots = [];        // { generation, object (a name or null), nextFree }
    const handles = [];      // { name, index, generation }
    let firstFree = -1;
    let made = 0;
    let last = "Start: three objects, three handles. Each handle copies its slot's generation.";

    function create() {
        if (firstFree < 0 && slots.length === MAX_SLOTS) {
            last = "The table of this picture is full (6 slots). Destroy an object first.";
            return;
        }
        const name = String.fromCharCode(65 + made % 26);
        made += 1;
        let index = firstFree;
        if (index >= 0) {
            firstFree = slots[index].nextFree;
            last = `create ${name}: slot ${index} comes off the free list and keeps its generation ${slots[index].generation}.`;
        } else {
            index = slots.length;
            slots.push({ generation: 1, object: null, nextFree: -1 });
            last = `create ${name}: the free list is empty, so the table grows by slot ${index}.`;
        }
        slots[index].object = name;
        handles.push({ name: name, index: index, generation: slots[index].generation });
        if (handles.length > MAX_CHIPS) handles.shift();
    }

    function destroy(index) {
        const slot = slots[index];
        if (!slot || slot.object === null) return;
        last = `destroy ${slot.object}: slot ${index} goes from generation ${slot.generation} to ${slot.generation + 1} and joins the free list.`;
        slot.object = null;
        slot.generation += 1;
        slot.nextFree = firstFree;
        firstFree = index;
    }

    create();
    create();
    create();
    last = "Start: three objects, three handles. Each handle copies its slot's generation.";

    lab.button("Create an object", create);
    const check = lab.toggle({ label: "Lookup compares the generation", value: true });
    const slotsOut = lab.readout("slots in the table");
    const staleOut = lab.readout("handles of destroyed objects");
    const wrongOut = lab.readout("lookups that return the wrong thing");
    const note = lab.note();

    lab.el(svg, "text", { x: 20, y: 52, class: "title", text: "slots" });
    lab.el(svg, "text", { x: 20, y: 92, class: "small", text: "index" });
    lab.el(svg, "text", { x: 20, y: 124, class: "title", text: "free list" });
    lab.el(svg, "text", { x: 20, y: 158, class: "title", text: "handles" });
    lab.el(svg, "text", { x: 130, y: 158, class: "small", text: "{index, generation}, and what a lookup returns now" });
    const freeText = lab.el(svg, "text", { x: 130, y: 124, class: "mono" });
    const lastText = lab.el(svg, "text", { x: 20, y: 236, class: "small" });

    const slotParts = [];
    for (let i = 0; i < MAX_SLOTS; i++) {
        const x = SLOT_X + i * SLOT_STEP;
        const rect = lab.el(svg, "rect", { x: x, y: 20, width: SLOT_W, height: 54, rx: 4 });
        lab.clickable(rect, `Destroy the object in slot ${i}`, function () { destroy(i); });
        slotParts.push({
            rect: rect,
            name: lab.el(svg, "text", { x: x + SLOT_W / 2, y: 44 }),
            gen: lab.el(svg, "text", { x: x + SLOT_W / 2, y: 64, class: "mid small" }),
            index: lab.el(svg, "text", { x: x + SLOT_W / 2, y: 92, class: "mid small", text: String(i) }),
        });
    }
    const chipParts = [];
    for (let k = 0; k < MAX_CHIPS; k++) {
        const x = 20 + k * CHIP_STEP;
        chipParts.push({
            rect: lab.el(svg, "rect", { x: x, y: 168, width: 84, height: 26, rx: 4 }),
            text: lab.el(svg, "text", { x: x + 42, y: 186, class: "mid mono" }),
            result: lab.el(svg, "text", { x: x + 42, y: 212 }),
        });
    }

    lab.draw(function () {
        slotParts.forEach(function (part, i) {
            const slot = slots[i];
            if (!slot) {
                lab.set(part.rect, { class: "hide" });
                lab.set(part.name, { class: "hide" });
                lab.set(part.gen, { class: "hide" });
                lab.set(part.index, { class: "hide" });
                return;
            }
            const live = slot.object !== null;
            lab.set(part.rect, { class: live ? "mem pick" : "ghost" });
            lab.set(part.name, { class: live ? "mid title" : "mid t-faint", text: live ? slot.object : "free" });
            lab.set(part.gen, { class: "mid small", text: `gen ${slot.generation}` });
            lab.set(part.index, { class: "mid small" });
        });

        let chain = "";
        for (let i = firstFree; i >= 0; i = slots[i].nextFree) chain += `${i} → `;
        lab.set(freeText, {
            class: chain ? "mono" : "small t-faint",
            text: chain ? `first free = ${chain}end` : "empty: the next create adds a slot at the end",
        });
        lab.set(lastText, { text: last });

        let stale = 0;
        let wrong = 0;
        let wrongText = "";
        chipParts.forEach(function (part, k) {
            const h = handles[k];
            if (!h) {
                lab.set(part.rect, { class: "hide" });
                lab.set(part.text, { text: "" });
                lab.set(part.result, { text: "" });
                return;
            }
            const slot = slots[h.index];
            const match = slot.generation === h.generation;
            if (!match) stale += 1;
            let kind = "ok";
            let result = `→ ${h.name}: ${h.generation} = ${slot.generation}`;
            if (!check.value) {
                result = `→ ${h.name}`;
                if (!match) {
                    kind = "bad";
                    wrong += 1;
                    result = slot.object === null ? "→ freed memory" : `→ ${slot.object}, not ${h.name}`;
                    if (slot.object !== null) wrongText = `The handle of ${h.name} returns ${slot.object}: the code changes the wrong object, and nothing fails.`;
                    else if (!wrongText) wrongText = `The handle of ${h.name} reads slot ${h.index}, which holds no object: a use after free.`;
                }
            } else if (!match) {
                kind = "ghost";
                result = `→ null: ${h.generation} ≠ ${slot.generation}`;
            }
            lab.set(part.rect, { class: kind });
            lab.set(part.text, { text: `${h.name} = {${h.index}, ${h.generation}}` });
            lab.set(part.result, { class: `mid small ${kind === "bad" ? "t-bad" : kind === "ok" ? "t-ok" : "t-faint"}`, text: result });
        });

        slotsOut.set(String(slots.length));
        staleOut.set(String(stale));
        wrongOut.set(String(wrong), wrong ? "bad" : "ok");
        if (wrong) note.set(`Without the compare, a lookup is only slots[index]. ${wrongText}`, "bad");
        else if (stale === 1) note.set("One handle is stale: its generation is not the slot's generation, so the lookup returns null. Nobody had to find the handle.", "ok");
        else if (stale) note.set(`${stale} handles are stale: each has a generation that is not its slot's generation, so its lookup returns null. Nobody had to find them.`, "ok");
        else note.set("Every handle matches its slot, so every lookup returns its object.", "ok");
    });
});
