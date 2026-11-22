// Labs of docs/guides/texture-uploads.html.

// The host-copy rule for one format on a few kinds of GPU. Each device lists its memory types (heap, flags) and the
// types an image may use when it is filled by a copy and when it is made for host copy. Host copy is used only when
// the device has the feature, the format keeps its best layout (optimalDeviceAccess), and a device-local type is in
// both sets: the same rule as the page's checklist.
RasmLab.register("host-copy-rule", function (lab) {
    const DEVICES = {
        bar: {
            label: "discrete, resizable BAR",
            feature: true,
            heaps: ["video memory, 12 GiB", "system RAM, 16 GiB"],
            types: [
                { heap: 1, local: false, host: false },
                { heap: 0, local: true, host: false },
                { heap: 0, local: true, host: false },
                { heap: 1, local: false, host: true },
                { heap: 1, local: false, host: true },
                { heap: 0, local: true, host: true },
            ],
            copy: [1, 2],
            hostCopy: [3, 4, 5],
        },
        smallBar: {
            label: "discrete, 256 MiB BAR",
            feature: true,
            heaps: ["video memory, 12 GiB", "system RAM, 16 GiB", "BAR, 256 MiB"],
            types: [
                { heap: 1, local: false, host: false },
                { heap: 0, local: true, host: false },
                { heap: 1, local: false, host: true },
                { heap: 2, local: true, host: true },
            ],
            copy: [1],
            hostCopy: [2, 3],
        },
        same: {
            label: "discrete, same types",
            feature: true,
            heaps: ["video memory, 8 GiB", "system RAM, 16 GiB"],
            types: [
                { heap: 0, local: true, host: false },
                { heap: 1, local: false, host: true },
                { heap: 0, local: true, host: true },
            ],
            copy: [0, 1, 2],
            hostCopy: [0, 1, 2],
        },
        unified: {
            label: "unified memory",
            feature: true,
            heaps: ["one memory for CPU and GPU"],
            types: [{ heap: 0, local: true, host: true }],
            copy: [0],
            hostCopy: [0],
        },
        none: {
            label: "no host image copy",
            feature: false,
            heaps: ["video memory, 4 GiB", "system RAM, 8 GiB"],
            types: [
                { heap: 0, local: true, host: false },
                { heap: 1, local: false, host: true },
            ],
            copy: [0],
            hostCopy: [],
        },
    };

    const svg = lab.svg(760, 250, "The device's memory types, grouped by heap. Green bars mark the types an image filled by a copy may use, red bars the types an image made for host copy may use. A type in both, in video memory, can hold a host-copied texture.");
    const layer = lab.el(svg, "g", {});

    const device = lab.choice({
        label: "GPU",
        options: Object.keys(DEVICES).map(function (key) { return { value: key, label: DEVICES[key].label }; }),
        value: "bar",
    });
    const layout = lab.toggle({ label: "the format keeps its best layout", value: true });
    const pathOut = lab.readout("path");
    const typesOut = lab.readout("host-copy types");
    const note = lab.note();

    lab.draw(function () {
        lab.clear(layer);
        const d = DEVICES[device.value];
        const heapCount = d.heaps.length;
        const heapWidth = (720 - (heapCount - 1) * 16) / heapCount;
        const shared = d.copy.filter(function (t) { return d.hostCopy.indexOf(t) >= 0 && d.types[t].local; });

        d.heaps.forEach(function (name, h) {
            const x = 20 + h * (heapWidth + 16);
            const inHeap = [];
            d.types.forEach(function (type, t) { if (type.heap === h) inHeap.push(t); });
            lab.el(layer, "rect", { x: x, y: 30, width: heapWidth, height: 150, rx: 8, class: "zone" });
            lab.el(layer, "text", { x: x + 10, y: 22, class: "small title", text: "heap " + h + " · " + name });
            const cell = Math.min(110, (heapWidth - 16) / Math.max(1, inHeap.length) - 8);
            inHeap.forEach(function (t, i) {
                const type = d.types[t];
                const cx = x + 10 + i * (cell + 8);
                const cls = type.local ? "gpu" : type.host ? "cpu" : "paper";
                lab.el(layer, "rect", { x: cx, y: 44, width: cell, height: 60, rx: 5, class: cls });
                if (shared.indexOf(t) >= 0 && layout.value && d.feature)
                    lab.el(layer, "rect", { x: cx, y: 44, width: cell, height: 60, rx: 5, class: "hot" });
                lab.el(layer, "text", { x: cx + cell / 2, y: 66, class: "mid title", text: "type " + t });
                const flags = (type.local ? "local" : "") + (type.local && type.host ? " + " : "") + (type.host ? "host" : "");
                lab.el(layer, "text", { x: cx + cell / 2, y: 86, class: "mid small", text: flags || "none" });
                if (d.copy.indexOf(t) >= 0)
                    lab.el(layer, "rect", { x: cx, y: 116, width: cell, height: 10, rx: 3, class: "ok" });
                if (d.hostCopy.indexOf(t) >= 0)
                    lab.el(layer, "rect", { x: cx, y: 134, width: cell, height: 10, rx: 3, class: "bad" });
            });
        });
        lab.el(layer, "rect", { x: 20, y: 200, width: 14, height: 10, rx: 3, class: "ok" });
        lab.el(layer, "text", { x: 40, y: 209, class: "small", text: "an image filled by a copy may use it" });
        lab.el(layer, "rect", { x: 330, y: 200, width: 14, height: 10, rx: 3, class: "bad" });
        lab.el(layer, "text", { x: 350, y: 209, class: "small", text: "an image made for host copy may use it" });
        lab.el(layer, "text", { x: 20, y: 236, class: "small t-faint", text: "outlined: a type that can hold a host-copied texture" });

        const host = d.feature && layout.value && shared.length > 0;
        pathOut.set(host ? "host copy" : "staging", host ? "gpu" : "cpu");
        typesOut.set(host ? shared.join(", ") : "none", host ? "ok" : "bad");

        if (!d.feature)
            note.set("Staging: the device has no host image copy. It still starts and draws; every texture comes through the staging buffer.", "ok");
        else if (!layout.value)
            note.set("Staging: a host-copied image of this format would be stored in a layout the GPU reads more slowly, for the texture's whole life.", "ok");
        else if (host)
            note.set("Host copy: type " + shared.join(" and ") + " is video memory that ordinary textures use too, so the texture is no different once it is filled.", "ok");
        else if (d.hostCopy.some(function (t) { return d.types[t].local; }))
            note.set("Staging: host-copied images could only go to memory ordinary textures never use (the BAR window). On a real GPU of this kind that stalled the GPU while textures loaded.", "bad");
        else
            note.set("Staging: host-copied images could only live in system RAM, so every read would cross the bus.", "bad");
    });
});
