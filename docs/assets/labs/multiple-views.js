// Labs of docs/guides/multiple-views.html.

// The pixels a rect covers: each edge rounded and kept inside the image, at least 1 x 1 (getViewPixels in RASM).
function viewPixels(rect, width, height) {
    function edge(fraction, size) {
        if (!(fraction > 0)) return 0;
        return Math.min(Math.round(fraction * size), size);
    }
    const x0 = Math.min(edge(rect.x, width), width - 1);
    const y0 = Math.min(edge(rect.y, height), height - 1);
    const x1 = Math.max(edge(rect.x + rect.width, width), x0 + 1);
    const y1 = Math.max(edge(rect.y + rect.height, height), y0 + 1);
    return { x: x0, y: y0, width: x1 - x0, height: y1 - y0 };
}

// A window split at a fraction, and a map in its top-right corner: the rects in pixels, from the rounded edges.
RasmLab.register("split-window", function (lab) {
    const svg = lab.svg(760, 330, "A window split in a left and a right view at a fraction you choose, with an optional map in the top-right corner. Each rect's edges are rounded to pixels; the two halves share their edge.");
    const split = lab.slider({ label: "Split at", min: 0.2, max: 0.8, step: 0.001, value: 0.5, format: function (v) { return v.toFixed(3); } });
    const width = lab.choice({ label: "Window width", options: [{ value: 1280, label: "1280" }, { value: 1281, label: "1281" }, { value: 7, label: "7" }], value: 1281 });
    const map = lab.toggle({ label: "Map in the corner", value: true });
    const leftOut = lab.readout("left width, px");
    const rightOut = lab.readout("right width, px");
    const gapOut = lab.readout("gap between them, px");
    const note = lab.note();

    const W = { x: 20, y: 20, w: 720, h: 260 };
    lab.el(svg, "rect", { x: W.x, y: W.y, width: W.w, height: W.h, class: "paper" });
    const left = lab.el(svg, "rect", { class: "cpu o-50" });
    const right = lab.el(svg, "rect", { class: "gpu o-50" });
    const corner = lab.el(svg, "rect", { class: "mem" });
    const leftText = lab.el(svg, "text", { class: "mid small" });
    const rightText = lab.el(svg, "text", { class: "mid small" });
    const cornerText = lab.el(svg, "text", { class: "mid small" });
    const edgeText = lab.el(svg, "text", { class: "mid small mono t-hot" });

    lab.draw(function () {
        const pixelsWide = width.value;
        const pixelsHigh = pixelsWide === 7 ? 4 : 720;
        const s = split.value;
        const a = viewPixels({ x: 0, y: 0, width: s, height: 1 }, pixelsWide, pixelsHigh);
        const b = viewPixels({ x: s, y: 0, width: 1 - s, height: 1 }, pixelsWide, pixelsHigh);
        const m = viewPixels({ x: 0.75, y: 0.02, width: 0.24, height: 0.3 }, pixelsWide, pixelsHigh);
        const sx = W.w / pixelsWide;
        const sy = W.h / pixelsHigh;
        lab.set(left, { x: W.x + a.x * sx, y: W.y + a.y * sy, width: a.width * sx, height: a.height * sy });
        lab.set(right, { x: W.x + b.x * sx, y: W.y + b.y * sy, width: b.width * sx, height: b.height * sy });
        lab.set(corner, { x: W.x + m.x * sx, y: W.y + m.y * sy, width: m.width * sx, height: m.height * sy, class: map.value ? "mem" : "ghost" });
        lab.set(leftText, { x: W.x + (a.x + a.width / 2) * sx, y: W.y + W.h / 2 + 30, text: `left: ${a.width} × ${a.height}` });
        lab.set(rightText, { x: W.x + (b.x + b.width / 2) * sx, y: W.y + W.h / 2 + 30, text: `right: ${b.width} × ${b.height}` });
        lab.set(cornerText, { x: W.x + (m.x + m.width / 2) * sx, y: W.y + (m.y + m.height / 2) * sy + 4, text: map.value ? `map: ${m.width} × ${m.height}` : "" });
        lab.set(edgeText, { x: W.x + b.x * sx, y: W.y + W.h + 22, text: `shared edge at x = ${b.x}` });

        const gap = b.x - (a.x + a.width);
        leftOut.set(String(a.width), "cpu");
        rightOut.set(String(b.width), "gpu");
        gapOut.set(String(gap), gap === 0 ? "ok" : "bad");
        if (pixelsWide === 7)
            note.set(map.value ? `A tiny window: every rect keeps at least one pixel. The map is ${m.width} × ${m.height}.` : "A tiny window: the halves still share their edge and keep at least one pixel each.", "ok");
        else if (a.width !== b.width)
            note.set(`The halves differ by ${Math.abs(a.width - b.width)} pixel: the edge is rounded once and shared, so no column is left out.`, "ok");
        else
            note.set("Both halves have the same width, and they meet at one edge with no gap.", "ok");
    });
});

