// Labs of docs/guides/transparency.html.

// Three see-through panes on one view ray, seen from the side. The panes are mixed into the background with the
// over operator, in the list's order or sorted back to front, and the result is compared with the sorted one.
RasmLab.register("blend-order", function (lab) {
    const svg = lab.svg(760, 250, "A side view: an eye on the left looks right through three see-through panes, red, green and blue, at the depths you drag them to. On the right, the color the eye sees when they are mixed in the chosen order, and the right color.");
    const order = lab.choice({ label: "Draw the panes", options: [{ value: "list", label: "in list order" }, { value: "sorted", label: "sorted, back to front" }], value: "list" });
    const orderOut = lab.readout("draw order");
    const errorOut = lab.readout("difference from the right color");
    const note = lab.note();

    const EYE_X = 40;
    const NEAR = 90;
    const FAR = 440;
    const Y = 120;
    const BACKGROUND = [0.85, 0.85, 0.85];
    const panes = [
        { name: "red", color: [0.9, 0.15, 0.1], alpha: 0.5, x: 150 },
        { name: "green", color: [0.15, 0.75, 0.2], alpha: 0.5, x: 260 },
        { name: "blue", color: [0.1, 0.25, 0.9], alpha: 0.5, x: 370 },
    ];

    lab.el(svg, "text", { x: 20, y: 24, class: "title", text: "seen from the side" });
    lab.el(svg, "circle", { cx: EYE_X, cy: Y, r: 10, class: "box" });
    lab.el(svg, "text", { x: EYE_X, y: Y + 30, class: "mid small", text: "eye" });
    lab.el(svg, "line", { x1: EYE_X + 10, y1: Y, x2: FAR + 30, y2: Y, class: "arrow light dashed" });
    lab.el(svg, "rect", { x: FAR + 30, y: 40, width: 18, height: 160, class: "solid" });
    lab.el(svg, "text", { x: FAR + 39, y: 222, class: "mid small", text: "background" });

    const paneEls = panes.map(function (pane, i) {
        const rect = lab.el(svg, "rect", { y: 50, width: 12, height: 140, fill: rgb(pane.color), "fill-opacity": 0.75 });
        const label = lab.el(svg, "text", { y: 214, class: "mid small" });
        const index = lab.el(svg, "text", { y: 44, class: "mid small t-faint" });
        const handle = lab.handle(svg, { x: pane.x, y: Y + 40 + i * 16, label: `Move the ${pane.name} pane`, minX: NEAR, maxX: FAR, minY: Y + 40 + i * 16, maxY: Y + 40 + i * 16 });
        return { rect: rect, label: label, index: index, handle: handle };
    });

    lab.el(svg, "text", { x: 540, y: 60, class: "title", text: "the eye sees" });
    const seen = lab.el(svg, "rect", { x: 540, y: 70, width: 90, height: 60, rx: 4 });
    lab.el(svg, "text", { x: 650, y: 60, class: "title", text: "right" });
    const right = lab.el(svg, "rect", { x: 650, y: 70, width: 90, height: 60, rx: 4 });
    const verdict = lab.el(svg, "text", { x: 540, y: 156, class: "small" });
    lab.el(svg, "text", { x: 540, y: 184, class: "small", text: "result = C · a + B · (1 − a)," });
    lab.el(svg, "text", { x: 540, y: 200, class: "small", text: "one pane at a time, a = 0.5" });

    function rgb(c) {
        const to = function (v) { return Math.round(255 * Math.pow(lab.clamp(v, 0, 1), 1 / 2.2)); };
        return `rgb(${to(c[0])}, ${to(c[1])}, ${to(c[2])})`;
    }
    function mix(sequence) {
        let c = BACKGROUND.slice();
        sequence.forEach(function (pane) {
            for (let k = 0; k < 3; k++) c[k] = pane.color[k] * pane.alpha + c[k] * (1 - pane.alpha);
        });
        return c;
    }
    function backToFront(list) {
        return list.slice().sort(function (a, b) { return b.x - a.x; });
    }

    lab.draw(function () {
        panes.forEach(function (pane, i) {
            pane.x = paneEls[i].handle.x;
            lab.set(paneEls[i].rect, { x: pane.x - 6 });
            lab.set(paneEls[i].label, { x: pane.x, text: pane.name });
        });
        const drawn = order.value === "sorted" ? backToFront(panes) : panes.slice();
        drawn.forEach(function (pane, k) {
            const i = panes.indexOf(pane);
            lab.set(paneEls[i].index, { x: pane.x, text: `#${k + 1}` });
        });
        const got = mix(drawn);
        const want = mix(backToFront(panes));
        const error = Math.max(Math.abs(got[0] - want[0]), Math.abs(got[1] - want[1]), Math.abs(got[2] - want[2]));
        lab.set(seen, { fill: rgb(got) });
        lab.set(right, { fill: rgb(want) });
        orderOut.set(drawn.map(function (p) { return p.name; }).join(", "));
        const ok = error < 0.005;
        errorOut.set(`${Math.round(error * 100)} %`, ok ? "ok" : "bad");
        lab.set(verdict, { text: ok ? "the same: the order is back to front" : "different: a nearer pane was mixed in earlier", class: ok ? "small t-ok" : "small t-bad" });
        note.set(ok ? "Each pane was mixed over everything behind it, so the result is right." : "A pane was mixed before a pane behind it, so the far pane covers the near one.", ok ? "ok" : "bad");
    });
});
