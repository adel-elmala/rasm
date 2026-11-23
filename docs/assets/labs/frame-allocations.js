// Labs of docs/guides/frame-allocations.html.

// A list is filled with the same number of 16-byte items in each of 10 frames, by adding one item at a time, and then
// handed on: moved into a second list (which leaves the first with no buffer), swapped with the second list and
// cleared, or only cleared. Growth follows the standard libraries: Microsoft's grows to old + old / 2, libstdc++ and
// libc++ to twice the old capacity, and both to at least the size asked for. Each growth is one allocation.
RasmLab.register("vector-growth", function (lab) {
    const svg = lab.svg(760, 300, "Top: the allocations of each of 10 frames as bars. Bottom: the buffers one frame's growth allocates, to scale.");
    const items = lab.slider({ label: "Items per frame", min: 1, max: 8000, step: 1, value: 5000 });
    const policy = lab.choice({ label: "Handed on by", options: [{ value: "move", label: "moved away" }, { value: "swap", label: "swap + clear" }, { value: "clear", label: "clear" }], value: "move" });
    const growth = lab.choice({ label: "Growth", options: [{ value: "1.5", label: "1.5×" }, { value: "2", label: "2×" }], value: "1.5" });
    const lastOut = lab.readout("allocations in frame 10");
    const bytesOut = lab.readout("bytes allocated in frame 10");
    const totalOut = lab.readout("allocations in 10 frames");
    const note = lab.note();

    const FRAMES = 10;
    const ITEM = 16;
    const X0 = 70;
    const W = 670;
    const BAR_TOP = 56;
    const BAR_BOTTOM = 160;

    lab.el(svg, "text", { x: 10, y: 22, class: "title", text: "Allocations per frame" });
    lab.el(svg, "path", { d: `M${X0},${BAR_BOTTOM}H${X0 + W}`, class: "arrow" });
    lab.el(svg, "text", { x: 10, y: 212, class: "title", text: "The buffers of one growing frame" });
    const bars = lab.el(svg, "g");
    const chain = lab.el(svg, "g");
    const chainText = lab.el(svg, "text", { x: X0, y: 286, class: "small", text: "" });

    function grow(capacity, needed, factor) {
        const geometric = factor === "2" ? capacity * 2 : capacity + Math.floor(capacity / 2);
        return Math.max(geometric, needed);
    }

    // Fills a list from empty to n items: returns the new buffers' capacities.
    function fill(list, n, factor) {
        const made = [];
        for (let size = 0; size < n; size++) {
            if (size === list.capacity) {
                list.capacity = grow(list.capacity, size + 1, factor);
                made.push(list.capacity);
            }
        }
        return made;
    }

    lab.draw(function () {
        const n = items.value;
        let a = { capacity: 0 };
        let b = { capacity: 0 };
        const frames = [];
        for (let f = 0; f < FRAMES; f++) {
            frames.push(fill(a, n, growth.value));
            if (policy.value === "move") {
                b = a;
                a = { capacity: 0 };
            } else if (policy.value === "swap") {
                const t = a;
                a = b;
                b = t;
            }
        }

        const most = Math.max(1, Math.max.apply(null, frames.map(function (f) { return f.length; })));
        const slot = W / FRAMES;
        lab.clear(bars);
        frames.forEach(function (made, f) {
            const h = (BAR_BOTTOM - BAR_TOP) * made.length / most;
            const x = X0 + f * slot + slot * 0.2;
            if (made.length > 0) lab.el(bars, "rect", { x: x, y: BAR_BOTTOM - h, width: slot * 0.6, height: h, rx: 2, class: "hot" });
            lab.el(bars, "text", { x: x + slot * 0.3, y: BAR_BOTTOM - h - 5, class: made.length > 0 ? "mid small" : "mid small t-ok", text: String(made.length) });
            lab.el(bars, "text", { x: x + slot * 0.3, y: BAR_BOTTOM + 16, class: "mid small t-faint", text: `frame ${f + 1}` });
        });

        // The growth of frame 1, each buffer to scale against the sum of them all.
        const first = frames[0];
        const sum = first.reduce(function (s, c) { return s + c; }, 0);
        const gap = 2;
        const room = W - gap * first.length;
        let x = X0;
        lab.clear(chain);
        first.forEach(function (capacity) {
            const w = Math.max(1, room * capacity / sum);
            lab.el(chain, "rect", { x: x, y: 226, width: w, height: 30, class: "mem" });
            if (w > 60) lab.el(chain, "text", { x: x + w / 2, y: 245, class: "mid small", text: `${capacity} items` });
            x += w + gap;
        });
        const finalCapacity = first.length > 0 ? first[first.length - 1] : 0;
        lab.set(chainText, { text: `${first.length} buffers, ${(sum * ITEM / 1000).toFixed(1)} KB in all; the last one holds ${finalCapacity} items, the others are copied and freed.` });

        const last = frames[FRAMES - 1];
        const lastBytes = last.reduce(function (s, c) { return s + c * ITEM; }, 0);
        const total = frames.reduce(function (s, f) { return s + f.length; }, 0);
        lastOut.set(String(last.length), last.length === 0 ? "ok" : "bad");
        bytesOut.set(lastBytes >= 1000 ? `${(lastBytes / 1000).toFixed(1)} KB` : `${lastBytes} B`, last.length === 0 ? "ok" : "bad");
        totalOut.set(String(total));
        if (last.length > 0) note.set(`Every frame grows the list again from empty: ${last.length} allocations, each with a copy of the items so far, and a free of the old buffer.`, "bad");
        else if (policy.value === "swap") note.set("Each of the two lists grows once, in frame 1 or 2. Then both have the capacity, and no frame allocates.", "ok");
        else note.set("The list grows in frame 1 only. Its buffer stays, so no later frame allocates.", "ok");
    });
});