// A list of views, one of them a view texture that the scene's TV shows. A view after the texture view in the list
// reads this frame's picture; the views before it, and the texture view itself, read last frame's.
RasmLab.register("order-views", function (lab) {
    const svg = lab.svg(760, 220, "A list of four views in draw order. The texture view writes the TV's picture. Each view that sees the TV is marked with the frame it sees: this frame when it comes after the texture view, the last frame when it comes before, or when it is the texture view itself.");
    const place = lab.choice({ label: "Texture view at place", options: [{ value: 0, label: "1" }, { value: 1, label: "2" }, { value: 2, label: "3" }, { value: 3, label: "4" }], value: 0 });
    const thisOut = lab.readout("views with this frame's picture");
    const lastOut = lab.readout("views with last frame's");
    const note = lab.note();

    const OTHERS = ["main", "side", "map"];
    const boxes = [];
    for (let i = 0; i < 4; ++i) {
        const x = 20 + i * 185;
        const g = lab.el(svg, "g");
        boxes.push({
            rect: lab.el(g, "rect", { x: x, y: 50, width: 165, height: 64, rx: 6 }),
            name: lab.el(g, "text", { x: x + 82, y: 76, class: "mid" }),
            sees: lab.el(g, "text", { x: x + 82, y: 98, class: "mid small" }),
            place: lab.el(g, "text", { x: x + 82, y: 38, class: "mid small t-faint", text: `place ${i + 1}` }),
        });
        if (i < 3)
            lab.el(svg, "line", { x1: x + 165, y1: 82, x2: x + 183, y2: 82, class: "arrow gpu", "marker-end": "url(#ah-gpu)" });
    }
    lab.el(svg, "text", { x: 20, y: 178, class: "small", text: "Passes run in list order. The texture view's tone map writes the TV's picture." });
    lab.el(svg, "text", { x: 20, y: 200, class: "small", text: "A view after it reads the new picture; a view before it reads what the last frame wrote." });
    const marker = lab.el(svg, "text", { class: "mid small t-hot", y: 136 });

    lab.draw(function () {
        const texturePlace = place.value;
        let other = 0;
        let fresh = 0;
        let old = 0;
        for (let i = 0; i < 4; ++i) {
            const box = boxes[i];
            if (i === texturePlace) {
                lab.set(box.rect, { class: "mem" });
                lab.set(box.name, { text: "security → T" });
                lab.set(box.sees, { text: "its TV: last frame", class: "mid small t-hot" });
                ++old;
                lab.set(marker, { x: 20 + i * 185 + 82, text: "writes T" });
                continue;
            }
            const after = i > texturePlace;
            lab.set(box.rect, { class: after ? "ok o-50" : "box" });
            lab.set(box.name, { text: OTHERS[other++] });
            lab.set(box.sees, { text: after ? "its TV: this frame" : "its TV: last frame", class: after ? "mid small t-ok" : "mid small t-hot" });
            if (after) ++fresh; else ++old;
        }
        thisOut.set(String(fresh), fresh === 3 ? "ok" : "");
        lastOut.set(String(old), old > 1 ? "bad" : "");
        if (texturePlace === 0)
            note.set("The texture view comes first: every other view shows this frame's picture. Only the security camera's own view of the TV is one frame old, which breaks the cycle.", "ok");
        else
            note.set(`${texturePlace} view(s) come before the texture view and show the last frame's picture. Put the texture view first so every other view shows this frame's.`, "bad");
    });
});
