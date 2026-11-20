// Labs of docs/guides/load-store-ops.html.

// One color image through one pass on a tile-based GPU: what is in memory before, what the pass starts with,
// what it ends with, and what memory holds afterwards, with the bytes the two ops move at 1920 x 1080 RGBA8.
RasmLab.register("ops-pass", function (lab) {
    const svg = lab.svg(760, 180, "Four pictures of one color image: in memory before the pass, in tile memory when the pass starts, in tile memory when it ends, and in memory after the pass. The load op and the store op decide what moves between them.");
    const load = lab.choice({ label: "Load op", options: ["LOAD", "CLEAR", "DONT_CARE"], value: "CLEAR" });
    const store = lab.choice({ label: "Store op", options: ["STORE", "DONT_CARE"], value: "STORE" });
    const cover = lab.choice({ label: "The pass draws", options: [{ value: "part", label: "some pixels" }, { value: "all", label: "every pixel" }], value: "part" });
    const readOut = lab.readout("read from memory");
    const writeOut = lab.readout("written to memory");
    const rateOut = lab.readout("memory traffic at 60 fps");
    const note = lab.note();

    const BYTES = 1920 * 1080 * 4;      // one RGBA8 image
    const W = 140;
    const H = 96;
    const TOP = 52;
    const PANELS = [
        { x: 10, title: "in memory, before", chip: false },
        { x: 210, title: "the pass starts with", chip: true },
        { x: 410, title: "the pass ends with", chip: true },
        { x: 610, title: "in memory, after", chip: false },
    ];

    // the same garbage every time: a small repeatable random generator
    let seed = 11;
    function random() {
        seed = (seed * 1664525 + 1013904223) % 4294967296;
        return seed / 4294967296;
    }
    const NOISE = [];
    for (let k = 0; k < 26; k++) NOISE.push({ x: 4 + random() * (W - 34), y: 4 + random() * (H - 22), w: 8 + random() * 22, h: 4 + random() * 12, cls: random() < 0.3 ? "bad o-50" : (random() < 0.5 ? "shade" : "solid o-25") });

    lab.el(svg, "text", { x: 80, y: 18, class: "mid small t-mem", text: "VRAM" });
    lab.el(svg, "text", { x: 380, y: 18, class: "mid small t-gpu", text: "on chip: tile memory" });
    lab.el(svg, "text", { x: 680, y: 18, class: "mid small t-mem", text: "VRAM" });
    lab.el(svg, "path", { d: "M210,24H550", class: "arrow gpu" });
    PANELS.forEach(function (p) {
        lab.el(svg, "text", { x: p.x + W / 2, y: 42, class: "mid small", text: p.title });
    });
    const frames = PANELS.map(function (p) { return lab.el(svg, "g"); });
    const links = lab.el(svg, "g");

    // what a panel can hold
    function old(g, x) {
        lab.el(g, "polygon", { points: `${x + 14},${TOP + 82} ${x + 52},${TOP + 26} ${x + 86},${TOP + 82}`, class: "solid o-50" });
        lab.el(g, "circle", { cx: x + 108, cy: TOP + 28, r: 13, class: "solid o-50" });
    }
    function garbage(g, x) {
        NOISE.forEach(function (n) { lab.el(g, "rect", { x: x + n.x, y: TOP + n.y, width: n.w, height: n.h, class: n.cls }); });
    }
    function drawn(g, x, all) {
        if (all) lab.el(g, "rect", { x: x + 3, y: TOP + 3, width: W - 6, height: H - 6, rx: 3, class: "accent o-50" });
        lab.el(g, "polygon", { points: `${x + 30},${TOP + 86} ${x + 64},${TOP + 20} ${x + 92},${TOP + 62}`, class: "accent" });
        lab.el(g, "polygon", { points: `${x + 76},${TOP + 86} ${x + 104},${TOP + 40} ${x + 130},${TOP + 86}`, class: "accent" });
    }
    function panel(k, kind, startKind, all) {
        const p = PANELS[k];
        const g = frames[k];
        lab.clear(g);
        const undefinedNow = kind === "garbage";
        lab.el(g, "rect", { x: p.x, y: TOP, width: W, height: H, rx: 6, class: undefinedNow ? "ghost" : (p.chip ? "gpu" : "mem") });
        if (kind === "old") old(g, p.x);
        else if (kind === "garbage") garbage(g, p.x);
        else if (kind === "result") {
            if (!all && startKind === "old") old(g, p.x);
            if (!all && startKind === "garbage") garbage(g, p.x);
            drawn(g, p.x, all);
        }
        let caption = "one clear value";
        if (kind === "old") caption = "the old picture";
        else if (kind === "garbage") caption = "undefined";
        else if (kind === "result") {
            if (all) caption = "the new picture";
            else if (startKind === "old") caption = "new, on top of old";
            else if (startKind === "garbage") caption = "new, with garbage";
            else caption = "new, on the clear value";
        }
        const wrong = kind === "garbage" || (kind === "result" && !all && startKind === "garbage");
        lab.el(g, "text", { x: p.x + W / 2, y: TOP + H + 18, class: wrong ? "mid small t-bad" : "mid small", text: caption });
    }
    function link(x, moves, label, bytes) {
        if (moves) lab.el(links, "line", { x1: x + 4, y1: TOP + 48, x2: x + 54, y2: TOP + 48, class: "arrow mem flowing", "marker-end": "url(#ah-mem)" });
        lab.el(links, "text", { x: x + 30, y: TOP + 36, class: moves ? "mid small t-mem" : "mid small t-gpu", text: label });
        lab.el(links, "text", { x: x + 30, y: TOP + 68, class: moves ? "mid small t-mem" : "mid small t-faint", text: bytes });
    }

    lab.draw(function () {
        const all = cover.value === "all";
        const startKind = load.value === "LOAD" ? "old" : (load.value === "CLEAR" ? "clear" : "garbage");
        const stored = store.value === "STORE";
        const read = load.value === "LOAD" ? BYTES : 0;
        const written = stored ? BYTES : 0;

        panel(0, "old", "", false);
        panel(1, startKind, "", false);
        panel(2, "result", startKind, all);
        if (stored) panel(3, "result", startKind, all);
        else panel(3, "garbage", "", false);

        lab.clear(links);
        link(150, read > 0, read > 0 ? "copy in" : "no read", read > 0 ? "8.3 MB" : "0 bytes");
        lab.el(links, "line", { x1: 354, y1: TOP + 48, x2: 404, y2: TOP + 48, class: "arrow gpu", "marker-end": "url(#ah-gpu)" });
        lab.el(links, "text", { x: 380, y: TOP + 36, class: "mid small t-gpu", text: "draw" });
        link(550, stored, stored ? "copy out" : "no write", stored ? "8.3 MB" : "0 bytes");

        readOut.set(`${(read / 1e6).toFixed(1)} MB`, read ? "mem" : "gpu");
        writeOut.set(`${(written / 1e6).toFixed(1)} MB`, written ? "mem" : "gpu");
        rateOut.set(`${((read + written) * 60 / 1e9).toFixed(2)} GB/s`);

        const garbageShows = load.value === "DONT_CARE" && !all;
        if (!stored) note.set("The picture is lost: after the pass the image in memory is undefined. That is right only when nothing reads it later, like depth after the last 3D pass.", garbageShows ? "bad" : "");
        else if (garbageShows) note.set("Wrong: the pixels the pass does not draw keep undefined contents, and STORE writes them to memory.", "bad");
        else if (load.value === "LOAD" && all) note.set("It works, but the 8.3 MB read is wasted: the pass overwrites every pixel it loaded. DONT_CARE gives the same image.", "bad");
        else if (load.value === "LOAD") note.set("Right when the pass draws on top of an earlier pass. The price is one read of the whole image.", "ok");
        else if (load.value === "CLEAR" && all) note.set("Right, and free. The clear value is never seen, so DONT_CARE works too.", "ok");
        else if (load.value === "CLEAR") note.set("Right for the first pass on the image: the pixels it does not draw hold the clear value, and nothing is read.", "ok");
        else note.set("Right, and the cheapest: the pass writes every pixel, so the undefined start is never seen.", "ok");
    });
});
